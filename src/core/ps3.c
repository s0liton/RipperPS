#include "ripper/ps3.h"

#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "ripper/aes.h"
#include "ripper/common.h"
#include "ripper/log.h"
#include "ripper/util.h"

#define PS3_CHUNK_SECTORS 512  // 1 MiB per read/decrypt/write pass

int ps3_parse_layout(const uint8_t sector0[PS3_SECTOR], struct ps3_layout *out) {
  uint32_t plain = rip_be32(sector0);
  uint32_t i;
  unsigned n = 0;

  memset(out, 0, sizeof(*out));
  if (plain == 0 || 8 + (size_t)plain * 8 > PS3_SECTOR || plain * 2 - 1 > PS3_MAX_REGIONS) {
    return RIP_ERR;
  }
  for (i = 0; i < plain; i++) {
    uint32_t first = rip_be32(sector0 + 8 + 8 * i);
    uint32_t last = rip_be32(sector0 + 12 + 8 * i);

    if (last < first || (i == 0 && first != 0)) {
      return RIP_ERR;
    }
    if (i > 0) {
      uint32_t prev_last = out->regions[n - 1].last;
      if (first <= prev_last) {
        return RIP_ERR;
      }
      if (first > prev_last + 1) {
        out->regions[n].first = prev_last + 1;
        out->regions[n].last = first - 1;
        out->regions[n].encrypted = 1;
        n++;
      }
    }
    out->regions[n].first = first;
    out->regions[n].last = last;
    out->regions[n].encrypted = 0;
    n++;
  }
  out->count = n;
  out->total_sectors = out->regions[n - 1].last + 1;
  return RIP_OK;
}

int ps3_sector_encrypted(const struct ps3_layout *layout, uint32_t lba) {
  unsigned i;

  for (i = 0; i < layout->count; i++) {
    if (lba >= layout->regions[i].first && lba <= layout->regions[i].last) {
      return layout->regions[i].encrypted;
    }
  }
  return 0;
}

void ps3_key_from_d1(const uint8_t d1[16], uint8_t key[16]) {
  static const uint8_t d1_key[16] = {0x38, 0x0B, 0xCF, 0x0B, 0x53, 0x45, 0x5B, 0x3C,
                                     0x78, 0x17, 0xAB, 0x4F, 0xA3, 0xBA, 0x90, 0xED};
  static const uint8_t d1_iv[16] = {0x69, 0x47, 0x47, 0x72, 0xAF, 0x6F, 0xDA, 0xB3,
                                    0x42, 0x74, 0x3A, 0xEF, 0xAA, 0x18, 0x62, 0x87};
  struct rip_aes128 aes;
  uint8_t iv[16];

  memcpy(iv, d1_iv, 16);
  memcpy(key, d1, 16);
  rip_aes128_init_enc(&aes, d1_key);
  rip_aes128_cbc_encrypt(&aes, iv, key, 16);
}

static void sector_iv(uint32_t lba, uint8_t iv[16]) {
  memset(iv, 0, 12);
  rip_put_be32(iv + 12, lba);
}

void ps3_crypt_sectors(const struct ps3_layout *layout, const uint8_t key[16], int decrypt, uint32_t first_lba,
                       uint8_t *buf, uint32_t count) {
  struct rip_aes128 aes;
  uint8_t iv[16];
  uint32_t i;

  if (decrypt) {
    rip_aes128_init_dec(&aes, key);
  } else {
    rip_aes128_init_enc(&aes, key);
  }
  for (i = 0; i < count; i++) {
    uint32_t lba = first_lba + i;
    if (!ps3_sector_encrypted(layout, lba)) {
      continue;
    }
    sector_iv(lba, iv);
    if (decrypt) {
      rip_aes128_cbc_decrypt(&aes, iv, buf + (size_t)i * PS3_SECTOR, PS3_SECTOR);
    } else {
      rip_aes128_cbc_encrypt(&aes, iv, buf + (size_t)i * PS3_SECTOR, PS3_SECTOR);
    }
  }
}

int ps3_decrypt_file(const char *path, const uint8_t key[16], ps3_progress_fn progress, void *user) {
  struct ps3_layout *layout = NULL;
  uint8_t *buf = NULL;
  uint64_t enc_total = 0;
  uint64_t enc_done = 0;
  int fd;
  int rc = RIP_ERR;
  unsigned i;

  fd = open(path, O_RDWR);
  if (fd < 0) {
    return RIP_ENOENT;
  }
  layout = malloc(sizeof(*layout));
  buf = malloc((size_t)PS3_CHUNK_SECTORS * PS3_SECTOR);
  if (!layout || !buf) {
    rc = RIP_ENOMEM;
    goto out;
  }
  if (rip_seek_read(fd, 0, buf, PS3_SECTOR) != 0 || ps3_parse_layout(buf, layout) != RIP_OK) {
    RIP_ERROR("%s: no PS3 region table in sector 0", path);
    goto out;
  }
  for (i = 0; i < layout->count; i++) {
    if (layout->regions[i].encrypted) {
      enc_total += (uint64_t)layout->regions[i].last - layout->regions[i].first + 1;
    }
  }

  for (i = 0; i < layout->count; i++) {
    const struct ps3_region *reg = &layout->regions[i];
    uint64_t lba;

    if (!reg->encrypted) {
      continue;
    }
    for (lba = reg->first; lba <= reg->last; lba += PS3_CHUNK_SECTORS) {
      uint32_t n = (uint32_t)((reg->last - lba + 1) < PS3_CHUNK_SECTORS ? (reg->last - lba + 1) : PS3_CHUNK_SECTORS);
      uint64_t off = lba * PS3_SECTOR;
      size_t bytes = (size_t)n * PS3_SECTOR;

      if (rip_seek_read(fd, off, buf, bytes) != 0) {
        RIP_ERROR("%s: read failed at sector %llu", path, (unsigned long long)lba);
        goto out;
      }
      ps3_crypt_sectors(layout, key, 1, (uint32_t)lba, buf, n);
      if (rip_seek_write(fd, off, buf, bytes) != 0) {
        RIP_ERROR("%s: write failed at sector %llu", path, (unsigned long long)lba);
        goto out;
      }
      enc_done += n;
      if (progress) {
        progress(enc_done * PS3_SECTOR, enc_total * PS3_SECTOR, user);
      }
    }
  }
  rc = fsync(fd) == 0 ? RIP_OK : RIP_ERR;

out:
  free(buf);
  free(layout);
  close(fd);
  return rc;
}
