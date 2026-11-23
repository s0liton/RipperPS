#include "ripper/wbfs.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>

#include "ripper/common.h"
#include "ripper/log.h"
#include "ripper/util.h"

// The geometry wit and USB Loader GX pick for a one-game file: 512-byte hd
// sectors and the smallest WBFS sector (64 Wii sectors) that numbers a
// dual-layer disc's worth of blocks in 16 bits.
#define HD_SHIFT 9
#define HD_SECTOR (1u << HD_SHIFT)
#define BLOCK_SHIFT 21
#define BLOCK (1u << BLOCK_SHIFT)
#define WII_SECTOR 0x8000u
// Every WBFS tool sizes the block table for this many Wii sectors.
#define WII_MAX_SECTORS (143432u * 2)
#define DISC_BLOCKS (WII_MAX_SECTORS / (BLOCK / WII_SECTOR))
// The disc info: the first 0x100 bytes of the disc, then a big-endian block
// number for each 2 MiB of the disc (0 for left out), from the second hd
// sector. The rest of the first block stays zero.
#define INFO_HEADER 0x100u
#define WBFS_VERSION 1  // what wit writes; USB Loader GX leaves it 0 and ignores it

#define WII_MAGIC 0x5D1C9EA3u
#define PART_TABLES 0x40000u
#define REGION_END 0x4E020u
#define MAX_PARTS_PER_TABLE 64  // real discs have a handful
#define MAX_FILES 10            // USB Loader GX opens .wbfs and .wbf1 to .wbf9
#define SPLIT_ALIGN 0x8000ull

struct out {
  const char *base;
  uint64_t split;  // 0 for one file
  uint64_t in_file;
  unsigned file;     // the one open now
  unsigned created;  // files made so far, removed again on failure
  int fd;
};

// "X.wbfs" for the first file, "X.wbf1"... after it.
static void file_name(const char *base, unsigned idx, char *out, size_t cap) {
  size_t len = strlen(base);
  if (idx == 0) {
    snprintf(out, cap, "%s", base);
  } else {
    snprintf(out, cap, "%.*s%u", (int)(len - 1), base, idx);
  }
}

static int open_file(struct out *o) {
  char name[1100];
  file_name(o->base, o->file, name, sizeof(name));
  o->fd = open(name, O_WRONLY | O_CREAT | O_TRUNC, 0644);
  if (o->fd < 0) {
    RIP_ERROR("wbfs: can't create %s: %s", name, strerror(errno));
    return RIP_ERR;
  }
  o->created = o->file + 1;
  o->in_file = 0;
  return RIP_OK;
}

static int close_file(struct out *o) {
  char name[1100];
  int fd = o->fd;
  o->fd = -1;
  if (close(fd) != 0) {
    file_name(o->base, o->file, name, sizeof(name));
    RIP_ERROR("wbfs: can't finish %s: %s", name, strerror(errno));
    return RIP_ERR;
  }
  return RIP_OK;
}

static int out_write(struct out *o, const uint8_t *buf, size_t len) {
  while (len) {
    size_t n = len;
    if (o->split) {
      if (o->in_file == o->split) {
        if (close_file(o) != RIP_OK) {
          return RIP_ERR;
        }
        o->file++;
        if (open_file(o) != RIP_OK) {
          return RIP_ERR;
        }
      }
      if (n > o->split - o->in_file) {
        n = (size_t)(o->split - o->in_file);
      }
    }
    if (rip_write_all(o->fd, buf, n) != 0) {
      char name[1100];
      file_name(o->base, o->file, name, sizeof(name));
      RIP_ERROR("wbfs: can't write %s: %s", name, strerror(errno));
      return RIP_ERR;
    }
    o->in_file += n;
    buf += n;
    len -= n;
  }
  return RIP_OK;
}

static int ends_with_wbfs(const char *path) {
  size_t len = strlen(path);
  return len > 5 && strcasecmp(path + len - 5, ".wbfs") == 0;
}

static void mark(uint8_t *used, uint64_t start, uint64_t end) {
  uint64_t b;
  for (b = start >> BLOCK_SHIFT; b <= (end - 1) >> BLOCK_SHIFT; b++) {
    used[b] = 1;
  }
}

