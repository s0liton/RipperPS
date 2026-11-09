#include "ripper/zip.h"

#include <dirent.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#include <zlib.h>

#include "ripper/common.h"
#include "ripper/log.h"
#include "ripper/util.h"

// After PKWARE's APPNOTE: every entry stored, with a ZIP64 extra field in
// both its local and central headers, so sizes and offsets past 4 GiB need
// nothing special.

#define LOCAL_SIG 0x04034b50u
#define CENTRAL_SIG 0x02014b50u
#define EOCD64_SIG 0x06064b50u
#define LOCATOR_SIG 0x07064b50u
#define EOCD_SIG 0x06054b50u
#define VERSION 45  // 4.5: ZIP64
#define FLAG_UTF8 0x0800

struct entry {
  char name[1024];  // in the zip, '/'-separated
  char path[1100];  // on disk
  uint64_t size;
  uint64_t offset;  // of its local header
  uint32_t crc;
  uint16_t time;
  uint16_t date;
};

struct list {
  struct entry *e;
  int count;
  int cap;
  uint64_t total;
};

static void put16(uint8_t **p, uint16_t v) {
  rip_put_le16(*p, v);
  *p += 2;
}

static void put32(uint8_t **p, uint32_t v) {
  rip_put_le32(*p, v);
  *p += 4;
}

static void put64(uint8_t **p, uint64_t v) {
  put32(p, (uint32_t)v);
  put32(p, (uint32_t)(v >> 32));
}

static void dos_time(time_t t, uint16_t *tm_out, uint16_t *date_out) {
  struct tm tm;

  localtime_r(&t, &tm);
  if (tm.tm_year < 80) {
    *tm_out = 0;
    *date_out = (1 << 5) | 1;
    return;
  }
  *tm_out = (uint16_t)((tm.tm_hour << 11) | (tm.tm_min << 5) | (tm.tm_sec / 2));
  *date_out = (uint16_t)(((tm.tm_year - 80) << 9) | ((tm.tm_mon + 1) << 5) | tm.tm_mday);
}

static int collect(struct list *l, const char *dir, const char *prefix) {
  DIR *d = opendir(dir);
  struct dirent *e;
  int rc = RIP_OK;

  if (!d) {
    RIP_ERROR("zip: can't read %s", dir);
    return RIP_ERR;
  }
  while (rc == RIP_OK && (e = readdir(d)) != NULL) {
    char path[1100];
    char name[1024];
    struct stat st;
    if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) {
      continue;
    }
    snprintf(path, sizeof(path), "%s/%s", dir, e->d_name);
    snprintf(name, sizeof(name), "%s%s%s", prefix, prefix[0] ? "/" : "", e->d_name);
    if (stat(path, &st) != 0) {
      continue;
    }
    if (S_ISDIR(st.st_mode)) {
      rc = collect(l, path, name);
      continue;
    }
    if (!S_ISREG(st.st_mode)) {
      continue;
    }
    if (l->count == l->cap) {
      int cap = l->cap ? l->cap * 2 : 64;
      struct entry *n = realloc(l->e, (size_t)cap * sizeof(*n));
      if (!n) {
        rc = RIP_ENOMEM;
        break;
      }
      l->e = n;
      l->cap = cap;
    }
    memset(&l->e[l->count], 0, sizeof(l->e[0]));
    snprintf(l->e[l->count].name, sizeof(l->e[0].name), "%s", name);
    snprintf(l->e[l->count].path, sizeof(l->e[0].path), "%s", path);
    l->e[l->count].size = (uint64_t)st.st_size;
    dos_time(st.st_mtime, &l->e[l->count].time, &l->e[l->count].date);
    l->total += (uint64_t)st.st_size;
    l->count++;
  }
  closedir(d);
  return rc;
}

// The local header: CRC and sizes are filled in once the data is written.
static size_t local_header(const struct entry *e, uint8_t *buf) {
  uint8_t *p = buf;
  size_t nl = strlen(e->name);

  put32(&p, LOCAL_SIG);
  put16(&p, VERSION);
  put16(&p, FLAG_UTF8);
  put16(&p, 0);  // stored
  put16(&p, e->time);
  put16(&p, e->date);
  put32(&p, e->crc);
  put32(&p, 0xFFFFFFFFu);
  put32(&p, 0xFFFFFFFFu);
  put16(&p, (uint16_t)nl);
  put16(&p, 20);
  memcpy(p, e->name, nl);
  p += nl;
  put16(&p, 0x0001);  // ZIP64: both sizes
  put16(&p, 16);
  put64(&p, e->size);
  put64(&p, e->size);
  return (size_t)(p - buf);
}

