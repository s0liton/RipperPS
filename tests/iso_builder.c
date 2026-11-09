#include "iso_builder.h"

#include <stdlib.h>
#include <string.h>

#include "ripper/common.h"

#define SEC 2048
#define MAX_DIRS 8

static void both32(uint8_t *p, uint32_t v) {
  rip_put_le32(p, v);
  rip_put_be32(p + 4, v);
}

static void both16(uint8_t *p, uint16_t v) {
  rip_put_le16(p, v);
  rip_put_be16(p + 2, v);
}

static size_t put_record(uint8_t *p, uint32_t lba, uint32_t size, int dir, const uint8_t *name, uint8_t name_len) {
  uint8_t len = (uint8_t)(33 + name_len + ((name_len & 1) ? 0 : 1));

  memset(p, 0, len);
  p[0] = len;
  both32(p + 2, lba);
  both32(p + 10, size);
  p[25] = dir ? 0x02 : 0x00;
  both16(p + 28, 1);
  p[32] = name_len;
  memcpy(p + 33, name, name_len);
  return len;
}

static size_t put_named(uint8_t *p, uint32_t lba, uint32_t size, int dir, const char *name) {
  uint8_t full[64];
  size_t n = strlen(name);

  memcpy(full, name, n);
  if (!dir) {
    memcpy(full + n, ";1", 2);
    n += 2;
  }
  return put_record(p, lba, size, dir, full, (uint8_t)n);
}

uint8_t *isob_build(const struct isob_file *files, size_t n, uint32_t total_sectors, const char *volume_id,
                    size_t *out_len) {
  const uint8_t self = 0;
  const uint8_t parent = 1;
  char dirs[MAX_DIRS][32];
  uint32_t dir_lba[MAX_DIRS];
  uint32_t *file_lba;
  size_t ndirs = 0;
  uint32_t next;
  uint8_t *img;
  uint8_t *pvd;
  size_t off;
  size_t i;
  size_t d;

  img = calloc(total_sectors, SEC);
  file_lba = calloc(n ? n : 1, sizeof(*file_lba));
  if (!img || !file_lba) {
    free(img);
    free(file_lba);
    return NULL;
  }

  // Collect directories (one level).
  for (i = 0; i < n; i++) {
    const char *slash = strchr(files[i].path, '/');
    if (slash) {
      size_t len = (size_t)(slash - files[i].path);
      for (d = 0; d < ndirs; d++) {
        if (strlen(dirs[d]) == len && memcmp(dirs[d], files[i].path, len) == 0) {
          break;
        }
      }
      if (d == ndirs && ndirs < MAX_DIRS) {
        memcpy(dirs[ndirs], files[i].path, len);
        dirs[ndirs][len] = '\0';
        ndirs++;
      }
    }
  }

  // Layout: PVD 16, terminator 17, root 18, directories 19.., then files.
  for (d = 0; d < ndirs; d++) {
    dir_lba[d] = 19 + (uint32_t)d;
  }
  next = 19 + (uint32_t)ndirs;
  for (i = 0; i < n; i++) {
    file_lba[i] = files[i].lba ? files[i].lba : next;
    next = file_lba[i] + (uint32_t)((files[i].len + SEC - 1) / SEC);
    if (next <= file_lba[i]) {
      next = file_lba[i] + 1;
    }
    memcpy(img + (size_t)file_lba[i] * SEC, files[i].data, files[i].len);
  }

  pvd = img + 16 * SEC;
  pvd[0] = 1;
  memcpy(pvd + 1, "CD001", 5);
  pvd[6] = 1;
  memset(pvd + 8, ' ', 64);
  memcpy(pvd + 40, volume_id, strlen(volume_id));
  both32(pvd + 80, total_sectors);
  both16(pvd + 120, 1);
  both16(pvd + 124, 1);
  both16(pvd + 128, SEC);
  put_record(pvd + 156, 18, SEC, 1, &self, 1);
  img[17 * SEC] = 0xFF;
  memcpy(img + 17 * SEC + 1, "CD001", 5);
  img[17 * SEC + 6] = 1;

  // Root directory.
  off = 18 * SEC;
  off += put_record(img + off, 18, SEC, 1, &self, 1);
  off += put_record(img + off, 18, SEC, 1, &parent, 1);
  for (d = 0; d < ndirs; d++) {
    off += put_named(img + off, dir_lba[d], SEC, 1, dirs[d]);
  }
  for (i = 0; i < n; i++) {
    if (!strchr(files[i].path, '/')) {
      off += put_named(img + off, file_lba[i], (uint32_t)files[i].len, 0, files[i].path);
    }
  }

  // Subdirectories.
  for (d = 0; d < ndirs; d++) {
    size_t dlen = strlen(dirs[d]);
    off = (size_t)dir_lba[d] * SEC;
    off += put_record(img + off, dir_lba[d], SEC, 1, &self, 1);
    off += put_record(img + off, 18, SEC, 1, &parent, 1);
    for (i = 0; i < n; i++) {
      if (strncmp(files[i].path, dirs[d], dlen) == 0 && files[i].path[dlen] == '/') {
        off += put_named(img + off, file_lba[i], (uint32_t)files[i].len, 0, files[i].path + dlen + 1);
      }
    }
  }

  free(file_lba);
  *out_len = (size_t)total_sectors * SEC;
  return img;
}
