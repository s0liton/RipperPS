// CDs through a drive with a CD laser: every track read as raw 2352-byte
// frames, cut the way Redump cuts them (a track's INDEX 00 pregap in its own
// file, track 1 from LBA 0), into a folder of .bin files and a .cue, or into
// one CHD.
//
// Frames come with C2 error pointers when the drive gives them; a frame with
// C2 errors, or a data sector whose EDC doesn't match, is read again. Audio is
// shifted by the drive's read offset (AccurateRip's figure) so samples land
// where they are on the disc; data tracks aren't, READ CD aligns them itself.

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "rip_internal.h"
#include "ripper/audio_out.h"
#include "ripper/cdrom.h"
#include "ripper/chd.h"
#include "ripper/common.h"
#include "ripper/cue.h"
#include "ripper/hash.h"
#include "ripper/log.h"
#include "ripper/mmc.h"
#include "ripper/util.h"

// Per READ CD. The PS5's pass-through to a USB drive refuses 62 KiB (E2BIG)
// and takes 55 KiB; 56 KiB keeps 21 frames with C2, 24 without.
#define READ_BYTES 57344u
#define RETRIES 4        // reads of a chunk with errors, at full speed
#define SLOW_RETRIES 6   // reads of a bad frame, slowed down
#define SLOW_KBPS 1411   // 8x
#define FAST_AGAIN 3000  // good frames before going back to full speed
#define REPORT_BYTES (4u << 20)

/* Drive read offsets */

// Samples, as AccurateRip lists them; drives not here read with no shift and
// the manifest says so.
static const struct {
  const char *vendor;
  const char *product;  // prefix
  int offset;
} OFFSETS[] = {
    {"TSSTcorp", "CDDVDW SH-224", 6},   {"TSSTcorp", "CDDVDW SH-S22", 6},   {"ASUS", "DRW-24", 6},
    {"HL-DT-ST", "DVDRAM GH24NS", 667}, {"HL-DT-ST", "DVDRAM GH22NS", 667}, {"PLEXTOR", "DVDR   PX-760A", 30},
    {"PLEXTOR", "DVDR   PX-716A", 30},
};

static int drive_offset(const struct rip_drive *drive, int *known) {
  size_t i;

  *known = 0;
  for (i = 0; drive && i < RIP_ARRAY_LEN(OFFSETS); i++) {
    if (!strcmp(drive->inquiry.vendor, OFFSETS[i].vendor) &&
        !strncmp(drive->inquiry.product, OFFSETS[i].product, strlen(OFFSETS[i].product))) {
      *known = 1;
      return OFFSETS[i].offset;
    }
  }
  return 0;
}

/* Reading */

struct cd_read {
  struct rip_transport *t;
  unsigned flags;  // CD_READ_C2, CD_READ_SUB
  size_t stride;
  uint32_t per_read;   // frames per READ CD
  uint32_t end;        // the disc's lead-out: audio past it reads as silence
  uint8_t *raw;        // per_read * stride
  uint64_t c2_frames;  // frames still bad after every retry
  uint64_t retried;
  int c2_dropped;       // the drive balked at C2 partway through
  uint32_t cap;         // frames per command after the pass-through refused more (E2BIG), 0: no cap
  uint64_t unreadable;  // frames written as zeros
  int slow;             // read speed lowered for a bad patch
  uint32_t good_run;    // frames read cleanly since
  unsigned bad_streak;  // unreadable frames in a row: fewer tries while it lasts
  const volatile int *cancel;
};

static int frame_bad(const struct cd_read *c, const uint8_t *f, int data) {
  if (c->flags & CD_READ_C2) {
    const uint8_t *c2 = f + CD_FRAME;
    size_t i;
    for (i = 0; i < CD_C2; i++) {
      if (c2[i]) {
        return 1;
      }
    }
  }
  return data && cd_sector_ok(f) == 0;
}

// One READ CD of count frames into c->raw. When the command itself fails:
// refused as too big (E2BIG), smaller reads from then on; no answer from the
// drive (a Samsung SH-224DB on a USB bridge once hung on C2 reads until the
// bridge reset), C2 pointers dropped for the rest of the rip, then smaller.
static int read_cmd(struct cd_read *c, uint32_t lba, uint32_t count) {
  struct rip_sense sense;
  uint32_t step = c->cap && c->cap < count ? c->cap : count;
  uint32_t done = 0;
  unsigned fails = 0;

  while (done < count) {
    uint32_t n = count - done < step ? count - done : step;
    memset(&sense, 0, sizeof(sense));
    if (cd_read_frames(c->t, lba + done, n, c->flags, c->raw + (size_t)done * c->stride, &sense) == RIP_OK) {
      done += n;
      fails = 0;
      continue;
    }
    fails++;
    c->retried++;
    RIP_WARN("READ CD at %u (+%u) failed: sense %02x/%02x/%02x, %s", lba + done, n, sense.key, sense.asc, sense.ascq,
             c->t->last_error[0] ? c->t->last_error : "no detail");
    if (sense.key) {
      return RIP_ESENSE;  // the drive answered: a read error, retried by the caller
    }
    if (c->cancel && *c->cancel) {
      return RIP_ECANCEL;
    }
    if (strstr(c->t->last_error, "errno 7)")) {
      // E2BIG: the console's pass-through wouldn't map that much (it takes
      // 15 pages, fewer at times). Once more as it was, then smaller from
      // here on; C2 has nothing to do with it.
      if (fails == 1) {
        continue;
      }
      if (step > 1) {
        step = step > 4 ? step * 3 / 4 : step - 1;
        c->cap = step;
        RIP_WARN("reading %u frames at a time from here", step);
        fails = 0;
        continue;
      }
      return RIP_ERR;
    }
    if (c->flags & CD_READ_C2) {
      // Without C2 the frames are smaller and the drive simpler to please.
      size_t old = c->stride;
      uint32_t i;
      RIP_WARN("reading on without C2 error pointers");
      c->flags &= ~(unsigned)CD_READ_C2;
      c->c2_dropped = 1;
      c->stride = cd_frame_stride(c->flags);
      for (i = 0; i < done; i++) {
        memmove(c->raw + (size_t)i * c->stride, c->raw + (size_t)i * old, c->stride);
      }
      fails = 0;
    } else if (fails >= 2 && step > 1) {
      step = step > 4 ? step / 2 : 1;
      fails = 0;
    } else if (fails >= 3) {
      return RIP_ERR;
    }
  }
  return RIP_OK;
}

