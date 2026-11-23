// A Wii image written as WBFS (src/core/wbfs.c): the head, the block table,
// the kept blocks in place, a split into FAT32-style parts, and a cancel.
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "ripper/common.h"
#include "ripper/wbfs.h"
#include "test.h"

#define MIB (1024u * 1024)
#define BLK (2 * MIB)
// Blocks 0 (header, tables), 2 and 3 (the partition) are used, 1 isn't; the
// image ends halfway through block 3.
#define IMG_SIZE (7 * MIB + MIB / 2)
#define PART_OFF (4 * MIB)
#define DATA_OFF 0x20000u
#define OUT_SIZE (4 * BLK)

static int cancel_at_two(void *ud, uint64_t done, uint64_t total) {
  (void)ud;
  (void)total;
  return done >= 2 * BLK;
}

static int count_calls(void *ud, uint64_t done, uint64_t total) {
  int *calls = ud;
  (*calls)++;
  return done > total;
}

static uint8_t *make_image(void) {
  uint8_t *img = malloc(IMG_SIZE);
  uint32_t i;

  for (i = 0; i < IMG_SIZE; i++) {
    img[i] = (uint8_t)(i * 7 + (i >> 11));
  }
  memcpy(img, "RTSE01", 6);
  rip_put_be32(img + 0x18, 0x5D1C9EA3);
  memset(img + 0x20, 0, 0x40);
  memcpy(img + 0x20, "WBFS TEST", 9);
  // One partition in the first table, none in the other three.
  memset(img + 0x40000, 0, 0x28);
  rip_put_be32(img + 0x40000, 1);
  rip_put_be32(img + 0x40004, 0x40020 >> 2);
  rip_put_be32(img + 0x40020, PART_OFF >> 2);
  rip_put_be32(img + 0x40024, 0);
  // Its data runs to the end of the image.
  rip_put_be32(img + PART_OFF + 0x2B8, DATA_OFF >> 2);
  rip_put_be32(img + PART_OFF + 0x2BC, (IMG_SIZE - PART_OFF - DATA_OFF) >> 2);
  return img;
}

static void check_wbfs(const uint8_t *w, size_t len, const uint8_t *img) {
  static const uint8_t zero[MIB / 2];
  const uint8_t *table = w + 512 + 0x100;
  int i;
  int unused_zero = 1;

  CHECK(len == OUT_SIZE);
  if (len != OUT_SIZE) {
    return;
  }
  CHECK_MEM(w, "WBFS", 4);
  CHECK(rip_be32(w + 4) == OUT_SIZE / 512);  // trimmed to the blocks written
  CHECK(w[8] == 9);
  CHECK(w[9] == 21);
  CHECK(w[12] == 1);  // slot 0 holds the disc
  CHECK(w[13] == 0);
  CHECK_MEM(w + 512, img, 0x100);
  CHECK(rip_be16(table + 0) == 1);
  CHECK(rip_be16(table + 2) == 0);
  CHECK(rip_be16(table + 4) == 2);
  CHECK(rip_be16(table + 6) == 3);
  for (i = 4; i < 4482; i++) {
    if (rip_be16(table + i * 2) != 0) {
      unused_zero = 0;
    }
  }
  CHECK(unused_zero);
  // The rest of the first block is zero: the free block table says all used.
  CHECK(w[512 + 0x100 + 4482 * 2] == 0);
  CHECK(w[BLK - 1] == 0 && w[BLK - 512] == 0);
  CHECK_MEM(w + 1 * BLK, img, BLK);
  CHECK_MEM(w + 2 * BLK, img + 2 * BLK, BLK);
  CHECK_MEM(w + 3 * BLK, img + 3 * BLK, MIB + MIB / 2);
  CHECK_MEM(w + 3 * BLK + MIB + MIB / 2, zero, MIB / 2);
}

