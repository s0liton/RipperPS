#include "ripper/iso9660.h"

#include <ctype.h>
#include <stdlib.h>
#include <string.h>

#include "ripper/common.h"

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
      return RIP_ENOENT;
    }
    if (sec[0] == 0xFF) {
      break;  // set terminator
    }
    if (sec[0] == 0x01) {
      copy_dstring(fs->system_id, sec + 8, 32);
      copy_dstring(fs->volume_id, sec + 40, 32);
      fs->volume_blocks = rip_le32(sec + 80);
      parse_record(sec + 156, &fs->root);
      return RIP_OK;
    }
  }
  return RIP_ENOENT;
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
