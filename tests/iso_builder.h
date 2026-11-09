#ifndef RIPPER_TEST_ISO_BUILDER_H_
#define RIPPER_TEST_ISO_BUILDER_H_

#include <stddef.h>
#include <stdint.h>

// Builds a small ISO 9660 image in memory. Paths may have one directory level
// ("PS3_GAME/PARAM.SFO"). lba 0 places the file after the previous one.

struct isob_file {
  const char *path;
  const uint8_t *data;
  size_t len;
  uint32_t lba;
};

uint8_t *isob_build(const struct isob_file *files, size_t n, uint32_t total_sectors, const char *volume_id,
                    size_t *out_len);

#endif
