#ifndef RIPPER_TEST_UDF_BUILDER_H_
#define RIPPER_TEST_UDF_BUILDER_H_

#include <stddef.h>
#include <stdint.h>

// Builds a small UDF image in memory: revision 1.02 (DVD-Video's bridge file
// system, one partition) or 2.50 (Blu-ray: a metadata partition holding the
// directories and file entries, Extended File Entries). Paths may be nested
// ("BDMV/STREAM/00000.m2ts"); directories are made as needed.

struct udfb_file {
  const char *path;
  const uint8_t *data;
  size_t len;
};

uint8_t *udfb_build(const struct udfb_file *files, size_t n, int rev250, const char *volume_id, size_t *out_len);

#endif
