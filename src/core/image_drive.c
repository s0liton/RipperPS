#include "ripper/image_drive.h"

#include <fcntl.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "ripper/cdrom.h"
#include "ripper/common.h"
#include "ripper/cue.h"
#include "ripper/log.h"
#include "ripper/mmc.h"
#include "ripper/util.h"

#define DVD_MAX_BYTES 8600000000ull  // a dual-layer DVD is 8.5 GB; anything larger is a BD

struct image_drive {
  pthread_mutex_t lock;
  struct rip_image_drive_opts opts;
  char path[1024];
  int loaded;
  int is_cd;
  // DVD/BD
  int fd;
  uint64_t size;
  // CD
  struct cue_sheet *cue;
  int fds[CD_MAX_TRACKS];
  int sub_fd;  // raw P-W of a single-file image, -1 without
};

static int sense_of(struct rip_sense *sense, uint8_t key, uint8_t asc) {
  sense->key = key;
  sense->asc = asc;
  sense->ascq = 0;
  return RIP_ESENSE;
}

static void unload(struct image_drive *d) {
  unsigned i;

  if (d->fd >= 0) {
    close(d->fd);
  }
  if (d->cue) {
    for (i = 0; i < d->cue->file_count; i++) {
      if (d->fds[i] >= 0) {
        close(d->fds[i]);
      }
    }
    free(d->cue);
  }
  if (d->sub_fd >= 0) {
    close(d->sub_fd);
  }
  d->fd = -1;
  d->sub_fd = -1;
  d->cue = NULL;
  d->loaded = 0;
  d->is_cd = 0;
  d->path[0] = '\0';
}

static uint16_t current_profile(const struct image_drive *d) {
  if (!d->loaded) {
    return MMC_PROFILE_NONE;
  }
  if (d->is_cd) {
    return MMC_PROFILE_CD_ROM;
  }
  if (d->opts.profile) {
    return d->opts.profile;
  }
  return d->size > DVD_MAX_BYTES ? MMC_PROFILE_BD_ROM : MMC_PROFILE_DVD_ROM;
}

/* CD frames */

// Which track an LBA falls in, and where its data is. Returns the track, or
// NULL past the lead-out or between sessions. *in_file is set when the frame
// is stored (not PREGAP silence).
static const struct cue_track *locate(const struct image_drive *d, uint32_t lba, int *in_file, uint64_t *offset) {
  unsigned i;

  for (i = 0; i < d->cue->count; i++) {
    const struct cue_track *t = &d->cue->tracks[i];
    uint32_t data_first = t->start - t->pregap;
    uint32_t region_first = data_first - t->silence;

    if (lba >= region_first && lba < t->start + t->length) {
      *in_file = lba >= data_first;
      *offset = t->file_offset + (uint64_t)(lba - data_first) * CD_FRAME;
      return t;
    }
  }
  return NULL;
}

static void make_subchannel(const struct cue_track *t, uint32_t lba, uint8_t pw[CD_SUB]) {
  uint8_t q[12];
  int in_pregap = lba < t->start;
  int i;

  cd_make_q(q, t->control, t->number, in_pregap ? 0 : 1, in_pregap ? t->start - lba : lba - t->start, lba);
  for (i = 0; i < CD_SUB; i++) {
    pw[i] = (uint8_t)((in_pregap ? 0x80 : 0) | (((q[i / 8] >> (7 - i % 8)) & 1) << 6));
  }
}

static int read_cd_frame(struct image_drive *d, uint32_t lba, uint8_t frame[CD_FRAME], uint8_t pw[CD_SUB]) {
  const struct cue_track *t;
  uint64_t offset = 0;
  int in_file = 0;

  t = locate(d, lba, &in_file, &offset);
  if (!t) {
    return RIP_ERR;
  }
  if (!in_file) {
    memset(frame, 0, CD_FRAME);
  } else if (rip_seek_read(d->fds[t->file], offset, frame, CD_FRAME) != 0) {
    return RIP_ERR;
  }
  if (pw) {
    if (d->sub_fd >= 0 && in_file) {
      if (rip_seek_read(d->sub_fd, offset / CD_FRAME * CD_SUB, pw, CD_SUB) != 0) {
        make_subchannel(t, lba, pw);
      }
    } else {
      make_subchannel(t, lba, pw);
    }
  }
  return RIP_OK;
}

