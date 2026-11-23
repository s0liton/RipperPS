#include "ripper/iso9660.h"

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "ripper/common.h"
#include "ripper/util.h"

#define ISO_SECTOR 2048
#define ISO_PVD_LBA 16
#define ISO_MAX_DESCRIPTORS 32
#define ISO_MAX_DIR_BYTES (4u * 1024 * 1024)

static void copy_dstring(char *dst, const uint8_t *src, size_t n) {
  memcpy(dst, src, n);
  dst[n] = '\0';
  while (n > 0 && dst[n - 1] == ' ') {
    dst[--n] = '\0';
  }
}

static void parse_record(const uint8_t *rec, struct iso_entry *e) {
  e->lba = rip_le32(rec + 2);
  e->size = rip_le32(rec + 10);
  e->is_dir = (rec[25] & 0x02) != 0;
}

int iso_mount(struct iso_fs *fs, struct rip_reader *r) {
  uint8_t sec[ISO_SECTOR];
  uint32_t lba;

  memset(fs, 0, sizeof(*fs));
  if (r->sector_size != ISO_SECTOR) {
    return RIP_ENOTSUP;
  }
  fs->r = r;
  for (lba = ISO_PVD_LBA; lba < ISO_PVD_LBA + ISO_MAX_DESCRIPTORS && lba < r->sector_count; lba++) {
    if (rip_reader_read(r, lba, 1, sec) != RIP_OK) {
      return RIP_ERR;
    }
    if (memcmp(sec + 1, "CD001", 5) != 0) {
      break;  // past the descriptors, with no terminator
    }
    if (sec[0] == 0xFF) {
      break;  // set terminator
    }
    if (sec[0] == 0x01 && !fs->volume_blocks) {
      copy_dstring(fs->system_id, sec + 8, 32);
      copy_dstring(fs->volume_id, sec + 40, 32);
      fs->volume_blocks = rip_le32(sec + 80);
      parse_record(sec + 156, &fs->root);
    } else if (sec[0] == 0x02 && sec[88] == 0x25 && sec[89] == 0x2F &&
               (sec[90] == 0x40 || sec[90] == 0x43 || sec[90] == 0x45)) {
      // A supplementary descriptor with Joliet's escape sequence (UCS-2, levels 1-3).
      parse_record(sec + 156, &fs->joliet_root);
      fs->joliet = 1;
    }
  }
  return fs->volume_blocks ? RIP_OK : RIP_ENOENT;
}

// ISO names carry ";1" versions and sometimes a trailing '.' for no extension.
static int name_matches(const uint8_t *name, size_t len, const char *want, size_t want_len) {
  size_t i;

  for (i = 0; i < len; i++) {
    if (name[i] == ';') {
      len = i;
      break;
    }
  }
  if (len > 0 && name[len - 1] == '.') {
    len--;
  }
  if (len != want_len) {
    return 0;
  }
  for (i = 0; i < len; i++) {
    if (toupper(name[i]) != toupper((unsigned char)want[i])) {
      return 0;
    }
  }
  return 1;
}

static int find_in_dir(struct iso_fs *fs, const struct iso_entry *dir, const char *want, size_t want_len,
                       struct iso_entry *out) {
  uint32_t sectors = (dir->size + ISO_SECTOR - 1) / ISO_SECTOR;
  uint8_t sec[ISO_SECTOR];
  uint32_t s;

  if (dir->size > ISO_MAX_DIR_BYTES) {
    return RIP_ERR;
  }
  for (s = 0; s < sectors; s++) {
    size_t off = 0;

    if (rip_reader_read(fs->r, (uint64_t)dir->lba + s, 1, sec) != RIP_OK) {
      return RIP_ERR;
    }
    // Records never cross a sector; a zero length means the rest is padding.
    while (off + 33 <= ISO_SECTOR) {
      uint8_t len = sec[off];
      uint8_t name_len;

      if (len == 0) {
        break;
      }
      if (len < 33 || off + len > ISO_SECTOR) {
        return RIP_ERR;
      }
      name_len = sec[off + 32];
      if (33u + name_len <= len && !(name_len == 1 && sec[off + 33] <= 1) &&
          name_matches(sec + off + 33, name_len, want, want_len)) {
        parse_record(sec + off, out);
        return RIP_OK;
      }
      off += len;
    }
  }
  return RIP_ENOENT;
}

