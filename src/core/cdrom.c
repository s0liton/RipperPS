#include "ripper/cdrom.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ripper/common.h"
#include "ripper/hash.h"
#include "ripper/log.h"
#include "ripper/mmc.h"

#define SESSION_GAP 11400  // lead-out + lead-in + pregap between two sessions

uint8_t cd_bcd(uint8_t v) {
  return (uint8_t)(((v / 10) << 4) | (v % 10));
}

uint8_t cd_unbcd(uint8_t v) {
  return (uint8_t)((v >> 4) * 10 + (v & 0x0F));
}

void cd_msf_str(uint32_t frames, char *out, size_t len) {
  snprintf(out, len, "%02u:%02u:%02u", frames / (60 * CD_FRAMES_PER_SEC), (frames / CD_FRAMES_PER_SEC) % 60,
           frames % CD_FRAMES_PER_SEC);
}

size_t cd_frame_stride(unsigned flags) {
  return CD_FRAME + ((flags & CD_READ_C2) ? CD_C2 : 0) + ((flags & CD_READ_SUB) ? CD_SUB : 0);
}

/* TOC */

static int read_toc(struct rip_transport *t, uint8_t format, uint8_t *buf, uint16_t len) {
  uint8_t cdb[10] = {0x43, 0, format, 0, 0, 0, format == 2 ? 1 : 0, 0, 0, 0};

  rip_put_be16(cdb + 7, len);
  memset(buf, 0, len);
  return rip_scsi(t, cdb, sizeof(cdb), RIP_DIR_IN, buf, len, MMC_TIMEOUT_MS, NULL);
}

static uint32_t msf_lba(uint8_t m, uint8_t s, uint8_t f) {
  return ((uint32_t)m * 60 + s) * CD_FRAMES_PER_SEC + f - CD_PREGAP;
}

static struct cd_track *add_track(struct cd_toc *toc, uint8_t number) {
  struct cd_track *tr;

  if (toc->count >= CD_MAX_TRACKS || number == 0 || number > 99) {
    return NULL;
  }
  tr = &toc->tracks[toc->count++];
  memset(tr, 0, sizeof(*tr));
  tr->number = number;
  tr->session = 1;
  return tr;
}

// Full TOC (format 2): 11-byte descriptors per session and point.
static int parse_full_toc(const uint8_t *buf, size_t len, struct cd_toc *toc, uint32_t *leadouts) {
  size_t total = (size_t)rip_be16(buf) + 2;
  size_t off;

  if (total > len) {
    total = len;
  }
  for (off = 4; off + 11 <= total; off += 11) {
    const uint8_t *d = buf + off;
    uint8_t session = d[0];
    uint8_t adr = d[1] >> 4;
    uint8_t control = d[1] & 0x0F;
    uint8_t point = d[3];

    if (adr != 1 || session == 0 || session > 99) {
      continue;
    }
    if (session > toc->sessions) {
      toc->sessions = session;
    }
    if (point >= 1 && point <= 99) {
      struct cd_track *tr = add_track(toc, point);
      if (tr) {
        tr->control = control;
        tr->session = session;
        tr->start = msf_lba(d[8], d[9], d[10]);
      }
    } else if (point == 0xA0 && session == 1) {
      toc->type = (enum cd_disc_type)d[9];
    } else if (point == 0xA2) {
      leadouts[session] = msf_lba(d[8], d[9], d[10]);
    }
  }
  return toc->count ? RIP_OK : RIP_ERR;
}

static int parse_plain_toc(const uint8_t *buf, size_t len, struct cd_toc *toc, uint32_t *leadouts) {
  size_t total = (size_t)rip_be16(buf) + 2;
  size_t off;

  if (total > len) {
    total = len;
  }
  toc->sessions = 1;
  for (off = 4; off + 8 <= total; off += 8) {
    const uint8_t *d = buf + off;
    if (d[2] == 0xAA) {
      leadouts[1] = rip_be32(d + 4);
    } else {
      struct cd_track *tr = add_track(toc, d[2]);
      if (tr) {
        tr->control = d[1] & 0x0F;
        tr->start = rip_be32(d + 4);
      }
    }
  }
  return toc->count && leadouts[1] ? RIP_OK : RIP_ERR;
}

static int by_number(const void *a, const void *b) {
  return (int)((const struct cd_track *)a)->number - (int)((const struct cd_track *)b)->number;
}