// The 2048 user bytes of a data frame, as READ(10)/(12) returns them.
static int read_cd_data(struct image_drive *d, uint32_t lba, uint8_t out[CD_DATA]) {
  uint8_t frame[CD_FRAME];

  if (read_cd_frame(d, lba, frame, NULL) != RIP_OK) {
    return RIP_ERR;
  }
  switch (frame[15]) {
  case 1:
    memcpy(out, frame + 16, CD_DATA);
    return RIP_OK;
  case 2:
    memcpy(out, frame + 24, CD_DATA);
    return RIP_OK;
  default:
    return RIP_ERR;
  }
}

/* Commands */

static size_t put_feature(uint8_t *p, uint16_t code, const uint8_t *data, uint8_t len) {
  rip_put_be16(p, code);
  p[2] = 0x01;
  p[3] = len;
  memcpy(p + 4, data, len);
  return 4u + len;
}

static int get_configuration(struct image_drive *d, const uint8_t *cdb, uint8_t *buf, size_t len) {
  static const uint16_t sony[] = {MMC_PROFILE_PS2_DVD, MMC_PROFILE_PS3_DVD, MMC_PROFILE_PS3_BD, MMC_PROFILE_PS4_BD,
                                  MMC_PROFILE_PS5_BD};
  uint16_t start = rip_be16(cdb + 2);
  uint8_t out[256];
  size_t n = 8;

  memset(out, 0, sizeof(out));
  rip_put_be16(out + 6, current_profile(d));
  if (start == MMC_FEATURE_PROFILE_LIST) {
    uint8_t list[64];
    uint8_t l = 0;
    size_t i;
    if (d->opts.cd) {
      rip_put_be16(list + l, MMC_PROFILE_CD_ROM);
      l += 4;
    }
    rip_put_be16(list + l, MMC_PROFILE_DVD_ROM);
    l += 4;
    rip_put_be16(list + l, MMC_PROFILE_BD_ROM);
    l += 4;
    for (i = 0; d->opts.sony && i < RIP_ARRAY_LEN(sony); i++) {
      rip_put_be16(list + l, sony[i]);
      l += 4;
    }
    for (i = 0; i < l; i += 4) {
      list[i + 2] = rip_be16(list + i) == current_profile(d);
      list[i + 3] = 0;
    }
    n += put_feature(out + n, MMC_FEATURE_PROFILE_LIST, list, l);
  } else if ((start == MMC_FEATURE_CD_READ && d->opts.cd) || start == MMC_FEATURE_DVD_READ ||
             start == MMC_FEATURE_BD_READ) {
    uint8_t data[4] = {start == MMC_FEATURE_CD_READ ? 0x02 : 0, 0, 0, 0};  // C2 pointers
    n += put_feature(out + n, start, data, sizeof(data));
  }
  rip_put_be32(out, (uint32_t)(n - 4));
  memcpy(buf, out, len < n ? len : n);
  return RIP_OK;
}

static void put_msf(uint8_t *p, uint32_t lba) {
  uint32_t f = lba + CD_PREGAP;

  p[0] = (uint8_t)(f / (60 * CD_FRAMES_PER_SEC));
  p[1] = (uint8_t)((f / CD_FRAMES_PER_SEC) % 60);
  p[2] = (uint8_t)(f % CD_FRAMES_PER_SEC);
}

static uint32_t session_leadout(const struct cue_sheet *cue, uint8_t session) {
  unsigned i;
  uint32_t end = cue->leadout;

  for (i = 0; i < cue->count; i++) {
    if (cue->tracks[i].session == session) {
      end = cue->tracks[i].start + cue->tracks[i].length;
    }
  }
  return end;
}