int iso_lookup(struct iso_fs *fs, const char *path, struct iso_entry *out) {
  struct iso_entry cur = fs->root;
  const char *p = path;

  while (*p == '/') {
    p++;
  }
  while (*p) {
    const char *end = strchr(p, '/');
    size_t n = end ? (size_t)(end - p) : strlen(p);
    int rc;

    if (!cur.is_dir) {
      return RIP_ENOENT;
    }
    rc = find_in_dir(fs, &cur, p, n, &cur);
    if (rc != RIP_OK) {
      return rc;
    }
    p += n;
    while (*p == '/') {
      p++;
    }
  }
  *out = cur;
  return RIP_OK;
}

int iso_read_file(struct iso_fs *fs, const struct iso_entry *e, size_t max, uint8_t **out, size_t *out_len) {
  uint32_t sectors;
  uint8_t *buf;

  *out = NULL;
  *out_len = 0;
  if (e->is_dir || e->size > max) {
    return RIP_ERR;
  }
  sectors = (e->size + ISO_SECTOR - 1) / ISO_SECTOR;
  buf = malloc(sectors ? (size_t)sectors * ISO_SECTOR : 1);
  if (!buf) {
    return RIP_ENOMEM;
  }
  if (sectors && rip_reader_read(fs->r, e->lba, sectors, buf) != RIP_OK) {
    free(buf);
    return RIP_ERR;
  }
  *out = buf;
  *out_len = e->size;
  return RIP_OK;
}

/* Extraction */

struct extract {
  struct iso_fs *fs;
  iso_progress_fn progress;
  void *ud;
  uint64_t done;
  uint64_t total;
  uint8_t *buf;
};

#define EXTRACT_CHUNK 512  // sectors a read: 1 MiB

// A record's name as a file name: Joliet's UCS-2 in UTF-8, or the primary's
// less its ";1" and trailing '.'. Empty for anything unsafe to write.
static void record_name(const uint8_t *name, uint8_t len, int joliet, char *out, size_t n) {
  size_t o = 0;
  size_t i;

  if (joliet) {
    for (i = 0; i + 1 < len; i += 2) {
      unsigned c = (unsigned)name[i] << 8 | name[i + 1];
      if (c == ';') {
        break;
      }
      if (c < 0x80) {
        if (o + 1 >= n) {
          break;
        }
        out[o++] = (char)c;
      } else if (c < 0x800) {
        if (o + 2 >= n) {
          break;
        }
        out[o++] = (char)(0xC0 | c >> 6);
        out[o++] = (char)(0x80 | (c & 0x3F));
      } else {
        if (o + 3 >= n) {
          break;
        }
        out[o++] = (char)(0xE0 | c >> 12);
        out[o++] = (char)(0x80 | (c >> 6 & 0x3F));
        out[o++] = (char)(0x80 | (c & 0x3F));
      }
    }
  } else {
    for (i = 0; i < len && name[i] != ';' && o + 1 < n; i++) {
      out[o++] = (char)name[i];
    }
    if (o > 0 && out[o - 1] == '.') {
      o--;
    }
  }
  out[o] = '\0';
  if (!strcmp(out, ".") || !strcmp(out, "..") || strchr(out, '/') || strchr(out, '\\')) {
    out[0] = '\0';
  }
}

static int write_extent(struct extract *x, int fd, uint32_t lba, uint32_t size) {
  uint64_t left = size;

  while (left) {
    uint32_t sectors = (uint32_t)((left + ISO_SECTOR - 1) / ISO_SECTOR);
    size_t bytes;
    if (sectors > EXTRACT_CHUNK) {
      sectors = EXTRACT_CHUNK;
    }
    bytes = (size_t)sectors * ISO_SECTOR < left ? (size_t)sectors * ISO_SECTOR : (size_t)left;
    if (rip_reader_read(x->fs->r, lba, sectors, x->buf) != RIP_OK || rip_write_all(fd, x->buf, bytes) != 0) {
      return RIP_ERR;
    }
    lba += sectors;
    left -= bytes;
    x->done += bytes;
    if (x->progress && x->progress(x->ud, x->done, x->total)) {
      return RIP_ECANCEL;
    }
  }
  return RIP_OK;
}

