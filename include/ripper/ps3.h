#ifndef RIPPER_PS3_H_
#define RIPPER_PS3_H_

#include <stdint.h>

#define PS3_SECTOR 2048
#define PS3_MAX_REGIONS 511  // sector 0 holds up to 255 plain regions

// Sector 0 of a PS3 disc lists its unencrypted ranges:
//   0x00  u32 BE  number of plain regions (n)
//   0x08  n pairs of u32 BE (first, last) sector numbers, inclusive
// Every sector between two plain regions is encrypted with AES-128-CBC, the
// disc key, and an IV of the sector number as a 16-byte big-endian integer.

struct ps3_region {
  uint32_t first;
  uint32_t last;  // inclusive
  int encrypted;
};

struct ps3_layout {
  struct ps3_region regions[PS3_MAX_REGIONS];
  unsigned count;
  uint32_t total_sectors;
};

int ps3_parse_layout(const uint8_t sector0[PS3_SECTOR], struct ps3_layout *out);
int ps3_sector_encrypted(const struct ps3_layout *layout, uint32_t lba);

// Redump lists the processed disc key; IRD files and drives give "data1",
// which turns into the disc key through one fixed AES-CBC encryption.
void ps3_key_from_d1(const uint8_t d1[16], uint8_t key[16]);

// In place. Sectors outside encrypted regions are left untouched.
void ps3_crypt_sectors(const struct ps3_layout *layout, const uint8_t key[16], int decrypt, uint32_t first_lba,
                       uint8_t *buf, uint32_t count);

typedef void (*ps3_progress_fn)(uint64_t done, uint64_t total, void *user);

// Decrypts an image file in place. Only encrypted regions are rewritten, so a
// failure part-way leaves a mixed image: keep the hashes of the original.
int ps3_decrypt_file(const char *path, const uint8_t key[16], ps3_progress_fn progress, void *user);

#endif
