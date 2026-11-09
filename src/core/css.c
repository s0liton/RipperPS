// CSS through libdvdcss: its ioctl.h, sent over ripper's transport, and a
// reader that descrambles the VOB files of a DVD-Video disc as it reads.

#include "ripper/css.h"

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/uio.h>

#include "css.h"  // libdvdcss's: key sizes
#include "dvdcss/dvdcss.h"
#include "ioctl.h"  // libdvdcss's, implemented below
#include "ripper/common.h"
#include "ripper/log.h"
#include "ripper/udf.h"
#include "ripper/util.h"

#define KEY_TIMEOUT_MS 10000u
#define MAX_VOBS 128
#define DVD_CHALLENGE_SIZE 10  // ioctl.c has it, and ioctl.c is left out

/* libdvdcss's ioctl.h on the transport */

// libdvdcss keeps no per-handle place for us, and its ioctls only get a file
// descriptor: one CSS session at a time, its transport here.
static pthread_mutex_t g_session = PTHREAD_MUTEX_INITIALIZER;
static struct rip_transport *g_t;

static int report_key(int agid, uint8_t format, uint32_t lba, uint8_t *buf, uint16_t len) {
  uint8_t cdb[12] = {0xA4, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0};

  rip_put_be32(cdb + 2, lba);
  rip_put_be16(cdb + 8, len);
  cdb[10] = (uint8_t)((agid << 6) | format);
  if (buf) {
    memset(buf, 0, len);
  }
  return rip_scsi(g_t, cdb, sizeof(cdb), len ? RIP_DIR_IN : RIP_DIR_NONE, buf, len, KEY_TIMEOUT_MS, NULL) == RIP_OK
             ? 0
             : -1;
}

static int send_key(int agid, uint8_t format, uint8_t *buf, uint16_t len) {
  uint8_t cdb[12] = {0xA3, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0};

  rip_put_be16(cdb + 8, len);
  cdb[10] = (uint8_t)((agid << 6) | format);
  return rip_scsi(g_t, cdb, sizeof(cdb), RIP_DIR_OUT, buf, len, KEY_TIMEOUT_MS, NULL) == RIP_OK ? 0 : -1;
}

static int read_structure(int layer, uint8_t format, int agid, uint8_t *buf, uint16_t len) {
  uint8_t cdb[12] = {0xAD, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0};

  cdb[6] = (uint8_t)layer;
  cdb[7] = format;
  rip_put_be16(cdb + 8, len);
  cdb[10] = (uint8_t)(agid << 6);
  memset(buf, 0, len);
  return rip_scsi(g_t, cdb, sizeof(cdb), RIP_DIR_IN, buf, len, KEY_TIMEOUT_MS, NULL) == RIP_OK ? 0 : -1;
}

int ioctl_ReadCopyright(int i_fd, int i_layer, int *pi_copyright) {
  uint8_t buf[8];
  int rc = read_structure(i_layer, DVD_STRUCT_COPYRIGHT, 0, buf, sizeof(buf));

  (void)i_fd;
  if (rc == 0) {
    *pi_copyright = buf[4];
  }
  return rc;
}

int ioctl_ReadDiscKey(int i_fd, const int *pi_agid, uint8_t *p_key) {
  uint8_t *buf = malloc(DVD_DISCKEY_SIZE + 4);
  int rc;

  (void)i_fd;
  if (!buf) {
    return -1;
  }
  rc = read_structure(0, DVD_STRUCT_DISCKEY, *pi_agid, buf, DVD_DISCKEY_SIZE + 4);
  if (rc == 0) {
    memcpy(p_key, buf + 4, DVD_DISCKEY_SIZE);
  }
  free(buf);
  return rc;
}

int ioctl_ReadTitleKey(int i_fd, const int *pi_agid, int i_pos, uint8_t *p_key) {
  uint8_t buf[12];
  int rc = report_key(*pi_agid, DVD_REPORT_TITLE_KEY, (uint32_t)i_pos, buf, sizeof(buf));

  (void)i_fd;
  memcpy(p_key, buf + 5, DVD_KEY_SIZE);
  return rc;
}

int ioctl_ReportAgid(int i_fd, int *pi_agid) {
  uint8_t buf[8];
  int rc = report_key(*pi_agid, DVD_REPORT_AGID, 0, buf, sizeof(buf));

  (void)i_fd;
  *pi_agid = buf[7] >> 6;
  return rc;
}

int ioctl_ReportChallenge(int i_fd, const int *pi_agid, uint8_t *p_challenge) {
  uint8_t buf[16];
  int rc = report_key(*pi_agid, DVD_REPORT_CHALLENGE, 0, buf, sizeof(buf));

  (void)i_fd;
  memcpy(p_challenge, buf + 4, DVD_CHALLENGE_SIZE);
  return rc;
}