// Walks dir: with out_dir NULL it only adds up the files' sizes.
static int extract_dir(struct extract *x, const struct iso_entry *dir, const char *out_dir, int depth) {
  uint32_t sectors = (dir->size + ISO_SECTOR - 1) / ISO_SECTOR;
  uint8_t *recs;
  uint32_t s;
  int rc = RIP_OK;
  int fd = -1;  // a multi-extent file still being written
  char open_name[512] = "";

  if (dir->size > ISO_MAX_DIR_BYTES || depth > 64) {
    return RIP_ERR;
  }
  recs = malloc((size_t)sectors * ISO_SECTOR + 1);
  if (!recs) {
    return RIP_ENOMEM;
  }
  if (sectors && rip_reader_read(x->fs->r, dir->lba, sectors, recs) != RIP_OK) {
    free(recs);
    return RIP_ERR;
  }
  if (out_dir && rip_mkdir_p(out_dir) != 0) {
    free(recs);
    return RIP_ERR;
  }
  for (s = 0; s < sectors && rc == RIP_OK; s++) {
    const uint8_t *sec = recs + (size_t)s * ISO_SECTOR;
    size_t off = 0;
    // Records never cross a sector; a zero length means the rest is padding.
    while (off + 33 <= ISO_SECTOR && rc == RIP_OK) {
      uint8_t len = sec[off];
      uint8_t name_len;
      uint8_t flags;
      struct iso_entry e;
      char name[512];
      char path[1024];
      if (len == 0) {
        break;
      }
      if (len < 33 || off + len > ISO_SECTOR) {
        rc = RIP_ERR;
        break;
      }
      name_len = sec[off + 32];
      flags = sec[off + 25];
      if (33u + name_len > len || (name_len == 1 && sec[off + 33] <= 1)) {
        off += len;
        continue;  // "." and ".."
      }
      parse_record(sec + off, &e);
      record_name(sec + off + 33, name_len, x->fs->joliet, name, sizeof(name));
      off += len;
      if (!name[0]) {
        continue;
      }
      if (e.is_dir) {
        if (out_dir) {
          snprintf(path, sizeof(path), "%s/%s", out_dir, name);
        }
        rc = extract_dir(x, &e, out_dir ? path : NULL, depth + 1);
        continue;
      }
      // A file in several extents: each but the last has the multi-extent flag.
      if (!out_dir) {
        x->total += e.size;
        continue;
      }
      if (fd < 0 || strcmp(name, open_name) != 0) {
        if (fd >= 0) {
          close(fd);
        }
        snprintf(path, sizeof(path), "%s/%s", out_dir, name);
        fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (fd < 0) {
          rc = RIP_ERR;
          break;
        }
        snprintf(open_name, sizeof(open_name), "%s", name);
      }
      rc = write_extent(x, fd, e.lba, e.size);
      if (!(flags & 0x80)) {
        if (close(fd) != 0 && rc == RIP_OK) {
          rc = RIP_ERR;
        }
        fd = -1;
        open_name[0] = '\0';
      }
    }
  }
  if (fd >= 0) {
    close(fd);
  }
  free(recs);
  return rc;
}

int iso_extract(struct iso_fs *fs, const char *out_dir, iso_progress_fn progress, void *ud) {
  struct extract x;
  const struct iso_entry *root = fs->joliet ? &fs->joliet_root : &fs->root;
  int rc;

  memset(&x, 0, sizeof(x));
  x.fs = fs;
  x.progress = progress;
  x.ud = ud;
  x.buf = malloc((size_t)EXTRACT_CHUNK * ISO_SECTOR);
  if (!x.buf) {
    return RIP_ENOMEM;
  }
  rc = extract_dir(&x, root, NULL, 0);
  if (rc == RIP_OK) {
    rc = extract_dir(&x, root, out_dir, 0);
  }
  free(x.buf);
  return rc;
}