int cd_read_toc(struct rip_transport *t, struct cd_toc *toc) {
  uint8_t *buf = malloc(4 + 11 * 120);
  uint32_t leadouts[100];
  unsigned i;
  int rc = RIP_ERR;

  memset(toc, 0, sizeof(*toc));
  memset(leadouts, 0, sizeof(leadouts));
  if (!buf) {
    return RIP_ENOMEM;
  }
  if (read_toc(t, 2, buf, 4 + 11 * 120) == RIP_OK) {
    rc = parse_full_toc(buf, 4 + 11 * 120, toc, leadouts);
  }
  if (rc != RIP_OK) {
    memset(toc, 0, sizeof(*toc));
    memset(leadouts, 0, sizeof(leadouts));
    if (read_toc(t, 0, buf, 4 + 8 * 100) == RIP_OK) {
      rc = parse_plain_toc(buf, 4 + 8 * 100, toc, leadouts);
    }
  }
  free(buf);
  if (rc != RIP_OK) {
    return rc;
  }

  qsort(toc->tracks, toc->count, sizeof(toc->tracks[0]), by_number);
  toc->first = toc->tracks[0].number;
  toc->last = toc->tracks[toc->count - 1].number;
  toc->leadout = leadouts[toc->sessions] ? leadouts[toc->sessions] : leadouts[1];
  for (i = 0; i < toc->count; i++) {
    struct cd_track *tr = &toc->tracks[i];
    uint32_t end;

    tr->mode = cd_track_is_audio(tr) ? CD_MODE_AUDIO : CD_MODE1;
    if (i + 1 < toc->count && toc->tracks[i + 1].session == tr->session) {
      end = toc->tracks[i + 1].start;
    } else {
      end = leadouts[tr->session] ? leadouts[tr->session] : toc->leadout;
    }
    tr->length = end > tr->start ? end - tr->start : 0;
    // A data track followed by audio (mixed mode) leaves a 2-second pregap
    // on the audio track; the Q channel says for sure (cd_find_pregaps).
    if (i > 0 && toc->tracks[i - 1].session == tr->session && !cd_track_is_audio(&toc->tracks[i - 1]) &&
        cd_track_is_audio(tr)) {
      tr->pregap = CD_PREGAP;
    }
  }
  // Lengths so far run start to start; the pregap belongs to the next track.
  for (i = 0; i + 1 < toc->count; i++) {
    struct cd_track *next = &toc->tracks[i + 1];
    if (next->session == toc->tracks[i].session && toc->tracks[i].length > next->pregap) {
      toc->tracks[i].length -= next->pregap;
    }
  }
  return RIP_OK;
}

/* READ CD */

int cd_read_frames(struct rip_transport *t, uint32_t lba, uint32_t count, unsigned flags, void *buf,
                   struct rip_sense *sense) {
  uint8_t cdb[12] = {0xBE, 0, 0, 0, 0, 0, 0, 0, 0, 0xF8, 0, 0};

  rip_put_be32(cdb + 2, lba);
  cdb[6] = (uint8_t)(count >> 16);
  cdb[7] = (uint8_t)(count >> 8);
  cdb[8] = (uint8_t)count;
  if (flags & CD_READ_C2) {
    cdb[9] |= 0x02;
  }
  if (flags & CD_READ_SUB) {
    cdb[10] = 0x01;
  }
  return rip_scsi(t, cdb, sizeof(cdb), RIP_DIR_IN, buf, (size_t)count * cd_frame_stride(flags), MMC_READ_TIMEOUT_MS,
                  sense);
}

int cd_detect_mode(struct rip_transport *t, struct cd_track *track) {
  uint8_t frame[CD_FRAME];

  if (cd_track_is_audio(track)) {
    track->mode = CD_MODE_AUDIO;
    return RIP_OK;
  }
  if (cd_read_frames(t, track->start, 1, 0, frame, NULL) != RIP_OK) {
    return RIP_ERR;
  }
  switch (frame[15]) {
  case 1:
    track->mode = CD_MODE1;
    break;
  case 2:
    track->mode = CD_MODE2;
    break;
  default:
    track->mode = CD_MODE2_FORMLESS;
    break;
  }
  return RIP_OK;
}

/* Q subchannel */

static uint16_t crc16_ccitt(const uint8_t *p, size_t n) {
  uint16_t crc = 0;
  size_t i;
  int b;

  for (i = 0; i < n; i++) {
    crc ^= (uint16_t)(p[i] << 8);
    for (b = 0; b < 8; b++) {
      crc = (crc & 0x8000) ? (uint16_t)((crc << 1) ^ 0x1021) : (uint16_t)(crc << 1);
    }
  }
  return crc;
}

int cd_q_crc_ok(const uint8_t q[12]) {
  return (uint16_t)~crc16_ccitt(q, 10) == rip_be16(q + 10);
}