int ioctl_ReportKey1(int i_fd, const int *pi_agid, uint8_t *p_key) {
  uint8_t buf[12];
  int rc = report_key(*pi_agid, DVD_REPORT_KEY1, 0, buf, sizeof(buf));

  (void)i_fd;
  memcpy(p_key, buf + 4, DVD_KEY_SIZE);
  return rc;
}

int ioctl_ReportASF(int i_fd, int *pi_asf) {
  uint8_t buf[8];
  int rc = report_key(0, DVD_REPORT_ASF, 0, buf, sizeof(buf));

  (void)i_fd;
  *pi_asf = buf[7] & 1;
  return rc;
}

int ioctl_InvalidateAgid(int i_fd, int *pi_agid) {
  (void)i_fd;
  return report_key(*pi_agid, DVDCSS_INVALIDATE_AGID, 0, NULL, 0);
}

int ioctl_SendChallenge(int i_fd, const int *pi_agid, const uint8_t *p_challenge) {
  uint8_t buf[16];

  (void)i_fd;
  memset(buf, 0, sizeof(buf));
  buf[1] = 0x0E;
  memcpy(buf + 4, p_challenge, DVD_CHALLENGE_SIZE);
  return send_key(*pi_agid, DVD_SEND_CHALLENGE, buf, sizeof(buf));
}

int ioctl_SendKey2(int i_fd, const int *pi_agid, const uint8_t *p_key) {
  uint8_t buf[12];

  (void)i_fd;
  memset(buf, 0, sizeof(buf));
  buf[1] = 0x0A;
  memcpy(buf + 4, p_key, DVD_KEY_SIZE);
  return send_key(*pi_agid, DVD_SEND_KEY2, buf, sizeof(buf));
}

int ioctl_ReportRPC(int i_fd, int *p_type, int *p_mask, int *p_scheme) {
  uint8_t buf[8];
  int rc = report_key(0, DVD_REPORT_RPC, 0, buf, sizeof(buf));

  (void)i_fd;
  *p_type = buf[4] >> 6;
  *p_mask = buf[5];
  *p_scheme = buf[6];
  return rc;
}

/* The reader */

struct vob {
  uint32_t first;
  uint32_t end;  // exclusive
};

struct css_ctx {
  struct rip_reader *base;
  dvdcss_t h;
  uint64_t pos;  // the stream's position, in bytes
  struct vob vobs[MAX_VOBS];
  unsigned vob_count;
  int keyed;      // the VOB whose title key is loaded, -1 for none
  uint64_t next;  // the sector libdvdcss is positioned at
};

static int stream_seek(void *p, uint64_t pos) {
  ((struct css_ctx *)p)->pos = pos;
  return 0;
}

static int stream_read(void *p, void *buf, int bytes) {
  struct css_ctx *c = p;
  uint32_t blocks = (uint32_t)bytes / DVDCSS_BLOCK_SIZE;

  if (blocks == 0 || rip_reader_read(c->base, c->pos / DVDCSS_BLOCK_SIZE, blocks, buf) != RIP_OK) {
    return -1;
  }
  c->pos += (uint64_t)blocks * DVDCSS_BLOCK_SIZE;
  return (int)(blocks * DVDCSS_BLOCK_SIZE);
}

static int stream_readv(void *p, const void *iov, int count) {
  const struct iovec *v = iov;
  int total = 0;
  int i;

  for (i = 0; i < count; i++) {
    int n = stream_read(p, v[i].iov_base, (int)v[i].iov_len);
    if (n < 0) {
      return total ? total : -1;
    }
    total += n;
  }
  return total;
}

static dvdcss_stream_cb g_stream_cb = {stream_seek, stream_read, stream_readv};

static int vob_at(const struct css_ctx *c, uint64_t lba) {
  unsigned i;

  for (i = 0; i < c->vob_count; i++) {
    if (lba >= c->vobs[i].first && lba < c->vobs[i].end) {
      return (int)i;
    }
  }
  return -1;
}