// One frame that keeps failing: read alone, slowed down; zeros when the disc
// won't give it up (the rip goes on and says how many).
static int read_one_hard(struct cd_read *c, uint32_t lba, int data, uint8_t *frame, uint8_t *sub) {
  unsigned attempt;
  int rc = RIP_ERR;

  if (!c->slow) {
    RIP_INFO("slowing down for a bad patch at frame %u", lba);
    mmc_set_cd_speed(c->t, SLOW_KBPS);
    c->slow = 1;
  }
  c->good_run = 0;
  // In the middle of a long scratch each read costs the drive seconds:
  // one try each until a frame reads again.
  for (attempt = 0; attempt < (c->bad_streak >= 2 ? 1u : SLOW_RETRIES); attempt++) {
    rc = read_cmd(c, lba, 1);
    if (rc == RIP_ECANCEL || rc == RIP_ERR) {
      return rc;
    }
    if (rc == RIP_OK && !frame_bad(c, c->raw, data)) {
      break;
    }
    c->retried++;
  }
  if (rc == RIP_OK) {
    c->bad_streak = 0;
    if (frame_bad(c, c->raw, data)) {
      c->c2_frames++;  // read, but with errors left in it: kept as read
    }
    memcpy(frame, c->raw, CD_FRAME);
    if (sub) {
      memcpy(sub, c->raw + CD_FRAME + ((c->flags & CD_READ_C2) ? CD_C2 : 0), CD_SUB);
    }
    return RIP_OK;
  }
  RIP_WARN("frame %u can't be read; written as zeros", lba);
  c->unreadable++;
  c->bad_streak++;
  memset(frame, 0, CD_FRAME);
  if (sub) {
    memset(sub, 0, CD_SUB);
  }
  return RIP_OK;
}

// Frames [lba, lba + count) as they are on the disc, with their subchannel
// when asked; read again while any has errors, then frame by frame.
static int read_raw(struct cd_read *c, uint32_t lba, uint32_t count, int data, uint8_t *frames, uint8_t *sub) {
  unsigned attempt;
  uint32_t i;
  uint32_t bad = 0;
  int rc = RIP_ERR;

  for (attempt = 0; attempt < RETRIES; attempt++) {
    if (c->cancel && *c->cancel) {
      return RIP_ECANCEL;
    }
    rc = read_cmd(c, lba, count);
    if (rc == RIP_ECANCEL || rc == RIP_ERR) {
      if (rc == RIP_ERR) {
        RIP_ERROR("the drive stopped answering at frame %u", lba);
      }
      return rc;
    }
    if (rc != RIP_OK) {
      continue;  // a read error: try the chunk again, then frame by frame
    }
    bad = 0;
    for (i = 0; i < count; i++) {
      bad += frame_bad(c, c->raw + (size_t)i * c->stride, data);
    }
    if (!bad) {
      break;
    }
    c->retried++;
  }
  if (rc == RIP_OK && !bad) {
    for (i = 0; i < count; i++) {
      const uint8_t *f = c->raw + (size_t)i * c->stride;
      memcpy(frames + (size_t)i * CD_FRAME, f, CD_FRAME);
      if (sub) {
        memcpy(sub + (size_t)i * CD_SUB, f + CD_FRAME + ((c->flags & CD_READ_C2) ? CD_C2 : 0), CD_SUB);
      }
    }
    c->good_run += count;
    if (c->slow && c->good_run >= FAST_AGAIN) {
      mmc_set_max_speed(c->t, c->end ? c->end - 1 : 0);
      c->slow = 0;
      RIP_INFO("back to full speed at frame %u", lba);
    }
    return RIP_OK;
  }
  // Frame by frame: the good ones as they come, the bad ones the hard way.
  for (i = 0; i < count; i++) {
    uint8_t *frame = frames + (size_t)i * CD_FRAME;
    uint8_t *s = sub ? sub + (size_t)i * CD_SUB : NULL;
    rc = c->bad_streak >= 2 ? RIP_ESENSE : read_cmd(c, lba + i, 1);  // in a scratch: straight to the slow read
    if (rc == RIP_ECANCEL || rc == RIP_ERR) {
      return rc;
    }
    if (rc == RIP_OK && !frame_bad(c, c->raw, data)) {
      c->bad_streak = 0;
      memcpy(frame, c->raw, CD_FRAME);
      if (s) {
        memcpy(s, c->raw + CD_FRAME + ((c->flags & CD_READ_C2) ? CD_C2 : 0), CD_SUB);
      }
      continue;
    }
    rc = read_one_hard(c, lba + i, data, frame, s);
    if (rc != RIP_OK) {
      return rc;
    }
  }
  return RIP_OK;
}