static size_t central_header(const struct entry *e, uint8_t *buf) {
  uint8_t *p = buf;
  size_t nl = strlen(e->name);

  put32(&p, CENTRAL_SIG);
  put16(&p, (3 << 8) | VERSION);  // made on Unix
  put16(&p, VERSION);
  put16(&p, FLAG_UTF8);
  put16(&p, 0);
  put16(&p, e->time);
  put16(&p, e->date);
  put32(&p, e->crc);
  put32(&p, 0xFFFFFFFFu);
  put32(&p, 0xFFFFFFFFu);
  put16(&p, (uint16_t)nl);
  put16(&p, 28);
  put16(&p, 0);  // comment
  put16(&p, 0);  // disk
  put16(&p, 0);  // internal attributes
  put32(&p, (uint32_t)(0100644u << 16));
  put32(&p, 0xFFFFFFFFu);
  memcpy(p, e->name, nl);
  p += nl;
  put16(&p, 0x0001);  // ZIP64: sizes and the header's offset
  put16(&p, 24);
  put64(&p, e->size);
  put64(&p, e->size);
  put64(&p, e->offset);
  return (size_t)(p - buf);
}

int zip_build_dir(const char *src_dir, const char *zip_path, zip_progress_fn progress, void *ud) {
  struct list l = {NULL, 0, 0, 0};
  uint8_t hdr[1200];
  uint8_t *buf = NULL;
  uint64_t at = 0;
  uint64_t done = 0;
  uint64_t cd_start;
  uint64_t cd_size = 0;
  int out = -1;
  int rc;
  int i;

  rc = collect(&l, src_dir, "");
  if (rc == RIP_OK && !l.count) {
    RIP_ERROR("zip: %s is empty", src_dir);
    rc = RIP_ERR;
  }
  buf = malloc(1 << 20);
  out = rc == RIP_OK && buf ? open(zip_path, O_RDWR | O_CREAT | O_TRUNC, 0644) : -1;
  if (rc == RIP_OK && out < 0) {
    RIP_ERROR("zip: can't create %s", zip_path);
    rc = RIP_ERR;
  }
  for (i = 0; rc == RIP_OK && i < l.count; i++) {
    struct entry *e = &l.e[i];
    size_t hl = local_header(e, hdr);
    uint64_t left = e->size;
    uLong crc = crc32(0L, Z_NULL, 0);
    int in = open(e->path, O_RDONLY);
    e->offset = at;
    if (in < 0 || rip_write_all(out, hdr, hl) != 0) {
      RIP_ERROR("zip: can't add %s", e->path);
      rc = RIP_ERR;
    }
    while (rc == RIP_OK && left) {
      long got = rip_read_full(in, buf, left < (1 << 20) ? (size_t)left : (1 << 20));
      if (got <= 0 || rip_write_all(out, buf, (size_t)got) != 0) {
        RIP_ERROR("zip: copying %s failed", e->path);
        rc = RIP_ERR;
        break;
      }
      crc = crc32(crc, buf, (uInt)got);
      left -= (uint64_t)got;
      done += (uint64_t)got;
      if (progress && progress(ud, done, l.total)) {
        rc = RIP_ECANCEL;
      }
    }
    if (in >= 0) {
      close(in);
    }
    if (rc == RIP_OK) {
      // The CRC goes back into the header now that it's known.
      e->crc = (uint32_t)crc;
      local_header(e, hdr);
      if (rip_seek_write(out, e->offset + 14, hdr + 14, 4) != 0 || lseek(out, 0, SEEK_END) < 0) {
        rc = RIP_ERR;
      }
      at += hl + e->size;
    }
  }
  cd_start = at;
  for (i = 0; rc == RIP_OK && i < l.count; i++) {
    size_t hl = central_header(&l.e[i], hdr);
    if (rip_write_all(out, hdr, hl) != 0) {
      rc = RIP_ERR;
    }
    cd_size += hl;
  }
  if (rc == RIP_OK) {
    uint8_t *p = hdr;
    put32(&p, EOCD64_SIG);  // ZIP64 end of central directory
    put64(&p, 44);
    put16(&p, (3 << 8) | VERSION);
    put16(&p, VERSION);
    put32(&p, 0);
    put32(&p, 0);
    put64(&p, (uint64_t)l.count);
    put64(&p, (uint64_t)l.count);
    put64(&p, cd_size);
    put64(&p, cd_start);
    put32(&p, LOCATOR_SIG);
    put32(&p, 0);
    put64(&p, cd_start + cd_size);
    put32(&p, 1);
    put32(&p, EOCD_SIG);  // and the classic one, pointing at it
    put16(&p, 0);
    put16(&p, 0);
    put16(&p, l.count > 0xFFFE ? 0xFFFF : (uint16_t)l.count);
    put16(&p, l.count > 0xFFFE ? 0xFFFF : (uint16_t)l.count);
    put32(&p, 0xFFFFFFFFu);
    put32(&p, 0xFFFFFFFFu);
    put16(&p, 0);
    if (rip_write_all(out, hdr, (size_t)(p - hdr)) != 0 || fsync(out) != 0) {
      rc = RIP_ERR;
    }
  }
  if (out >= 0) {
    close(out);
  }
  if (rc != RIP_OK) {
    unlink(zip_path);
  } else {
    RIP_INFO("zip: %s, %d file(s), %llu MiB", zip_path, l.count, (unsigned long long)(l.total >> 20));
  }
  free(buf);
  free(l.e);
  return rc;
}