void cd_deinterleave_q(const uint8_t *pw, uint8_t q[12]) {
  int i;

  memset(q, 0, 12);
  for (i = 0; i < 96; i++) {
    if (pw[i] & 0x40) {
      q[i / 8] |= (uint8_t)(0x80 >> (i % 8));
    }
  }
}

void cd_make_q(uint8_t q[12], uint8_t control, uint8_t track, uint8_t index, uint32_t rel, uint32_t abs_lba) {
  uint32_t abs = abs_lba + CD_PREGAP;

  q[0] = (uint8_t)((control << 4) | 1);
  q[1] = cd_bcd(track);
  q[2] = cd_bcd(index);
  q[3] = cd_bcd((uint8_t)(rel / (60 * CD_FRAMES_PER_SEC)));
  q[4] = cd_bcd((uint8_t)((rel / CD_FRAMES_PER_SEC) % 60));
  q[5] = cd_bcd((uint8_t)(rel % CD_FRAMES_PER_SEC));
  q[6] = 0;
  q[7] = cd_bcd((uint8_t)(abs / (60 * CD_FRAMES_PER_SEC)));
  q[8] = cd_bcd((uint8_t)((abs / CD_FRAMES_PER_SEC) % 60));
  q[9] = cd_bcd((uint8_t)(abs % CD_FRAMES_PER_SEC));
  rip_put_be16(q + 10, (uint16_t)~crc16_ccitt(q, 10));
}

int cd_read_q(struct rip_transport *t, uint32_t lba, uint8_t q[12]) {
  uint8_t buf[CD_FRAME + CD_SUB];

  if (cd_read_frames(t, lba, 1, CD_READ_SUB, buf, NULL) != RIP_OK) {
    return RIP_ERR;
  }
  cd_deinterleave_q(buf + CD_FRAME, q);
  return cd_q_crc_ok(q) ? RIP_OK : RIP_ERR;
}

// The track number a frame's Q says it belongs to: -1 when unreadable. Frames
// carrying something other than a position (ADR 2 and 3, about one in a
// hundred) are skipped for their neighbours.
static int q_track_at(struct rip_transport *t, uint32_t lba, uint32_t limit) {
  uint32_t probe;

  for (probe = lba; probe < lba + 4 && probe < limit; probe++) {
    uint8_t q[12];
    int attempt;

    for (attempt = 0; attempt < 3; attempt++) {
      if (cd_read_q(t, probe, q) == RIP_OK) {
        break;
      }
    }
    if (attempt == 3) {
      continue;
    }
    if ((q[0] & 0x0F) == 1) {
      return cd_unbcd(q[1]);
    }
  }
  return -1;
}

void cd_find_pregaps(struct rip_transport *t, struct cd_toc *toc) {
  unsigned i;

  for (i = 1; i < toc->count; i++) {
    struct cd_track *prev = &toc->tracks[i - 1];
    struct cd_track *tr = &toc->tracks[i];
    uint32_t lo;
    uint32_t hi;
    int at;

    if (prev->session != tr->session) {
      continue;
    }
    // The first frame whose Q names this track. Everything before it in the
    // previous track's range belongs to that track.
    lo = prev->start;
    hi = tr->start;
    at = q_track_at(t, hi - 1, hi);
    if (at < 0) {
      continue;
    }
    if (at != tr->number) {
      hi = tr->start;  // no pregap: the frame before INDEX 01 is the previous track's
    } else {
      while (lo + 1 < hi) {
        uint32_t mid = lo + (hi - lo) / 2;
        int n = q_track_at(t, mid, tr->start);
        if (n < 0) {
          break;
        }
        if (n == tr->number) {
          hi = mid;
        } else {
          lo = mid;
        }
      }
      if (lo + 1 < hi) {
        continue;  // the search could not finish; keep the default
      }
    }
    tr->pregap = tr->start - hi;
    prev->length = hi - prev->start;
    RIP_DEBUG("track %u: pregap %u frames", tr->number, tr->pregap);
  }
}

/* Offset-corrected audio */