static int read_toc(struct image_drive *d, const uint8_t *cdb, uint8_t *buf, size_t len, struct rip_sense *sense) {
  uint8_t out[4 + 11 * 110];
  uint8_t format = cdb[2] & 0x0F;
  size_t n = 4;
  unsigned i;
  struct cue_sheet single;
  const struct cue_sheet *cue = d->cue;

  if (!d->is_cd) {
    // A DVD or BD answers with one data track.
    memset(&single, 0, sizeof(single));
    single.count = 1;
    single.tracks[0].number = 1;
    single.tracks[0].control = CD_CTRL_DATA;
    single.tracks[0].session = 1;
    single.tracks[0].length = (uint32_t)(d->size / CD_DATA);
    single.leadout = single.tracks[0].length;
    cue = &single;
  }
  memset(out, 0, sizeof(out));
  if (format == 0) {
    out[2] = cue->tracks[0].number;
    out[3] = cue->tracks[cue->count - 1].number;
    for (i = 0; i < cue->count; i++, n += 8) {
      out[n + 1] = (uint8_t)(0x10 | cue->tracks[i].control);
      out[n + 2] = cue->tracks[i].number;
      rip_put_be32(out + n + 4, cue->tracks[i].start);
    }
    out[n + 1] = 0x10 | (cue->tracks[cue->count - 1].control & CD_CTRL_DATA);
    out[n + 2] = 0xAA;
    rip_put_be32(out + n + 4, cue->leadout);
    n += 8;
  } else if (format == 1) {
    const struct cue_track *last_first = &cue->tracks[0];
    for (i = 0; i < cue->count; i++) {
      if (cue->tracks[i].session != cue->tracks[0].session &&
          (i == 0 || cue->tracks[i - 1].session != cue->tracks[i].session)) {
        last_first = &cue->tracks[i];
      }
    }
    out[2] = 1;
    out[3] = cue->tracks[cue->count - 1].session;
    out[n + 1] = (uint8_t)(0x10 | last_first->control);
    out[n + 2] = last_first->number;
    rip_put_be32(out + n + 4, last_first->start);
    n += 8;
  } else if (format == 2) {
    uint8_t sessions = cue->tracks[cue->count - 1].session;
    uint8_t s;
    out[2] = 1;
    out[3] = sessions;
    for (s = 1; s <= sessions; s++) {
      const struct cue_track *first = NULL;
      const struct cue_track *last = NULL;
      for (i = 0; i < cue->count; i++) {
        if (cue->tracks[i].session == s) {
          if (!first) {
            first = &cue->tracks[i];
          }
          last = &cue->tracks[i];
        }
      }
      if (!first) {
        continue;
      }
      // A0: first track and disc type; A1: last track; A2: lead-out.
      out[n] = s;
      out[n + 1] = (uint8_t)(0x10 | first->control);
      out[n + 3] = 0xA0;
      out[n + 8] = first->number;
      out[n + 9] = (first->control & CD_CTRL_DATA) && first->mode == CD_MODE2 ? CD_TYPE_XA : CD_TYPE_DA_ROM;
      n += 11;
      out[n] = s;
      out[n + 1] = (uint8_t)(0x10 | last->control);
      out[n + 3] = 0xA1;
      out[n + 8] = last->number;
      n += 11;
      out[n] = s;
      out[n + 1] = (uint8_t)(0x10 | (last->control & CD_CTRL_DATA));
      out[n + 3] = 0xA2;
      put_msf(out + n + 8, session_leadout(cue, s));
      n += 11;
      for (i = 0; i < cue->count; i++) {
        if (cue->tracks[i].session != s) {
          continue;
        }
        out[n] = s;
        out[n + 1] = (uint8_t)(0x10 | cue->tracks[i].control);
        out[n + 3] = cue->tracks[i].number;
        put_msf(out + n + 8, cue->tracks[i].start);
        n += 11;
      }
    }
  } else {
    return sense_of(sense, 0x05, 0x24);
  }
  rip_put_be16(out, (uint16_t)(n - 2));
  memset(buf, 0, len);
  memcpy(buf, out, len < n ? len : n);
  return RIP_OK;
}

static int read_cd(struct image_drive *d, const uint8_t *cdb, uint8_t *buf, size_t len, struct rip_sense *sense) {
  uint32_t lba = rip_be32(cdb + 2);
  uint32_t count = ((uint32_t)cdb[6] << 16) | ((uint32_t)cdb[7] << 8) | cdb[8];
  int raw = (cdb[9] & 0xF8) == 0xF8;
  int user_only = (cdb[9] & 0xF8) == 0x10;
  int c2 = (cdb[9] & 0x06) != 0;
  size_t c2_len = (cdb[9] & 0x06) == 0x04 ? 296 : CD_C2;
  int sub = cdb[10] == 0x01;
  size_t stride;
  uint32_t i;

  if (!d->is_cd) {
    return sense_of(sense, 0x05, 0x64);
  }
  if (!raw && !user_only) {
    return sense_of(sense, 0x05, 0x24);
  }
  stride = (raw ? CD_FRAME : 0) + (c2 ? c2_len : 0) + (sub ? CD_SUB : 0);
  if (user_only) {
    stride = 0;  // decided per frame below
  }
  for (i = 0; i < count; i++) {
    uint8_t frame[CD_FRAME];
    uint8_t pw[CD_SUB];
    uint8_t *p;

    if (read_cd_frame(d, lba + i, frame, sub ? pw : NULL) != RIP_OK) {
      return sense_of(sense, 0x05, 0x21);
    }
    if (user_only) {
      // Data frames give their 2048 bytes, audio its whole frame.
      size_t off = frame[15] == 2 ? 24 : 16;
      int audio = memcmp(frame, "\x00\xff\xff\xff\xff\xff\xff\xff\xff\xff\xff\x00", 12) != 0;
      size_t user = audio ? CD_FRAME : CD_DATA;
      if ((size_t)(i + 1) * user > len) {
        return sense_of(sense, 0x05, 0x24);
      }
      memcpy(buf + (size_t)i * user, audio ? frame : frame + off, user);
      continue;
    }
    if ((size_t)(i + 1) * stride > len) {
      return sense_of(sense, 0x05, 0x24);
    }
    p = buf + (size_t)i * stride;
    memcpy(p, frame, CD_FRAME);
    p += CD_FRAME;
    if (c2) {
      memset(p, 0, c2_len);
      p += c2_len;
    }
    if (sub) {
      memcpy(p, pw, CD_SUB);
    }
  }
  return RIP_OK;
}