// Audio frames [lba, lba + count) shifted by shift bytes: the frames either
// side are read too, and what lies outside the disc is silence.
static int read_shifted(struct cd_read *c, int64_t lba, uint32_t count, int64_t shift, uint8_t *out, uint8_t *sub,
                        uint8_t *tmp) {
  int64_t first_byte = lba * CD_FRAME + shift;
  int64_t first = first_byte >= 0 ? first_byte / CD_FRAME : -((-first_byte + CD_FRAME - 1) / CD_FRAME);
  int64_t last = (first_byte + (int64_t)count * CD_FRAME + CD_FRAME - 1) / CD_FRAME;
  int64_t lo = first < 0 ? 0 : first;
  int64_t hi = last > (int64_t)c->end ? (int64_t)c->end : last;
  int rc;

  memset(tmp, 0, (size_t)(last - first) * CD_FRAME);
  if (hi > lo) {
    rc = read_raw(c, (uint32_t)lo, (uint32_t)(hi - lo), 0, tmp + (size_t)(lo - first) * CD_FRAME, NULL);
    if (rc != RIP_OK) {
      return rc;
    }
  }
  memcpy(out, tmp + (first_byte - first * CD_FRAME), (size_t)count * CD_FRAME);
  if (sub) {
    // The subchannel stays with the frames as addressed, not shifted.
    return read_raw(c, (uint32_t)lba, count, 0, tmp, sub);
  }
  return RIP_OK;
}

/* Output */

// The CloneCD .sub: each frame's 96 bytes as P..W, 12 bytes a channel.
static void deinterleave_sub(const uint8_t *pw, uint8_t *out) {
  unsigned ch;
  unsigned i;

  memset(out, 0, CD_SUB);
  for (i = 0; i < CD_SUB; i++) {
    for (ch = 0; ch < 8; ch++) {
      if (pw[i] & (0x80 >> ch)) {
        out[ch * 12 + i / 8] |= (uint8_t)(0x80 >> (i % 8));
      }
    }
  }
}

struct track_out {
  char name[300];  // the .bin's name
  uint32_t first;  // LBA the file starts at (INDEX 00, or 01 for track 1)
  uint32_t frames;
  uint32_t pregap;  // INDEX 00 frames in the file
  struct rip_hashes hashes;
};

/* Dreamcast GD-ROM */

#define TOC1_AT (16 + 0x100)  // in IP.BIN, the high-density area's first sector
#define TOC1_ENTRIES 97

static uint32_t toc1_lba(const uint8_t *e) {
  // Little-endian, 24 bits, counted from MSF 00:00:00.
  return ((uint32_t)e[0] | ((uint32_t)e[1] << 8) | ((uint32_t)e[2] << 16)) - CD_PREGAP;
}

// The high-density area's tracks, from the TOC1 in its IP.BIN, after the
// low-density ones as a second session (redumper's dreamcast.ixx).
static int gd_add_hd(struct rip_transport *t, struct cd_toc *toc) {
  uint8_t frame[CD_FRAME];
  const uint8_t *toc1 = frame + TOC1_AT;
  uint8_t first;
  unsigned i;
  unsigned from = toc->count;

  if (cd_read_frames(t, GD_HD_LBA, 1, 0, frame, NULL) != RIP_OK || memcmp(toc1, "TOC1", 4) != 0) {
    return RIP_ERR;
  }
  first = toc1[4 + TOC1_ENTRIES * 4 + 2];
  for (i = 0; i < TOC1_ENTRIES && toc->count < CD_MAX_TRACKS; i++) {
    const uint8_t *e = toc1 + 4 + i * 4;
    struct cd_track *tr = &toc->tracks[toc->count];
    if (e[3] == 0xFF) {
      break;
    }
    memset(tr, 0, sizeof(*tr));
    tr->number = (uint8_t)(first + i);
    tr->control = e[3] >> 4;
    tr->session = 2;
    tr->mode = (tr->control & CD_CTRL_DATA) ? CD_MODE1 : CD_MODE_AUDIO;
    tr->start = toc1_lba(e);
    toc->count++;
  }
  if (toc->count == from) {
    return RIP_ERR;
  }
  toc->leadout = toc1_lba(toc1 + 4 + TOC1_ENTRIES * 4 + 8);
  for (i = from; i < toc->count; i++) {
    uint32_t end = i + 1 < toc->count ? toc->tracks[i + 1].start : toc->leadout;
    if (end <= toc->tracks[i].start) {
      return RIP_ERR;
    }
    toc->tracks[i].length = end - toc->tracks[i].start;
  }
  toc->last = toc->tracks[toc->count - 1].number;
  toc->sessions = 2;
  RIP_INFO("GD-ROM: %u track(s) in the high-density area, lead-out %u", toc->count - from, toc->leadout);
  return RIP_OK;
}

// The sheet emulators take for a GD-ROM: every track's file and where it starts.
static int write_gdi(const char *path, const struct cd_toc *toc, const struct track_out *tracks) {
  char gdi[8192];
  size_t len = (size_t)snprintf(gdi, sizeof(gdi), "%u\n", toc->count);
  unsigned i;

  for (i = 0; i < toc->count && len < sizeof(gdi); i++) {
    len += (size_t)snprintf(gdi + len, sizeof(gdi) - len, "%u %u %u 2352 \"%s\" 0\n", toc->tracks[i].number,
                            tracks[i].first, cd_track_is_audio(&toc->tracks[i]) ? 0 : 4, tracks[i].name);
  }
  return len < sizeof(gdi) && rip_write_file(path, gdi, len) == 0 ? RIP_OK : RIP_ERR;
}