void test_wbfs(void) {
  uint8_t *img = make_image();
  char dir[64];
  char iso[128], out[128], part[128];
  uint8_t *w;
  uint8_t *joined;
  size_t len, len1, len2;
  int calls = 0;
  FILE *f;

  test_tmpdir(dir, sizeof(dir));
  snprintf(iso, sizeof(iso), "%s/disc.iso", dir);
  snprintf(out, sizeof(out), "%s/RTSE01.wbfs", dir);
  f = fopen(iso, "wb");
  fwrite(img, 1, IMG_SIZE, f);
  fclose(f);

  // One file.
  CHECK(wbfs_from_iso(iso, out, 0, count_calls, &calls) == RIP_OK);
  CHECK(calls == 4);
  w = test_read_file(out, &len);
  CHECK(w != NULL);
  if (w) {
    check_wbfs(w, len, img);
  }
  free(w);

  // Split at 3 MiB (the extra bytes round down to 32 KiB): 3 + 3 + 2 MiB.
  CHECK(wbfs_from_iso(iso, out, 3 * MIB + 100, NULL, NULL) == RIP_OK);
  joined = malloc(OUT_SIZE);
  w = test_read_file(out, &len);
  CHECK(w && len == 3 * MIB);
  if (w && len == 3 * MIB) {
    memcpy(joined, w, len);
  }
  free(w);
  snprintf(part, sizeof(part), "%s/RTSE01.wbf1", dir);
  w = test_read_file(part, &len1);
  CHECK(w && len1 == 3 * MIB);
  if (w && len1 == 3 * MIB) {
    memcpy(joined + 3 * MIB, w, len1);
  }
  free(w);
  snprintf(part, sizeof(part), "%s/RTSE01.wbf2", dir);
  w = test_read_file(part, &len2);
  CHECK(w && len2 == 2 * MIB);
  if (w && len2 == 2 * MIB) {
    memcpy(joined + 6 * MIB, w, len2);
    check_wbfs(joined, OUT_SIZE, img);
  }
  free(w);
  free(joined);
  snprintf(part, sizeof(part), "%s/RTSE01.wbf3", dir);
  CHECK(access(part, F_OK) != 0);

  // Too many parts for USB Loader GX, and a split name it wouldn't find.
  CHECK(wbfs_from_iso(iso, out, 512 * 1024, NULL, NULL) == RIP_ERR);
  snprintf(part, sizeof(part), "%s/RTSE01.iso2", dir);
  CHECK(wbfs_from_iso(iso, part, 3 * MIB, NULL, NULL) == RIP_ERR);

  // Unsplit again: the old parts go, or they'd be read as part of it.
  CHECK(wbfs_from_iso(iso, out, 0, NULL, NULL) == RIP_OK);
  snprintf(part, sizeof(part), "%s/RTSE01.wbf1", dir);
  CHECK(access(part, F_OK) != 0);
  snprintf(part, sizeof(part), "%s/RTSE01.wbf2", dir);
  CHECK(access(part, F_OK) != 0);

  // A cancel leaves nothing behind, split or not.
  CHECK(wbfs_from_iso(iso, out, 0, cancel_at_two, NULL) == RIP_ECANCEL);
  CHECK(access(out, F_OK) != 0);
  CHECK(wbfs_from_iso(iso, out, 3 * MIB, cancel_at_two, NULL) == RIP_ECANCEL);
  CHECK(access(out, F_OK) != 0);
  snprintf(part, sizeof(part), "%s/RTSE01.wbf1", dir);
  CHECK(access(part, F_OK) != 0);

  // Not a Wii disc.
  rip_put_be32(img + 0x18, 0);
  f = fopen(iso, "wb");
  fwrite(img, 1, IMG_SIZE, f);
  fclose(f);
  CHECK(wbfs_from_iso(iso, out, 0, NULL, NULL) == RIP_ENOTSUP);
  CHECK(access(out, F_OK) != 0);

  free(img);
  remove(iso);
  remove(dir);
}