// Marks the blocks every partition covers, from its start through the end of
// its data. Offsets in the tables and partition headers are stored >> 2.
static int mark_partitions(int fd, const char *iso_path, uint64_t iso_size, uint8_t *used) {
  uint8_t tables[32];
  unsigned t;
  unsigned found = 0;

  if (rip_seek_read(fd, PART_TABLES, tables, sizeof(tables)) != 0) {
    RIP_ERROR("wbfs: can't read the partition tables of %s: %s", iso_path, strerror(errno));
    return RIP_ERR;
  }
  for (t = 0; t < 4; t++) {
    uint32_t count = rip_be32(tables + t * 8);
    uint64_t at = (uint64_t)rip_be32(tables + t * 8 + 4) << 2;
    uint32_t i;
    if (count > MAX_PARTS_PER_TABLE) {
      RIP_ERROR("wbfs: %s: partition table %u lists %u partitions", iso_path, t, (unsigned)count);
      return RIP_ERR;
    }
    for (i = 0; i < count; i++) {
      uint8_t ent[8];
      uint8_t ph[8];
      uint64_t off, data_off, data_size, end;
      if (rip_seek_read(fd, at + i * 8u, ent, sizeof(ent)) != 0) {
        RIP_ERROR("wbfs: %s: can't read partition table %u", iso_path, t);
        return RIP_ERR;
      }
      off = (uint64_t)rip_be32(ent) << 2;
      if (off < REGION_END || rip_seek_read(fd, off + 0x2B8, ph, sizeof(ph)) != 0) {
        RIP_ERROR("wbfs: %s: no partition header at 0x%llx", iso_path, (unsigned long long)off);
        return RIP_ERR;
      }
      data_off = (uint64_t)rip_be32(ph) << 2;
      data_size = (uint64_t)rip_be32(ph + 4) << 2;
      end = off + data_off + data_size;
      if (!data_off || end > (uint64_t)DISC_BLOCKS * BLOCK) {
        RIP_ERROR("wbfs: %s: partition at 0x%llx has a bad data area (0x%llx + 0x%llx)", iso_path,
                  (unsigned long long)off, (unsigned long long)data_off, (unsigned long long)data_size);
        return RIP_ERR;
      }
      if (end > iso_size) {
        RIP_WARN("wbfs: %s: partition at 0x%llx runs 0x%llx bytes past the end of the image; written as zeros",
                 iso_path, (unsigned long long)off, (unsigned long long)(end - iso_size));
      }
      RIP_INFO("wbfs: partition type %u at 0x%llx, data 0x%llx bytes", (unsigned)rip_be32(ent + 4),
               (unsigned long long)off, (unsigned long long)data_size);
      mark(used, off, end);
      found++;
    }
  }
  if (!found) {
    RIP_ERROR("wbfs: %s has no partitions", iso_path);
    return RIP_ERR;
  }
  return RIP_OK;
}