static void track_name(const char *stem, unsigned i, unsigned count, char *out, size_t n) {
  if (count == 1) {
    snprintf(out, n, "%s.bin", stem);
  } else if (count < 10) {
    snprintf(out, n, "%s (Track %u).bin", stem, i + 1);
  } else {
    snprintf(out, n, "%s (Track %02u).bin", stem, i + 1);
  }
}

static void write_cd_manifest(const char *path, const struct rip_drive *drive, const struct rip_disc *disc,
                              const struct cd_toc *toc, const struct track_out *tracks, int offset, int offset_known,
                              const struct cd_read *c, const char *image) {
  FILE *f = fopen(path, "w");
  unsigned i;

  if (!f) {
    RIP_WARN("cannot write manifest %s", path);
    return;
  }
  fprintf(f, "disc: %s\n", rip_disc_kind_name(disc->kind));
  fprintf(f, "volume_id: %s\n", disc->volume_id);
  if (disc->title_id[0]) {
    fprintf(f, "title_id: %s\n", disc->title_id);
  }
  if (drive) {
    fprintf(f, "drive: %s %s %s (%s)\n", drive->inquiry.vendor, drive->inquiry.product, drive->inquiry.revision,
            rip_drive_source_name(drive->source));
  }
  fprintf(f, "image: %s\n", image);
  fprintf(f, "read_offset: %+d samples%s\n", offset,
          offset_known ? "" : " (drive not in the offset list, not corrected)");
  fprintf(f, "c2: %s\n",
          (c->flags & CD_READ_C2) ? "checked"
          : c->c2_dropped         ? "checked until the drive stopped taking it, then off"
                                  : "not supported by the drive");
  fprintf(f, "rereads: %llu\nframes_with_errors: %llu\nunreadable_frames: %llu%s\n", (unsigned long long)c->retried,
          (unsigned long long)c->c2_frames, (unsigned long long)c->unreadable,
          c->unreadable ? " (written as zeros: this image won't match Redump)" : "");
  fprintf(f, "tracks: %u, sessions: %u, lead-out: %u\n", toc->count, toc->sessions, toc->leadout);
  for (i = 0; i < toc->count; i++) {
    const struct track_out *t = &tracks[i];
    char crc[9];
    char md5[33];
    char sha1[41];
    snprintf(crc, sizeof(crc), "%08x", t->hashes.crc32);
    rip_hex(t->hashes.md5, 16, md5);
    rip_hex(t->hashes.sha1, 20, sha1);
    fprintf(f, "track %02u: %s, %s, start %u, pregap %u, %u frames, size %llu, crc32 %s, md5 %s, sha1 %s\n",
            toc->tracks[i].number, t->name, cue_mode_name(toc->tracks[i].mode), toc->tracks[i].start, t->pregap,
            t->frames, (unsigned long long)t->hashes.size, crc, md5, sha1);
  }
  fclose(f);
}

/* Audio CDs */

// A CUE sheet string: no double quotes inside.
static void cue_str(const char *in, char *out, size_t n) {
  size_t i;

  snprintf(out, n, "%s", in);
  for (i = 0; out[i]; i++) {
    if (out[i] == '"') {
      out[i] = '\'';
    }
  }
}

struct cue_text {
  char *buf;
  size_t len;
  size_t cap;
};

static void cue_put(struct cue_text *c, const char *fmt, const char *a, const char *b) {
  char line[1200];
  int n = snprintf(line, sizeof(line), fmt, a, b);
  if (n > 0 && c->len + (size_t)n < c->cap) {
    memcpy(c->buf + c->len, line, (size_t)n + 1);
    c->len += (size_t)n;
  }
}

static void cue_index(struct cue_text *c, unsigned index, uint32_t frames) {
  char msf[16];
  char idx[8];

  cd_msf_str(frames, msf, sizeof(msf));
  snprintf(idx, sizeof(idx), "%02u", index);
  cue_put(c, "    INDEX %s %s\r\n", idx, msf);
}

