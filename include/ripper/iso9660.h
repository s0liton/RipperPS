#ifndef RIPPER_ISO9660_H_
#define RIPPER_ISO9660_H_

#include <stddef.h>
#include <stdint.h>

#include "ripper/reader.h"

// Read-only ISO 9660 over a 2048-byte sector reader. It reads the disc directly,
// so identification works on any drive and on image files, with no OS mount.

struct iso_entry {
  uint32_t lba;
  uint32_t size;
  int is_dir;
};

struct iso_fs {
  struct rip_reader *r;
  struct iso_entry root;
  uint32_t volume_blocks;
  char system_id[33];
  char volume_id[33];
};

// RIP_OK when a primary volume descriptor exists, RIP_ENOENT when the disc has none.
int iso_mount(struct iso_fs *fs, struct rip_reader *r);
// Case-insensitive path such as "PS3_GAME/PARAM.SFO". Version suffixes (";1") are ignored.
int iso_lookup(struct iso_fs *fs, const char *path, struct iso_entry *out);
// Reads a whole file of at most max bytes. The caller frees *out.
int iso_read_file(struct iso_fs *fs, const struct iso_entry *e, size_t max, uint8_t **out, size_t *out_len);

#endif