int wbfs_from_iso(const char *iso_path, const char *out_path, uint64_t split_size, iso_progress_fn progress, void *ud) {
  uint8_t used[DISC_BLOCKS];
  uint8_t disc_header[INFO_HEADER];
  struct out o;
  struct stat st;
  uint8_t *buf = NULL;
  uint64_t total, done, files;
  unsigned n_used = 0;
  unsigned i, next;
  int fd;
  int rc;

  memset(&o, 0, sizeof(o));
  o.base = out_path;
  o.fd = -1;

  fd = open(iso_path, O_RDONLY);
  if (fd < 0) {
    RIP_ERROR("wbfs: can't open %s: %s", iso_path, strerror(errno));
    return RIP_ERR;
  }
  if (fstat(fd, &st) != 0) {
    RIP_ERROR("wbfs: can't stat %s: %s", iso_path, strerror(errno));
    close(fd);
    return RIP_ERR;
  }
  if ((uint64_t)st.st_size < REGION_END || rip_seek_read(fd, 0, disc_header, sizeof(disc_header)) != 0 ||
      rip_be32(disc_header + 0x18) != WII_MAGIC) {
    RIP_ERROR("wbfs: %s isn't a Wii disc image", iso_path);
    close(fd);
    return RIP_ENOTSUP;
  }

  // The header, partition tables and region info all sit in the first block.
  memset(used, 0, sizeof(used));
  used[0] = 1;
  if (mark_partitions(fd, iso_path, (uint64_t)st.st_size, used) != RIP_OK) {
    close(fd);
    return RIP_ERR;
  }
  for (i = 0; i < DISC_BLOCKS; i++) {
    n_used += used[i];
  }
  // Block 0 of the file holds the WBFS head and disc info; data starts at 1.
  total = ((uint64_t)n_used + 1) * BLOCK;

  if (split_size) {
    split_size &= ~(SPLIT_ALIGN - 1);
    if (!split_size || !ends_with_wbfs(out_path)) {
      RIP_ERROR("wbfs: can't split %s (parts need a .wbfs name and at least 32 KiB)", out_path);
      close(fd);
      return RIP_ERR;
    }
    files = (total + split_size - 1) / split_size;
    if (files > MAX_FILES) {
      RIP_ERROR("wbfs: %llu-byte parts would need %llu files, USB Loader GX reads %u", (unsigned long long)split_size,
                (unsigned long long)files, MAX_FILES);
      close(fd);
      return RIP_ERR;
    }
    o.split = split_size;
  }

  buf = malloc(BLOCK);
  if (!buf) {
    close(fd);
    return RIP_ENOMEM;
  }

  // Parts left from an earlier, longer write would be read as part of this one.
  if (ends_with_wbfs(out_path)) {
    for (i = 1; i < MAX_FILES; i++) {
      char name[1100];
      file_name(out_path, i, name, sizeof(name));
      unlink(name);
    }
  }

  // The first block: the head, one disc in slot 0, its info, and zeros. Zeros
  // in the free block table mark every block used, as after a trim.
  memset(buf, 0, BLOCK);
  memcpy(buf, "WBFS", 4);
  rip_put_be32(buf + 4, (uint32_t)((n_used + 1) << (BLOCK_SHIFT - HD_SHIFT)));
  buf[8] = HD_SHIFT;
  buf[9] = BLOCK_SHIFT;
  buf[10] = WBFS_VERSION;
  buf[12] = 1;
  memcpy(buf + HD_SECTOR, disc_header, INFO_HEADER);
  for (i = 0, next = 1; i < DISC_BLOCKS; i++) {
    if (used[i]) {
      rip_put_be16(buf + HD_SECTOR + INFO_HEADER + i * 2, (uint16_t)next++);
    }
  }

  rc = open_file(&o);
  if (rc == RIP_OK) {
    rc = out_write(&o, buf, BLOCK);
  }
  done = BLOCK;
  if (rc == RIP_OK && progress && progress(ud, done, total)) {
    rc = RIP_ECANCEL;
  }
  for (i = 0; i < DISC_BLOCKS && rc == RIP_OK; i++) {
    long got;
    if (!used[i]) {
      continue;
    }
    if (lseek(fd, (off_t)i * BLOCK, SEEK_SET) < 0) {
      got = -1;
    } else {
      got = rip_read_full(fd, buf, BLOCK);
    }
    if (got < 0) {
      RIP_ERROR("wbfs: can't read %s at 0x%llx: %s", iso_path, (unsigned long long)i * BLOCK, strerror(errno));
      rc = RIP_ERR;
      break;
    }
    // The last block can run past the end of the image.
    memset(buf + got, 0, BLOCK - (size_t)got);
    rc = out_write(&o, buf, BLOCK);
    done += BLOCK;
    if (rc == RIP_OK && progress && progress(ud, done, total)) {
      rc = RIP_ECANCEL;
    }
  }
  if (o.fd >= 0 && close_file(&o) != RIP_OK && rc == RIP_OK) {
    rc = RIP_ERR;
  }
  free(buf);
  close(fd);

  if (rc != RIP_OK) {
    unsigned k;
    for (k = 0; k < o.created; k++) {
      char name[1100];
      file_name(out_path, k, name, sizeof(name));
      unlink(name);
    }
    return rc;
  }
  RIP_INFO("wbfs: wrote %s, %u of %u blocks (%llu MiB) in %u file(s)", out_path, n_used,
           (unsigned)((st.st_size + BLOCK - 1) / BLOCK), (unsigned long long)(total >> 20), o.file + 1);
  return RIP_OK;
}