static int read_blocks(struct image_drive *d, uint32_t lba, uint32_t count, uint8_t *buf, size_t len,
                       struct rip_sense *sense) {
  uint32_t i;

  if ((size_t)count * CD_DATA > len) {
    return sense_of(sense, 0x05, 0x24);
  }
  if (!d->is_cd) {
    if ((uint64_t)(lba + (uint64_t)count) * CD_DATA > d->size) {
      return sense_of(sense, 0x05, 0x21);
    }
    return rip_seek_read(d->fd, (uint64_t)lba * CD_DATA, buf, (size_t)count * CD_DATA) == 0
               ? RIP_OK
               : sense_of(sense, 0x03, 0x11);
  }
  for (i = 0; i < count; i++) {
    if (read_cd_data(d, lba + i, buf + (size_t)i * CD_DATA) != RIP_OK) {
      return sense_of(sense, 0x05, 0x64);  // illegal mode for this track (audio)
    }
  }
  return RIP_OK;
}

static int exec_locked(struct image_drive *d, const uint8_t *cdb, uint8_t *buf, size_t len, struct rip_sense *sense) {
  switch (cdb[0]) {
  case 0x12: {  // INQUIRY
    uint8_t inq[36];
    memset(inq, ' ', sizeof(inq));
    inq[0] = 0x05;
    inq[1] = 0x80;  // removable
    memcpy(inq + 8, "RIPPER  ", 8);
    memcpy(inq + 16, "VIRTUAL DRIVE   ", 16);
    memcpy(inq + 32, "1.00", 4);
    memcpy(buf, inq, len < sizeof(inq) ? len : sizeof(inq));
    return RIP_OK;
  }
  case 0x00:  // TEST UNIT READY
    return d->loaded ? RIP_OK : sense_of(sense, 0x02, 0x3A);
  case 0x46:  // GET CONFIGURATION
    if (!d->loaded && rip_be16(cdb + 2) != MMC_FEATURE_PROFILE_LIST && rip_be16(cdb + 2) != MMC_FEATURE_CD_READ &&
        rip_be16(cdb + 2) != MMC_FEATURE_DVD_READ && rip_be16(cdb + 2) != MMC_FEATURE_BD_READ) {
      return sense_of(sense, 0x02, 0x3A);
    }
    return get_configuration(d, cdb, buf, len);
  case 0x1B:  // START STOP UNIT: eject when LoEj is set and Start is not
    if ((cdb[4] & 0x03) == 0x02) {
      unload(d);
    }
    return RIP_OK;
  case 0x1E:  // PREVENT ALLOW MEDIUM REMOVAL
  case 0xB6:  // SET STREAMING
  case 0xBB:  // SET CD SPEED
    return RIP_OK;
  }
  if (!d->loaded) {
    return sense_of(sense, 0x02, 0x3A);
  }
  switch (cdb[0]) {
  case 0x25: {  // READ CAPACITY
    uint32_t blocks = d->is_cd ? d->cue->leadout : (uint32_t)(d->size / CD_DATA);
    rip_put_be32(buf, blocks ? blocks - 1 : 0);
    rip_put_be32(buf + 4, CD_DATA);
    return RIP_OK;
  }
  case 0x28:  // READ(10)
    return read_blocks(d, rip_be32(cdb + 2), rip_be16(cdb + 7), buf, len, sense);
  case 0xA8:  // READ(12)
    return read_blocks(d, rip_be32(cdb + 2), rip_be32(cdb + 6), buf, len, sense);
  case 0x43:  // READ TOC
    return read_toc(d, cdb, buf, len, sense);
  case 0xBE:  // READ CD
    return read_cd(d, cdb, buf, len, sense);
  case 0xAD:  // READ DISC STRUCTURE: no copy protection
    memset(buf, 0, len);
    return d->is_cd ? sense_of(sense, 0x05, 0x30) : RIP_OK;
  default:
    return sense_of(sense, 0x05, 0x20);
  }
}

