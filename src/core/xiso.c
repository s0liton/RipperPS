#include "ripper/xiso.h"

#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "ripper/common.h"
#include "ripper/log.h"
#include "ripper/util.h"

#define SECTOR 2048u
#define VOLUME_SECTOR 32u
#define MAGIC "MICROSOFT*XBOX*MEDIA"
#define MAX_DEPTH 64
// Directory tables larger than this aren't an Xbox game's.
#define MAX_TABLE (16u << 20)

// The game partition's place in a full image: none (already an XISO), then
// XGD1 (Xbox), XGD2 and XGD3 (Xbox 360), as Redump lays them out.
static const uint64_t OFFSETS[] = {0, 0x18300000ull, 0xFD90000ull, 0x2080000ull};

int xiso_partition(int fd, uint64_t *offset) {
  uint8_t vd[SECTOR];
  size_t i;

  for (i = 0; i < RIP_ARRAY_LEN(OFFSETS); i++) {
    if (rip_seek_read(fd, OFFSETS[i] + (uint64_t)VOLUME_SECTOR * SECTOR, vd, sizeof(vd)) == 0 &&
        !memcmp(vd, MAGIC, 20) && !memcmp(vd + 0x7EC, MAGIC, 20)) {
      *offset = OFFSETS[i];
      return RIP_OK;
    }
  }
  return RIP_ENOENT;
}

struct walk {
  int fd;
  uint64_t offset;
  uint64_t end;
  int bad;
};

static void extend(struct walk *w, uint32_t sector, uint32_t size) {
  uint64_t end = (uint64_t)sector * SECTOR + (size + SECTOR - 1) / SECTOR * SECTOR;

  if (end > w->end) {
    w->end = end;
  }
}

static void table(struct walk *w, uint32_t sector, uint32_t size, int depth);

// One entry of a directory's tree and the ones under it: left and right are
// where the next entries are, in 4-byte steps from the table's start.
static void entry(struct walk *w, const uint8_t *t, uint32_t len, uint32_t at, int depth, unsigned *visits) {
  const uint8_t *e = t + (size_t)at * 4;
  uint16_t left;
  uint16_t right;
  uint32_t sector;
  uint32_t size;

  if (w->bad || ++*visits > len / 14 + 1 || (uint64_t)at * 4 + 14 > len) {
    w->bad = 1;
    return;
  }
  left = rip_le16(e);
  right = rip_le16(e + 2);
  if (left == 0xFFFF) {
    return;  // the padding at a sector's end, or an empty table
  }
  sector = rip_le32(e + 4);
  size = rip_le32(e + 8);
  extend(w, sector, size);
  if ((e[12] & 0x10) && size) {
    table(w, sector, size, depth + 1);
  }
  if (left) {
    entry(w, t, len, left, depth, visits);
  }
  if (right) {
    entry(w, t, len, right, depth, visits);
  }
}

static void table(struct walk *w, uint32_t sector, uint32_t size, int depth) {
  uint8_t *t;
  unsigned visits = 0;

  if (depth > MAX_DEPTH || size > MAX_TABLE) {
    w->bad = 1;
    return;
  }
  t = malloc(size);
  if (!t) {
    w->bad = 1;
    return;
  }
  if (rip_seek_read(w->fd, w->offset + (uint64_t)sector * SECTOR, t, size) != 0) {
    w->bad = 1;
  } else {
    entry(w, t, size, 0, depth, &visits);
  }
  free(t);
}

int xiso_used(int fd, uint64_t offset, uint64_t *length) {
  uint8_t vd[SECTOR];
  struct walk w;
  uint32_t root;
  uint32_t size;

  if (rip_seek_read(fd, offset + (uint64_t)VOLUME_SECTOR * SECTOR, vd, sizeof(vd)) != 0) {
    return RIP_ERR;
  }
  root = rip_le32(vd + 0x14);
  size = rip_le32(vd + 0x18);
  memset(&w, 0, sizeof(w));
  w.fd = fd;
  w.offset = offset;
  w.end = (uint64_t)(VOLUME_SECTOR + 2) * SECTOR;
  extend(&w, root, size);
  if (size) {
    table(&w, root, size, 0);
  }
  if (w.bad) {
    return RIP_ERR;
  }
  *length = w.end;
  return RIP_OK;
}

int xiso_from_iso(const char *iso_path, const char *out_path, iso_progress_fn progress, void *ud) {
  uint64_t offset = 0;
  uint64_t length = 0;
  uint64_t done = 0;
  uint8_t *buf = NULL;
  int in = open(iso_path, O_RDONLY);
  int out = -1;
  int rc;

  if (in < 0) {
    RIP_ERROR("xiso: can't open %s: %s", iso_path, strerror(errno));
    return RIP_ERR;
  }
  rc = xiso_partition(in, &offset);
  if (rc != RIP_OK) {
    RIP_ERROR("xiso: no Xbox game partition in %s", iso_path);
    close(in);
    return rc;
  }
  rc = xiso_used(in, offset, &length);
  if (rc != RIP_OK) {
    RIP_ERROR("xiso: the game partition's directory tables in %s don't hold up", iso_path);
    close(in);
    return rc;
  }
  RIP_INFO("xiso: game partition at %llu, %llu MiB used", (unsigned long long)offset,
           (unsigned long long)(length >> 20));
  buf = malloc(4u << 20);
  out = open(out_path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
  if (!buf || out < 0) {
    RIP_ERROR("xiso: can't create %s: %s", out_path, strerror(errno));
    rc = buf ? RIP_ERR : RIP_ENOMEM;
  }
  while (rc == RIP_OK && done < length) {
    size_t n = length - done < (4u << 20) ? (size_t)(length - done) : (4u << 20);
    if (rip_seek_read(in, offset + done, buf, n) != 0) {
      // Past the image's end (a short burned copy): the rest is zeros.
      ssize_t got = pread(in, buf, n, (off_t)(offset + done));
      memset(buf + (got > 0 ? got : 0), 0, n - (size_t)(got > 0 ? got : 0));
    }
    if (rip_write_all(out, buf, n) != 0) {
      RIP_ERROR("xiso: write to %s failed: %s", out_path, strerror(errno));
      rc = RIP_ERR;
      break;
    }
    done += n;
    if (progress && progress(ud, done, length)) {
      rc = RIP_ECANCEL;
    }
  }
  if (out >= 0 && rc == RIP_OK && fsync(out) != 0) {
    RIP_ERROR("xiso: can't finish %s: %s", out_path, strerror(errno));
    rc = RIP_ERR;
  }
  if (out >= 0 && close(out) != 0 && rc == RIP_OK) {
    rc = RIP_ERR;
  }
  if (rc != RIP_OK && out >= 0) {
    unlink(out_path);
  }
  free(buf);
  close(in);
  return rc;
}
