#ifndef RIPPER_UDF_H_
#define RIPPER_UDF_H_

#include <stddef.h>
#include <stdint.h>

#include "ripper/reader.h"

// Read-only UDF (1.02 to 2.60) over a 2048-byte sector reader: DVD-Video's
// bridge file system and Blu-ray's UDF 2.50 with its metadata partition.
// Enough to find files by path and read them; no writing, no VAT (discs only).

#define UDF_MAX_EXTENTS 64

struct udf_extent {
  uint32_t lba;  // absolute sector
  uint32_t length;  // bytes
};

struct udf_entry {
  int is_dir;
  uint64_t size;
  unsigned extent_count;
  struct udf_extent extents[UDF_MAX_EXTENTS];  // in file order
  // Data stored inside the file entry itself (small files)
  uint8_t embedded[256];
  uint32_t embedded_len;
};

struct udf_partition {
  uint16_t number;
  uint32_t start;
  uint32_t length;
};

struct udf_fs {
  struct rip_reader *r;
  char volume_id[64];   // primary volume descriptor's identifier
  char logical_id[128]; // logical volume identifier: usually the disc's name
  uint16_t udf_revision;
  struct udf_partition parts[4];
  unsigned part_count;
  // Partition maps: which partition each reference names, and whether it is
  // a metadata partition (UDF 2.50) mapped through the metadata file.
  struct {
    int metadata;
    unsigned part;  // index into parts
  } maps[4];
  unsigned map_count;
  struct udf_entry metadata_file;
  struct udf_entry root;
};

// RIP_OK when the disc has UDF, RIP_ENOENT when it has none.
int udf_mount(struct udf_fs *fs, struct rip_reader *r);
// Case-insensitive path, "/" separated: "BDMV/index.bdmv".
int udf_lookup(struct udf_fs *fs, const char *path, struct udf_entry *out);
// Reads a whole file of at most max bytes; the caller frees *out.
int udf_read_file(struct udf_fs *fs, const struct udf_entry *e, size_t max, uint8_t **out, size_t *out_len);

typedef int (*udf_dir_fn)(const char *name, const struct udf_entry *e, void *user);  // nonzero stops
// Visits each entry of a directory (not "." or "..").
int udf_list(struct udf_fs *fs, const struct udf_entry *dir, udf_dir_fn fn, void *user);

#endif