static int image_exec(void *ctx, const uint8_t *cdb, size_t cdb_len, enum rip_dir dir, void *buf, size_t len,
                      unsigned timeout_ms, struct rip_sense *sense) {
  struct image_drive *d = ctx;
  int rc;

  (void)cdb_len;
  (void)dir;
  (void)timeout_ms;
  pthread_mutex_lock(&d->lock);
  rc = exec_locked(d, cdb, buf, len, sense);
  pthread_mutex_unlock(&d->lock);
  return rc;
}

static void image_close(void *ctx) {
  struct image_drive *d = ctx;

  unload(d);
  pthread_mutex_destroy(&d->lock);
  free(d);
}

static const struct rip_transport_ops image_ops = {image_exec, image_close};

int rip_image_drive_open(struct rip_transport *t, const struct rip_image_drive_opts *opts) {
  struct image_drive *d = calloc(1, sizeof(*d));

  memset(t, 0, sizeof(*t));
  if (!d) {
    return RIP_ENOMEM;
  }
  pthread_mutex_init(&d->lock, NULL);
  d->fd = -1;
  d->sub_fd = -1;
  if (opts) {
    d->opts = *opts;
  } else {
    d->opts.cd = 1;
  }
  t->ops = &image_ops;
  t->ctx = d;
  snprintf(t->name, sizeof(t->name), "virtual drive");
  return RIP_OK;
}

static int ends_with(const char *s, const char *suffix) {
  size_t a = strlen(s);
  size_t b = strlen(suffix);
  size_t i;

  if (a < b) {
    return 0;
  }
  for (i = 0; i < b; i++) {
    char c = s[a - b + i];
    if (c >= 'A' && c <= 'Z') {
      c = (char)(c - 'A' + 'a');
    }
    if (c != suffix[i]) {
      return 0;
    }
  }
  return 1;
}

static int load_cd(struct image_drive *d, const char *path) {
  unsigned i;

  d->cue = malloc(sizeof(*d->cue));
  if (!d->cue) {
    return RIP_ENOMEM;
  }
  if (cue_load(path, d->cue) != RIP_OK) {
    free(d->cue);
    d->cue = NULL;
    return RIP_ERR;
  }
  for (i = 0; i < CD_MAX_TRACKS; i++) {
    d->fds[i] = -1;
  }
  for (i = 0; i < d->cue->file_count; i++) {
    d->fds[i] = open(d->cue->files[i], O_RDONLY);
    if (d->fds[i] < 0) {
      RIP_ERROR("cannot open %s", d->cue->files[i]);
      return RIP_ERR;
    }
  }
  if (d->cue->file_count == 1) {
    char sub[1100];
    char *dot;
    snprintf(sub, sizeof(sub), "%s", d->cue->files[0]);
    dot = strrchr(sub, '.');
    if (dot) {
      snprintf(dot, sizeof(sub) - (size_t)(dot - sub), ".sub");
      d->sub_fd = open(sub, O_RDONLY);
    }
  }
  d->is_cd = 1;
  return RIP_OK;
}

int rip_image_drive_load(struct rip_transport *t, const char *path) {
  struct image_drive *d = t->ctx;
  int rc = RIP_OK;

  pthread_mutex_lock(&d->lock);
  unload(d);
  if (ends_with(path, ".cue")) {
    if (!d->opts.cd) {
      RIP_ERROR("this virtual drive has no CD laser");
      rc = RIP_ENOTSUP;
    } else {
      rc = load_cd(d, path);
    }
  } else {
    struct stat st;
    d->fd = open(path, O_RDONLY);
    if (d->fd < 0 || fstat(d->fd, &st) != 0 || st.st_size < CD_DATA) {
      RIP_ERROR("cannot open %s", path);
      rc = RIP_ERR;
    } else {
      d->size = (uint64_t)st.st_size;
    }
  }
  if (rc == RIP_OK) {
    d->loaded = 1;
    snprintf(d->path, sizeof(d->path), "%s", path);
    RIP_INFO("virtual drive: loaded %s", path);
  } else {
    unload(d);
  }
  pthread_mutex_unlock(&d->lock);
  return rc;
}

void rip_image_drive_eject(struct rip_transport *t) {
  struct image_drive *d = t->ctx;

  pthread_mutex_lock(&d->lock);
  unload(d);
  pthread_mutex_unlock(&d->lock);
}

const char *rip_image_drive_path(struct rip_transport *t) {
  struct image_drive *d = t->ctx;

  return d->path;
}