// Sectors [lba, lba+count) all in VOB v (or none: v < 0).
static int read_run(struct css_ctx *c, int v, uint64_t lba, uint32_t count, uint8_t *buf) {
  int got;

  if (v < 0) {
    return rip_reader_read(c->base, lba, count, buf);
  }
  if (c->keyed != v) {
    // The VOB's title key, asked for at its first sector, then back to where we read.
    if (dvdcss_seek(c->h, (int)c->vobs[v].first, DVDCSS_SEEK_KEY) < 0) {
      RIP_ERROR("CSS: no title key for the VOB at sector %u: %s", c->vobs[v].first, dvdcss_error(c->h));
      return RIP_ERR;
    }
    c->keyed = v;
    c->next = c->vobs[v].first;
  }
  if (c->next != lba && dvdcss_seek(c->h, (int)lba, DVDCSS_NOFLAGS) < 0) {
    return RIP_ERR;
  }
  got = dvdcss_read(c->h, buf, (int)count, DVDCSS_READ_DECRYPT);
  c->next = lba + (got > 0 ? (uint64_t)got : 0);
  return got == (int)count ? RIP_OK : RIP_ERR;
}

static int css_read(void *ctx, uint64_t lba, uint32_t count, void *buf) {
  struct css_ctx *c = ctx;
  uint8_t *out = buf;

  while (count) {
    int v = vob_at(c, lba);
    uint32_t run = count;
    unsigned i;
    // Up to the end of this VOB, or the start of the next one.
    if (v >= 0 && c->vobs[v].end - lba < run) {
      run = (uint32_t)(c->vobs[v].end - lba);
    }
    for (i = 0; v < 0 && i < c->vob_count; i++) {
      if (c->vobs[i].first > lba && c->vobs[i].first - lba < run) {
        run = (uint32_t)(c->vobs[i].first - lba);
      }
    }
    if (read_run(c, v, lba, run, out) != RIP_OK) {
      return RIP_ERR;
    }
    lba += run;
    out += (size_t)run * DVDCSS_BLOCK_SIZE;
    count -= run;
  }
  return RIP_OK;
}

static void css_close(void *ctx) {
  struct css_ctx *c = ctx;

  dvdcss_close(c->h);
  g_t = NULL;
  pthread_mutex_unlock(&g_session);
  free(c);
}

static const struct rip_reader_ops css_ops = {css_read, css_close};

struct vob_list {
  struct css_ctx *c;
};

static int collect_vob(const char *name, const struct udf_entry *e, void *user) {
  struct css_ctx *c = ((struct vob_list *)user)->c;
  size_t n = strlen(name);
  unsigned i;

  if (n < 4 || strcasecmp(name + n - 4, ".VOB") != 0 || e->extent_count == 0 || c->vob_count >= MAX_VOBS) {
    return 0;
  }
  c->vobs[c->vob_count].first = e->extents[0].lba;
  c->vobs[c->vob_count].end = e->extents[0].lba;
  for (i = 0; i < e->extent_count; i++) {
    uint32_t end = e->extents[i].lba + (e->extents[i].length + DVDCSS_BLOCK_SIZE - 1) / DVDCSS_BLOCK_SIZE;
    if (end > c->vobs[c->vob_count].end) {
      c->vobs[c->vob_count].end = end;
    }
  }
  c->vob_count++;
  return 0;
}

int rip_reader_open_css(struct rip_reader *out, struct rip_reader *base, struct rip_transport *t,
                        const char *cache_dir) {
  struct css_ctx *c = calloc(1, sizeof(*c));
  struct udf_fs *fs = malloc(sizeof(*fs));
  struct udf_entry dir;
  struct vob_list list;

  memset(out, 0, sizeof(*out));
  if (!c || !fs) {
    free(c);
    free(fs);
    return RIP_ENOMEM;
  }
  c->base = base;
  c->keyed = -1;
  // Where the VOB files are: their sectors are the scrambled ones.
  list.c = c;
  if (udf_mount(fs, base) == RIP_OK && udf_lookup(fs, "VIDEO_TS", &dir) == RIP_OK) {
    udf_list(fs, &dir, collect_vob, &list);
  }
  free(fs);
  RIP_INFO("CSS: %u VOB files", c->vob_count);

  pthread_mutex_lock(&g_session);
  g_t = t;
  if (cache_dir && cache_dir[0] && rip_mkdir_p(cache_dir) == 0) {
    setenv("DVDCSS_CACHE", cache_dir, 1);
  } else {
    setenv("DVDCSS_CACHE", "off", 1);
  }
  c->h = dvdcss_open_stream(c, &g_stream_cb);
  if (!c->h) {
    RIP_ERROR("CSS: libdvdcss could not open the disc");
    g_t = NULL;
    pthread_mutex_unlock(&g_session);
    free(c);
    return RIP_ERR;
  }
  if (!dvdcss_is_scrambled(c->h)) {
    RIP_INFO("CSS: the disc isn't scrambled after all");
  }
  out->ops = &css_ops;
  out->ctx = c;
  out->sector_size = base->sector_size;
  out->sector_count = base->sector_count;
  out->max_sectors = base->max_sectors;
  return RIP_OK;
}