int cd_read_audio(struct rip_transport *t, int64_t lba, uint32_t count, int offset_samples, uint32_t end_lba,
                  uint8_t *buf) {
  int64_t shift = (int64_t)offset_samples * 4;
  int64_t first_byte = lba * CD_FRAME + shift;
  int64_t last_byte = first_byte + (int64_t)count * CD_FRAME;  // exclusive
  int64_t first_frame = first_byte >= 0 ? first_byte / CD_FRAME : -((-first_byte + CD_FRAME - 1) / CD_FRAME);
  int64_t last_frame = (last_byte + CD_FRAME - 1) / CD_FRAME;  // exclusive
  int64_t lo = first_frame < 0 ? 0 : first_frame;
  int64_t hi = last_frame > (int64_t)end_lba ? (int64_t)end_lba : last_frame;
  size_t span = (size_t)(last_frame - first_frame) * CD_FRAME;
  uint8_t *tmp;
  int rc = RIP_OK;

  if (shift == 0 && first_frame >= 0 && last_frame <= (int64_t)end_lba) {
    return cd_read_frames(t, (uint32_t)lba, count, 0, buf, NULL);
  }
  tmp = calloc(1, span);
  if (!tmp) {
    return RIP_ENOMEM;
  }
  if (hi > lo) {
    rc = cd_read_frames(t, (uint32_t)lo, (uint32_t)(hi - lo), 0, tmp + (size_t)(lo - first_frame) * CD_FRAME, NULL);
  }
  if (rc == RIP_OK) {
    memcpy(buf, tmp + (first_byte - first_frame * CD_FRAME), (size_t)count * CD_FRAME);
  }
  free(tmp);
  return rc;
}

/* Disc IDs */

// The audio tracks the IDs cover: the first session's (an Enhanced CD's data
// session is left out), and where that session's audio ends.
static unsigned audio_span(const struct cd_toc *toc, uint32_t *leadout) {
  unsigned n = 0;
  unsigned i;

  *leadout = toc->leadout;
  for (i = 0; i < toc->count; i++) {
    if (toc->tracks[i].session != 1) {
      break;
    }
    n = i + 1;
  }
  if (n < toc->count) {
    *leadout = toc->tracks[n].start - SESSION_GAP;
  } else if (n > 1 && !cd_track_is_audio(&toc->tracks[n - 1]) && cd_track_is_audio(&toc->tracks[0])) {
    // Enhanced CD written as one session: the last data track still goes.
    *leadout = toc->tracks[n - 1].start - SESSION_GAP;
    n--;
  }
  return n;
}

static void mb_base64(const uint8_t *in, size_t len, char *out) {
  static const char tab[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789._";
  size_t i;
  size_t o = 0;

  for (i = 0; i < len; i += 3) {
    uint32_t v = (uint32_t)in[i] << 16;
    size_t left = len - i;
    if (left > 1) {
      v |= (uint32_t)in[i + 1] << 8;
    }
    if (left > 2) {
      v |= in[i + 2];
    }
    out[o++] = tab[(v >> 18) & 63];
    out[o++] = tab[(v >> 12) & 63];
    out[o++] = left > 1 ? tab[(v >> 6) & 63] : '-';
    out[o++] = left > 2 ? tab[v & 63] : '-';
  }
  out[o] = '\0';
}

void cd_musicbrainz_id(const struct cd_toc *toc, char out[29]) {
  struct rip_sha1 sha;
  uint32_t leadout;
  unsigned n = audio_span(toc, &leadout);
  uint8_t digest[20];
  char hex[16];
  unsigned i;

  rip_sha1_init(&sha);
  snprintf(hex, sizeof(hex), "%02X", n ? toc->tracks[0].number : 1);
  rip_sha1_update(&sha, hex, 2);
  snprintf(hex, sizeof(hex), "%02X", n ? toc->tracks[n - 1].number : 1);
  rip_sha1_update(&sha, hex, 2);
  snprintf(hex, sizeof(hex), "%08X", leadout + CD_PREGAP);
  rip_sha1_update(&sha, hex, 8);
  for (i = 1; i < 100; i++) {
    uint32_t off = 0;
    unsigned k;
    for (k = 0; k < n; k++) {
      if (toc->tracks[k].number == i) {
        off = toc->tracks[k].start + CD_PREGAP;
      }
    }
    snprintf(hex, sizeof(hex), "%08X", off);
    rip_sha1_update(&sha, hex, 8);
  }
  rip_sha1_final(&sha, digest);
  mb_base64(digest, sizeof(digest), out);
}

uint32_t cd_freedb_id(const struct cd_toc *toc) {
  uint32_t leadout;
  unsigned n = audio_span(toc, &leadout);
  uint32_t sum = 0;
  uint32_t secs;
  unsigned i;

  if (n == 0) {
    return 0;
  }
  for (i = 0; i < n; i++) {
    uint32_t s = (toc->tracks[i].start + CD_PREGAP) / CD_FRAMES_PER_SEC;
    while (s) {
      sum += s % 10;
      s /= 10;
    }
  }
  secs = (leadout + CD_PREGAP) / CD_FRAMES_PER_SEC - (toc->tracks[0].start + CD_PREGAP) / CD_FRAMES_PER_SEC;
  return ((sum % 255) << 24) | (secs << 8) | n;
}
