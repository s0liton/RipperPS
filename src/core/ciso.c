#include "ripper/ciso.h"

#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "ripper/common.h"
#include "ripper/log.h"
#include "ripper/util.h"

#define HEADER 0x8000u
#define GC_MAGIC 0xC2339F3Du
#define APPLOADER 0x2440u
// File tables larger than this aren't a GameCube game's.
#define MAX_FST (8u << 20)

static void mark(uint8_t *map, uint64_t size, uint64_t from, uint64_t len) {
  uint64_t b;

  if (!len || from >= size) {
    return;
  }
  if (from + len > size) {
    len = size - from;
  }
  for (b = from / CISO_BLOCK; b <= (from + len - 1) / CISO_BLOCK && b < CISO_MAP; b++) {
    map[b] = 1;
  }
}

int ciso_used_blocks(int fd, uint64_t size, uint8_t map[CISO_MAP]) {
  uint8_t head[0x440];
  uint8_t app[0x20];
  uint8_t dol[0x100];
  uint32_t dol_at;
  uint32_t fst_at;
  uint32_t fst_len;
  uint8_t *fst;
  uint32_t count;
  uint32_t i;

  memset(map, 0, CISO_MAP);
  if (size > (uint64_t)CISO_BLOCK * CISO_MAP || rip_seek_read(fd, 0, head, sizeof(head)) != 0 ||
      rip_be32(head + 0x1C) != GC_MAGIC) {
    return RIP_ERR;
  }
  dol_at = rip_be32(head + 0x420);
  fst_at = rip_be32(head + 0x424);
  fst_len = rip_be32(head + 0x428);
  // The header, the debug area and the apploader (its code and trailer).
  if (rip_seek_read(fd, APPLOADER, app, sizeof(app)) != 0) {
    return RIP_ERR;
  }
  mark(map, size, 0, APPLOADER + 0x20 + (uint64_t)rip_be32(app + 0x14) + rip_be32(app + 0x18));
  // The main executable: its text and data sections.
  if (dol_at && rip_seek_read(fd, dol_at, dol, sizeof(dol)) == 0) {
    uint64_t end = 0x100;
    for (i = 0; i < 18; i++) {
      uint64_t e = (uint64_t)rip_be32(dol + i * 4) + rip_be32(dol + 0x90 + i * 4);
      if (rip_be32(dol + 0x90 + i * 4) && e > end) {
        end = e;
      }
    }
    mark(map, size, dol_at, end);
  }
  if (!fst_len || fst_len > MAX_FST) {
    return RIP_ERR;
  }
  mark(map, size, fst_at, fst_len);
  fst = malloc(fst_len);
  if (!fst) {
    return RIP_ENOMEM;
  }
  if (rip_seek_read(fd, fst_at, fst, fst_len) != 0) {
    free(fst);
    return RIP_ERR;
  }
  // Twelve bytes each: a directory flag and name offset, then a file's
  // offset and length. The root's length is how many there are.
  count = rip_be32(fst + 8);
  if (count == 0 || (uint64_t)count * 12 > fst_len) {
    free(fst);
    return RIP_ERR;
  }
  for (i = 1; i < count; i++) {
    const uint8_t *e = fst + (size_t)i * 12;
    if (e[0] == 0) {
      mark(map, size, rip_be32(e + 4), rip_be32(e + 8));
    }
  }
  free(fst);
  return RIP_OK;
}

int ciso_from_iso(const char *iso_path, const char *out_path, iso_progress_fn progress, void *ud) {
  uint8_t *header = NULL;
  uint8_t *buf = NULL;
  struct stat st;
  uint64_t total = 0;
  uint64_t done = 0;
  uint32_t blocks;
  uint32_t b;
  int in = open(iso_path, O_RDONLY);
  int out = -1;
  int rc = RIP_OK;

  if (in < 0 || fstat(in, &st) != 0) {
    RIP_ERROR("ciso: can't open %s: %s", iso_path, strerror(errno));
    if (in >= 0) {
      close(in);
    }
    return RIP_ERR;
  }
  header = calloc(1, HEADER);
  buf = malloc(CISO_BLOCK);
  if (!header || !buf) {
    rc = RIP_ENOMEM;
    goto done;
  }
  memcpy(header, "CISO", 4);
  header[4] = (uint8_t)CISO_BLOCK;
  header[5] = (uint8_t)(CISO_BLOCK >> 8);
  header[6] = (uint8_t)(CISO_BLOCK >> 16);
  header[7] = (uint8_t)(CISO_BLOCK >> 24);
  rc = ciso_used_blocks(in, (uint64_t)st.st_size, header + 8);
  if (rc != RIP_OK) {
    RIP_ERROR("ciso: %s isn't a GameCube disc this can read", iso_path);
    goto done;
  }
  blocks = (uint32_t)(((uint64_t)st.st_size + CISO_BLOCK - 1) / CISO_BLOCK);
  for (b = 0; b < blocks; b++) {
    total += header[8 + b] ? CISO_BLOCK : 0;
  }
  RIP_INFO("ciso: %llu of %u blocks used", (unsigned long long)(total / CISO_BLOCK), blocks);
  out = open(out_path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
  if (out < 0 || rip_write_all(out, header, HEADER) != 0) {
    RIP_ERROR("ciso: can't write %s: %s", out_path, strerror(errno));
    rc = RIP_ERR;
    goto done;
  }
  for (b = 0; b < blocks && rc == RIP_OK; b++) {
    ssize_t got;
    if (!header[8 + b]) {
      continue;
    }
    // The last block is whole in the file, zeros past the image's end.
    got = pread(in, buf, CISO_BLOCK, (off_t)b * CISO_BLOCK);
    if (got < 0) {
      RIP_ERROR("ciso: read from %s failed: %s", iso_path, strerror(errno));
      rc = RIP_ERR;
      break;
    }
    memset(buf + got, 0, CISO_BLOCK - (size_t)got);
    if (rip_write_all(out, buf, CISO_BLOCK) != 0) {
      RIP_ERROR("ciso: write to %s failed: %s", out_path, strerror(errno));
      rc = RIP_ERR;
      break;
    }
    done += CISO_BLOCK;
    if (progress && progress(ud, done, total)) {
      rc = RIP_ECANCEL;
    }
  }
  if (rc == RIP_OK && fsync(out) != 0) {
    RIP_ERROR("ciso: can't finish %s: %s", out_path, strerror(errno));
    rc = RIP_ERR;
  }

done:
  if (out >= 0 && close(out) != 0 && rc == RIP_OK) {
    rc = RIP_ERR;
  }
  if (rc != RIP_OK && out >= 0) {
    unlink(out_path);
  }
  free(header);
  free(buf);
  close(in);
  return rc;
}