// Every audio track: a FLAC each with the gap before the next track at its
// end (EAC's default, what music players expect), or one FLAC for the disc.
// Tags and names from opts->audio when MusicBrainz knew the disc.
static int rip_audio(struct cd_read *c, const struct cd_toc *toc, struct rip_drive *drive, const struct rip_disc *disc,
                     const struct rip_job_opts *opts, enum audio_codec codec, const char *stem,
                     struct rip_job_result *res, int offset, int offset_known) {
  struct audio_settings as = {codec, opts->audio_quality, opts->cover, opts->cover_len};
  // The CUE sheet's word for the files: MP3 has its own, the rest read as WAVE.
  const char *cue_kind = codec == AUDIO_MP3 ? "MP3" : "WAVE";
  const struct rip_audio_meta *m = opts->audio;
  unsigned idx[CD_MAX_TRACKS];
  unsigned n = 0;
  struct track_out *tracks = calloc(CD_MAX_TRACKS, sizeof(*tracks));
  uint8_t *frames = malloc((size_t)c->per_read * CD_FRAME);
  uint8_t *tmp = malloc((size_t)(c->per_read + 2) * c->stride);
  struct cue_text cue = {calloc(1, 65536), 0, 65536};
  struct audio_out *out = NULL;
  char folder[1024];
  char path[1400];
  char s1[300];
  char s2[300];
  uint64_t total = 0;
  uint64_t done = 0;
  uint64_t next_report = 0;
  uint64_t started = rip_now_ms();
  unsigned k;
  int rc = RIP_OK;

  if (!tracks || !frames || !tmp || !cue.buf) {
    rc = RIP_ENOMEM;
    goto done;
  }
  for (k = 0; k < toc->count; k++) {
    if (cd_track_is_audio(&toc->tracks[k])) {
      idx[n++] = k;
    }
  }
  if (!n) {
    RIP_ERROR("no audio tracks on this disc");
    rc = RIP_ERR;
    goto done;
  }
  // Each file's span: INDEX 01 up to the next audio track's INDEX 01 in the
  // same session (its gap included), else the track's own end.
  for (k = 0; k < n; k++) {
    const struct cd_track *tr = &toc->tracks[idx[k]];
    const struct cd_track *nx = k + 1 < n ? &toc->tracks[idx[k + 1]] : NULL;
    struct track_out *o = &tracks[k];
    o->first = tr->start;
    o->frames = nx && nx->session == tr->session ? nx->start - tr->start : tr->length;
    o->pregap = 0;
    total += (uint64_t)o->frames * CD_FRAME;
    if (m && m->titles[tr->number - 1][0]) {
      char title[256];
      snprintf(title, sizeof(title), "%s", m->titles[tr->number - 1]);
      rip_sanitize_name(title, sizeof(title));
      snprintf(o->name, sizeof(o->name), "%02u - %s%s", tr->number, title, audio_out_ext(codec));
    } else {
      snprintf(o->name, sizeof(o->name), "%02u - Track %02u%s", tr->number, tr->number, audio_out_ext(codec));
    }
  }
  if (opts->audio_image) {
    snprintf(tracks[0].name, sizeof(tracks[0].name), "%s%s", stem, audio_out_ext(codec));
  }
  // The album's folder: the tracks, the sheet and the manifest together.
  if (rip_unique_path(opts->out_dir, stem, "", folder, sizeof(folder)) != RIP_OK || rip_mkdir_p(folder) != 0) {
    RIP_ERROR("cannot create the folder for %s", stem);
    rc = RIP_ERR;
    goto done;
  }
  snprintf(res->image_path, sizeof(res->image_path), "%s", folder);
  // The cover beside the tracks too, where players look for it.
  if (opts->cover && opts->cover_len) {
    int png = opts->cover_len >= 4 && !memcmp(opts->cover, "\x89PNG", 4);
    int fd;
    snprintf(path, sizeof(path), "%s/cover.%s", folder, png ? "png" : "jpg");
    fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd >= 0) {
      if (rip_write_all(fd, opts->cover, opts->cover_len) != 0) {
        RIP_WARN("audio CD: the cover couldn't be saved");
      }
      close(fd);
    }
  }
  mmc_set_max_speed(c->t, toc->leadout ? toc->leadout - 1 : 0);
  RIP_INFO("audio CD: %u track(s), %.1f MiB, read offset %+d%s, C2 %s, %s", n, (double)total / 1048576.0, offset,
           offset_known ? "" : " (unknown drive)", c->flags & CD_READ_C2 ? "on" : "off",
           opts->audio_image ? "one file" : "a file per track");
  if (opts->progress) {
    opts->progress("reading", 0, total, opts->user);
  }

  // The sheet's head.
  if (m && m->date[0]) {
    cue_put(&cue, "REM DATE %s%s\r\n", m->date, "");
  }
  snprintf(s1, sizeof(s1), "%08X", cd_freedb_id(toc));
  cue_put(&cue, "REM DISCID %s%s\r\n", s1, "");
  if (disc->musicbrainz_id[0]) {
    cue_put(&cue, "REM MUSICBRAINZ_DISCID %s%s\r\n", disc->musicbrainz_id, "");
  }
  if (m && m->artist[0]) {
    cue_str(m->artist, s1, sizeof(s1));
    cue_put(&cue, "PERFORMER \"%s\"%s\r\n", s1, "");
  }
  if (m && m->album[0]) {
    cue_str(m->album, s1, sizeof(s1));
    cue_put(&cue, "TITLE \"%s\"%s\r\n", s1, "");
  }

  for (k = 0; rc == RIP_OK && k < n; k++) {
    const struct cd_track *tr = &toc->tracks[idx[k]];
    struct track_out *o = &tracks[k];
    struct audio_tags tags;
    struct rip_hash_ctx h;
    uint32_t at = 0;
    uint32_t base = opts->audio_image ? tracks[0].first : o->first;

    memset(&tags, 0, sizeof(tags));
    tags.artist = m ? m->artist : NULL;
    tags.album = m ? m->album : NULL;
    tags.date = m ? m->date : NULL;
    tags.mb_discid = disc->musicbrainz_id;
    if (!opts->audio_image) {
      tags.title = m ? m->titles[tr->number - 1] : NULL;
      tags.track = tr->number;
      tags.total = n;
    }
    if (!opts->audio_image || k == 0) {
      uint64_t frames_in_file = o->frames;
      if (opts->audio_image) {
        unsigned q;
        for (q = 1; q < n; q++) {
          frames_in_file += tracks[q].frames;
        }
      }
      snprintf(path, sizeof(path), "%s/%s", folder, o->name);
      rc = audio_out_open(&out, path, frames_in_file, &as, &tags);
      if (rc != RIP_OK) {
        break;
      }
      if (opts->audio_image) {
        cue_str(o->name, s1, sizeof(s1));
        cue_put(&cue, "FILE \"%s\" %s\r\n", s1, cue_kind);
      }
    }
    if (opts->audio_image) {
      // One file: every track's indexes are positions in it. (The sheet for
      // a file per track is written once all are read, below.)
      snprintf(s1, sizeof(s1), "%02u", tr->number);
      cue_put(&cue, "  TRACK %s AUDIO%s\r\n", s1, "");
      if (m && m->titles[tr->number - 1][0]) {
        cue_str(m->titles[tr->number - 1], s2, sizeof(s2));
        cue_put(&cue, "    TITLE \"%s\"%s\r\n", s2, "");
      }
      if (tr->control & (CD_CTRL_COPY | CD_CTRL_PREEMPHASIS | CD_CTRL_4CH)) {
        cue_put(&cue, "    FLAGS%s%s", (tr->control & CD_CTRL_COPY) ? " DCP" : "",
                (tr->control & CD_CTRL_PREEMPHASIS) ? " PRE" : "");
        cue_put(&cue, "%s%s\r\n", (tr->control & CD_CTRL_4CH) ? " 4CH" : "", "");
      }
      if (k > 0 && tr->pregap) {
        cue_index(&cue, 0, tr->start - tr->pregap - base);
      }
      cue_index(&cue, 1, tr->start - base);
    }

    rip_hash_init(&h);
    while (rc == RIP_OK && at < o->frames) {
      uint32_t cnt = o->frames - at < c->per_read ? o->frames - at : c->per_read;
      rc = read_shifted(c, (int64_t)o->first + at, cnt, (int64_t)offset * 4, frames, NULL, tmp);
      if (rc != RIP_OK) {
        break;
      }
      rip_hash_update(&h, frames, (size_t)cnt * CD_FRAME);
      rc = audio_out_write(out, frames, cnt);
      at += cnt;
      done += (uint64_t)cnt * CD_FRAME;
      if (opts->progress && done >= next_report) {
        opts->progress("reading", done, total, opts->user);
        next_report = done + REPORT_BYTES;
      }
    }
    rip_hash_final(&h, &o->hashes);
    if (!opts->audio_image || k + 1 == n || rc != RIP_OK) {
      int crc = audio_out_close(out, rc == RIP_OK);
      out = NULL;
      if (rc == RIP_OK) {
        rc = crc;
      }
    }
  }
  if (out) {
    audio_out_close(out, 0);
    out = NULL;
  }
  if (rc == RIP_OK && !opts->audio_image) {
    // A file per track: each gap's INDEX 00 is at the end of the file before,
    // so a track with a gap opens in one FILE and gets its INDEX 01 in the next.
    struct cue_text c2 = {calloc(1, 65536), 0, 65536};
    if (c2.buf) {
      memcpy(c2.buf, cue.buf, cue.len + 1);  // the head: REM, PERFORMER, TITLE
      c2.len = cue.len;
      for (k = 0; k < n; k++) {
        const struct cd_track *tr = &toc->tracks[idx[k]];
        cue_str(tracks[k].name, s1, sizeof(s1));
        if (k == 0 || !tr->pregap) {
          cue_put(&c2, "FILE \"%s\" %s\r\n", s1, cue_kind);
        }
        snprintf(s2, sizeof(s2), "%02u", tr->number);
        cue_put(&c2, "  TRACK %s AUDIO%s\r\n", s2, "");
        if (m && m->titles[tr->number - 1][0]) {
          char t2[300];
          cue_str(m->titles[tr->number - 1], t2, sizeof(t2));
          cue_put(&c2, "    TITLE \"%s\"%s\r\n", t2, "");
        }
        if (tr->control & (CD_CTRL_COPY | CD_CTRL_PREEMPHASIS | CD_CTRL_4CH)) {
          cue_put(&c2, "    FLAGS%s%s", (tr->control & CD_CTRL_COPY) ? " DCP" : "",
                  (tr->control & CD_CTRL_PREEMPHASIS) ? " PRE" : "");
          cue_put(&c2, "%s%s\r\n", (tr->control & CD_CTRL_4CH) ? " 4CH" : "", "");
        }
        if (k > 0 && tr->pregap) {
          // The gap is the end of the previous file; this track starts the next one.
          cue_index(&c2, 0, tr->start - tr->pregap - tracks[k - 1].first);
          cue_put(&c2, "FILE \"%s\" %s\r\n", s1, cue_kind);
        }
        cue_index(&c2, 1, 0);
      }
      free(cue.buf);
      cue = c2;
    }
  }
  if (rc == RIP_OK) {
    snprintf(path, sizeof(path), "%s/%s.cue", folder, stem);
    if (rip_write_file(path, cue.buf, cue.len) != 0) {
      RIP_ERROR("cannot write %s", path);
      rc = RIP_ERR;
    }
  }
  if (rc == RIP_OK) {
    uint64_t ms = rip_now_ms() - started;
    struct cd_toc listed = *toc;
    res->hashes = tracks[0].hashes;
    res->unreadable = c->unreadable;
    if (opts->progress) {
      opts->progress("reading", total, total, opts->user);
    }
    RIP_INFO(
        "read %llu MiB of audio in %llu s (%.2f MiB/s), %llu reread(s), %llu frame(s) with errors, %llu unreadable",
        (unsigned long long)(total >> 20), (unsigned long long)(ms / 1000),
        ms ? (double)total / 1048576.0 / ((double)ms / 1000.0) : 0.0, (unsigned long long)c->retried,
        (unsigned long long)c->c2_frames, (unsigned long long)c->unreadable);
    // The manifest lists the audio tracks with their files and CRCs.
    listed.count = n;
    for (k = 0; k < n; k++) {
      listed.tracks[k] = toc->tracks[idx[k]];
    }
    snprintf(res->manifest_path, sizeof(res->manifest_path), "%.700s/%.250s.txt", folder, stem);
    write_cd_manifest(res->manifest_path, drive, disc, &listed, tracks, offset, offset_known, c, res->image_path);
  } else {
    for (k = 0; k < n; k++) {
      snprintf(path, sizeof(path), "%s/%s", folder, tracks[k].name);
      unlink(path);
    }
    if (res->image_path[0]) {
      snprintf(path, sizeof(path), "%s/%s.cue", folder, stem);
      unlink(path);
      rmdir(folder);
    }
    res->image_path[0] = '\0';
  }

done:
  free(tracks);
  free(frames);
  free(tmp);
  free(cue.buf);
  return rc;
}

int rip_cd_run(const struct rip_recipe *recipe, struct rip_drive *drive, const struct rip_disc *disc,
               const struct rip_job_opts *opts, const char *stem, struct rip_job_result *res) {
  struct cd_toc toc;
  struct cd_read c;
  struct track_out *tracks = NULL;
  struct chd_writer *chd = NULL;
  uint8_t *frames = NULL;
  uint8_t *sub = NULL;
  uint8_t *tmp = NULL;
  uint8_t *subout = NULL;
  int chd_out = recipe->id == RIP_RECIPE_CHD;
  int want_sub = !chd_out && opts->cd_subchannel;
  int sub_fd = -1;
  char folder[1024] = "";
  char path[1400];
  char part[1500];
  uint64_t total = 0;
  uint64_t done = 0;
  uint64_t next_report = 0;
  uint64_t started = rip_now_ms();
  int offset_known;
  int offset = drive_offset(drive, &offset_known);
  unsigned i;
  int rc = RIP_OK;

  if (!drive || !disc->has_toc || !disc->toc.count) {
    RIP_ERROR("this CD's track list couldn't be read");
    return RIP_ERR;
  }
  memset(&c, 0, sizeof(c));
  toc = disc->toc;
  // Where each pregap starts, from the Q subchannel; data tracks' modes.
  rip_report(&(struct rip_progress){opts, 0}, "scanning", 0, 0, 1);
  cd_find_pregaps(drive->t, &toc);
  if (disc->kind == RIP_DISC_DREAMCAST && disc->gd_rom && gd_add_hd(drive->t, &toc) != RIP_OK) {
    RIP_ERROR("GD-ROM: the high-density area's track list can't be read in this drive");
    return RIP_ERR;
  }
  for (i = 0; i < toc.count; i++) {
    cd_detect_mode(drive->t, &toc.tracks[i]);
  }

  c.t = drive->t;
  c.flags = (drive->caps & RIP_CAP_CD_C2) ? CD_READ_C2 : 0;
  if (want_sub) {
    c.flags |= CD_READ_SUB;
  }
  c.stride = cd_frame_stride(c.flags);
  c.per_read = (uint32_t)(READ_BYTES / c.stride);
  c.end = toc.leadout;
  c.cancel = opts->cancel;
  // Page-aligned: an unaligned 62 KiB transfer spans 17 pages, one more
  // than the pass-through maps (E2BIG).
  // Room for two frames more than a read: a read shifted by the drive's offset
  // (read_shifted) asks for one frame past the chunk, and wrote it past the end
  // of this buffer before, corrupting the heap on every audio read.
  if (posix_memalign((void **)&c.raw, 4096, (size_t)(c.per_read + 2) * c.stride) != 0) {
    c.raw = NULL;
  }
  frames = malloc((size_t)c.per_read * CD_FRAME);
  tmp = malloc((size_t)(c.per_read + 2) * c.stride);
  sub = want_sub ? malloc((size_t)c.per_read * CD_SUB) : NULL;
  subout = want_sub ? malloc(CD_SUB) : NULL;
  tracks = calloc(toc.count, sizeof(*tracks));
  if (!c.raw || !frames || !tmp || !tracks || (want_sub && (!sub || !subout))) {
    rc = RIP_ENOMEM;
    goto done;
  }
  if (rip_recipe_is_audio(recipe->id)) {
    rc = rip_audio(&c, &toc, drive, disc, opts, (enum audio_codec)rip_recipe_audio_codec(recipe->id), stem, res, offset,
                   offset_known);
    goto done;
  }

  // Each track's file: its INDEX 00 frames (not track 1's lead-in gap) and the
  // track itself.
  for (i = 0; i < toc.count; i++) {
    const struct cd_track *tr = &toc.tracks[i];
    struct track_out *o = &tracks[i];
    o->pregap = i == 0 ? 0 : tr->pregap;
    o->first = tr->start - o->pregap;
    o->frames = o->pregap + tr->length;
    track_name(stem, i, toc.count, o->name, sizeof(o->name));
    total += (uint64_t)o->frames * CD_FRAME;
  }

  if (chd_out) {
    struct chd_cd_track ct[CD_MAX_TRACKS];
    for (i = 0; i < toc.count; i++) {
      ct[i].mode = toc.tracks[i].mode;
      ct[i].frames = tracks[i].frames;
      ct[i].pregap = tracks[i].pregap;
    }
    if (rip_unique_path(opts->out_dir, stem, ".chd", res->image_path, sizeof(res->image_path)) != RIP_OK) {
      rc = RIP_ERR;
      goto done;
    }
    snprintf(part, sizeof(part), "%s.part", res->image_path);
    rc = chd_create_cd(&chd, part, ct, toc.count, opts->chd_codec, 0);
    if (rc != RIP_OK) {
      goto done;
    }
  } else {
    // A folder of tracks and the sheet: what emulators and RomM take as one game.
    if (rip_unique_path(opts->out_dir, stem, "", folder, sizeof(folder)) != RIP_OK || rip_mkdir_p(folder) != 0) {
      RIP_ERROR("cannot create the folder for %s", stem);
      rc = RIP_ERR;
      goto done;
    }
    snprintf(res->image_path, sizeof(res->image_path), "%s", folder);
    if (want_sub) {
      snprintf(path, sizeof(path), "%s/%s.sub", folder, stem);
      sub_fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
      if (sub_fd < 0) {
        RIP_ERROR("cannot create %s: %s", path, strerror(errno));
        rc = RIP_ERR;
        goto done;
      }
    }
  }
  mmc_set_max_speed(drive->t, toc.leadout ? toc.leadout - 1 : 0);
  RIP_INFO("CD: %u track(s), %u session(s), %.1f MiB, read offset %+d%s, C2 %s", toc.count, toc.sessions,
           (double)total / 1048576.0, offset, offset_known ? "" : " (unknown drive)",
           c.flags & CD_READ_C2 ? "on" : "off");
  if (opts->progress) {
    opts->progress("reading", 0, total, opts->user);
  }

  for (i = 0; rc == RIP_OK && i < toc.count; i++) {
    const struct cd_track *tr = &toc.tracks[i];
    struct track_out *o = &tracks[i];
    struct rip_hash_ctx h;
    int audio = cd_track_is_audio(tr);
    int fd = -1;
    uint32_t at = 0;

    if (!chd_out) {
      snprintf(path, sizeof(path), "%s/%s", folder, o->name);
      fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
      if (fd < 0) {
        RIP_ERROR("cannot create %s: %s", path, strerror(errno));
        rc = RIP_ERR;
        break;
      }
    }
    rip_hash_init(&h);
    while (rc == RIP_OK && at < o->frames) {
      uint32_t n = o->frames - at < c.per_read ? o->frames - at : c.per_read;
      uint32_t lba = o->first + at;
      if (audio) {
        rc = read_shifted(&c, lba, n, (int64_t)offset * 4, frames, sub, tmp);
      } else {
        rc = read_raw(&c, lba, n, 1, frames, sub);
      }
      if (rc != RIP_OK) {
        break;
      }
      rip_hash_update(&h, frames, (size_t)n * CD_FRAME);
      if (chd_out) {
        rc = chd_write_frames(chd, frames, NULL, n);
      } else if (rip_write_all(fd, frames, (size_t)n * CD_FRAME) != 0) {
        RIP_ERROR("write failed: %s", strerror(errno));
        rc = RIP_ERR;
      }
      if (rc == RIP_OK && sub_fd >= 0) {
        uint32_t k;
        for (k = 0; k < n && rc == RIP_OK; k++) {
          deinterleave_sub(sub + (size_t)k * CD_SUB, subout);
          rc = rip_write_all(sub_fd, subout, CD_SUB) == 0 ? RIP_OK : RIP_ERR;
        }
      }
      at += n;
      done += (uint64_t)n * CD_FRAME;
      if (opts->progress && done >= next_report) {
        opts->progress("reading", done, total, opts->user);
        next_report = done + REPORT_BYTES;
      }
    }
    rip_hash_final(&h, &o->hashes);
    if (fd >= 0 && close(fd) != 0 && rc == RIP_OK) {
      rc = RIP_ERR;
    }
  }

  if (rc == RIP_OK && chd_out) {
    rc = chd_finish(chd, NULL);
    chd = NULL;
    if (rc == RIP_OK && rename(part, res->image_path) != 0) {
      rc = RIP_ERR;
    }
  }
  if (rc == RIP_OK && !chd_out) {
    // The sheet, one FILE per track as Redump lays it out.
    const char *names[CD_MAX_TRACKS];
    char sheet[16384];
    size_t len;
    for (i = 0; i < toc.count; i++) {
      names[i] = tracks[i].name;
    }
    len = cue_write(&toc, names, 1, sheet, sizeof(sheet));
    snprintf(path, sizeof(path), "%s/%s.cue", folder, stem);
    if (!len || rip_write_file(path, sheet, len) != 0) {
      RIP_ERROR("cannot write %s", path);
      rc = RIP_ERR;
    }
    snprintf(path, sizeof(path), "%s/%s.gdi", folder, stem);
    if (rc == RIP_OK && disc->gd_rom && write_gdi(path, &toc, tracks) != RIP_OK) {
      RIP_ERROR("cannot write %s", path);
      rc = RIP_ERR;
    }
  }
  if (rc == RIP_OK) {
    uint64_t ms = rip_now_ms() - started;
    res->hashes = tracks[0].hashes;
    res->unreadable = c.unreadable;
    if (opts->progress) {
      opts->progress("reading", total, total, opts->user);
    }
    RIP_INFO("read %llu MiB in %llu s (%.2f MiB/s), %llu reread(s), %llu frame(s) with errors, %llu unreadable",
             (unsigned long long)(total >> 20), (unsigned long long)(ms / 1000),
             ms ? (double)total / 1048576.0 / ((double)ms / 1000.0) : 0.0, (unsigned long long)c.retried,
             (unsigned long long)c.c2_frames, (unsigned long long)c.unreadable);
    if (rip_unique_path(opts->out_dir, stem, ".txt", res->manifest_path, sizeof(res->manifest_path)) == RIP_OK) {
      write_cd_manifest(res->manifest_path, drive, disc, &toc, tracks, offset, offset_known, &c, res->image_path);
    }
  }

done:
  if (chd) {
    chd_abort(chd);
  }
  if (sub_fd >= 0) {
    close(sub_fd);
  }
  if (rc != RIP_OK && res->image_path[0] && !chd_out) {
    // A part-read folder isn't left looking like a rip.
    for (i = 0; tracks && i < toc.count; i++) {
      snprintf(path, sizeof(path), "%s/%s", folder, tracks[i].name);
      unlink(path);
    }
    snprintf(path, sizeof(path), "%s/%s.sub", folder, stem);
    unlink(path);
    rmdir(folder);
  }
  if (rc != RIP_OK) {
    res->image_path[0] = '\0';
  }
  free(c.raw);
  free(frames);
  free(tmp);
  free(sub);
  free(subout);
  free(tracks);
  return rc;
}
