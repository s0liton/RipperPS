// OmniDrive's raw read, and GameCube, Wii and Wii U discs through it. The
// drive hands over frames still scrambled with Nintendo's seeds; they're
// descrambled here and checked against their EDC. After redumper (GPL-3):
// dvd/dvd_scrambler.ixx, dvd/nintendo.ixx, dvd/bd_scrambler.ixx.

#include "ripper/omnidrive.h"

#include <limits.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>

#include "ripper/common.h"
#include "ripper/log.h"
#include "ripper/mmc.h"

// Under 64 KiB a transfer: 31 x 2064 and 31 x 2070 both fit.
#define RAW_PER_READ 31
#define DVD_TABLE (2048 * DVD_ECC_FRAMES)

int omni_read(struct rip_transport *t, enum omni_disc type, int raw_addressing, int32_t address, uint32_t count,
              void *buf, size_t frame_size, struct rip_sense *sense) {
  uint8_t cdb[12] = {0xC0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0};

  cdb[1] = (uint8_t)((unsigned)type | (raw_addressing ? 0x04 : 0));
  rip_put_be32(cdb + 2, (uint32_t)address);
  rip_put_be32(cdb + 6, count);
  return rip_scsi(t, cdb, sizeof(cdb), RIP_DIR_IN, buf, (size_t)count * frame_size, MMC_READ_TIMEOUT_MS, sense);
}

uint32_t dvd_edc(const uint8_t *p, size_t n) {
  uint32_t crc = 0;
  size_t i;

  for (i = 0; i < n; i++) {
    int b;
    crc ^= (uint32_t)p[i] << 24;
    for (b = 0; b < 8; b++) {
      crc = (crc & 0x80000000u) ? (crc << 1) ^ 0x80000011u : crc << 1;
    }
  }
  return crc;
}

// ECMA-267's scrambling bytes, one run through all 16 presets.
static uint8_t g_table[DVD_TABLE];
static pthread_once_t g_table_once = PTHREAD_ONCE_INIT;

static void make_table(void) {
  uint16_t reg = 0x0001;
  size_t i;

  for (i = 0; i < DVD_TABLE; i++) {
    int b;
    g_table[i] = (uint8_t)reg;
    for (b = 0; b < CHAR_BIT; b++) {
      unsigned lsb = ((reg >> 14) ^ (reg >> 10)) & 1u;
      reg = (uint16_t)(((reg << 1) & 0x7FFF) | lsb);
    }
  }
}

void dvd_descramble(uint8_t *data, uint32_t psn, int nintendo_key) {
  uint32_t group = (psn >> 4) & 0xF;
  uint32_t offset = group * 2048;
  size_t i;

  pthread_once(&g_table_once, make_table);
  if (nintendo_key >= 0) {
    offset = (((uint32_t)nintendo_key ^ group) * 2048) + (7 * 2048) + 1024;
  }
  for (i = 0; i < 2048; i++) {
    // The table wraps to its second byte, not its first.
    uint32_t at = offset + (uint32_t)i;
    if (at >= DVD_TABLE) {
      at -= DVD_TABLE - 1;
    }
    data[i] ^= g_table[at];
  }
}

int dvd_frame_fix(uint8_t frame[DVD_RAW_FRAME], int nintendo_key) {
  uint32_t psn = ((uint32_t)frame[1] << 16) | ((uint32_t)frame[2] << 8) | frame[3];

  dvd_descramble(frame + 12, psn, nintendo_key);
  return dvd_edc(frame, DVD_RAW_FRAME - 4) == rip_be32(frame + DVD_RAW_FRAME - 4);
}

int nintendo_key_derive(const uint8_t bytes[8]) {
  unsigned sum = 0;
  int i;

  for (i = 0; i < 8; i++) {
    sum += bytes[i];
  }
  return (int)(((sum >> 4) + sum) & 0xF);
}

static uint32_t bd_seed_nintendo(int32_t lba) {
  uint32_t m = (uint32_t)lba & 0xFFFE0u;
  uint32_t local = m & 0xFFFF;
  uint32_t k = local >> 8;
  uint32_t slope = (local & 0xFF) << 7;
  uint32_t layer = (m >> 16) & 0xF;
  uint32_t jump4k = ((local >> 12) & 0x3) << 9;
  uint32_t jump32k = (local >> 15) << 12;
  uint32_t toggle = (k & 1) << 5;
  uint32_t decay = k << 4;

  return slope + 1248 + layer + toggle + jump4k + jump32k - decay;
}

void bd_descramble(uint8_t *frame, int32_t lba, int nintendo) {
  // ISO/IEC 30190.
  uint16_t seed = (uint16_t)(nintendo ? bd_seed_nintendo(lba) : ((uint32_t)lba + 0x100000u) >> 5);
  uint16_t reg = (uint16_t)(0x8000u | (seed & 0x7FFFu));
  size_t i;

  for (i = 0; i < 2052; i++) {
    int b;
    frame[i] ^= (uint8_t)reg;
    for (b = 0; b < CHAR_BIT; b++) {
      unsigned lsb = ((reg >> 15) ^ (reg >> 14) ^ (reg >> 12) ^ (reg >> 3)) & 1u;
      reg = (uint16_t)((reg << 1) | lsb);
    }
  }
}

/* Reader */

struct nintendo_ctx {
  struct rip_transport *t;
  int bd;
  int key;  // DVD: the key past the first ECC block
  uint8_t *raw;
};

// One read's frames to 2048-byte sectors.
static int nintendo_frames(struct nintendo_ctx *n, int32_t lba, uint32_t count, uint8_t *out) {
  size_t frame = n->bd ? BD_RAW_FRAME : DVD_RAW_FRAME;
  struct rip_sense sense = {0, 0, 0};
  uint32_t i;
  int rc = omni_read(n->t, n->bd ? OMNI_BD : OMNI_DVD, 0, lba, count, n->raw, frame, &sense);

  if (rc != RIP_OK) {
    RIP_DEBUG("C0 lba %d x%u: %d, sense %02x/%02x/%02x", lba, count, rc, sense.key, sense.asc, sense.ascq);
    return rc;
  }
  for (i = 0; i < count; i++) {
    uint8_t *f = n->raw + i * frame;
    int32_t at = lba + (int32_t)i;
    if (n->bd) {
      bd_descramble(f, at, 1);
      if (dvd_edc(f, 2048) != rip_be32(f + 2048)) {
        RIP_DEBUG("Wii U sector %d: EDC mismatch", at);
        return RIP_ERR;
      }
      memcpy(out + (size_t)i * 2048, f, 2048);
    } else {
      if (!dvd_frame_fix(f, at < DVD_ECC_FRAMES ? 0 : n->key)) {
        RIP_DEBUG("Nintendo sector %d: EDC mismatch", at);
        return RIP_ERR;
      }
      // Nintendo's 2048 bytes start at CPR_MAI, 6 bytes before the usual data.
      memcpy(out + (size_t)i * 2048, f + 6, 2048);
    }
  }
  return RIP_OK;
}

static int nintendo_read(void *ctx, uint64_t lba, uint32_t count, void *buf) {
  uint8_t *out = buf;

  while (count) {
    uint32_t n = count < RAW_PER_READ ? count : RAW_PER_READ;
    int rc = nintendo_frames(ctx, (int32_t)lba, n, out);
    if (rc != RIP_OK) {
      return rc;
    }
    lba += n;
    count -= n;
    out += (size_t)n * 2048;
  }
  return RIP_OK;
}

static void nintendo_close(void *ctx) {
  struct nintendo_ctx *n = ctx;

  free(n->raw);
  free(n);
}

static const struct rip_reader_ops nintendo_ops = {nintendo_read, nintendo_close};

int rip_reader_open_nintendo(struct rip_reader *r, struct rip_transport *t, int bd, uint64_t sectors) {
  struct nintendo_ctx *n;

  memset(r, 0, sizeof(*r));
  n = calloc(1, sizeof(*n));
  // Page-aligned, as the USB pass-through wants.
  if (!n || posix_memalign((void **)&n->raw, 4096, (size_t)RAW_PER_READ * BD_RAW_FRAME) != 0) {
    free(n);
    return RIP_ENOMEM;
  }
  n->t = t;
  n->bd = bd;
  if (!bd) {
    struct rip_sense sense = {0, 0, 0};
    int rc = omni_read(t, OMNI_DVD, 0, 0, 1, n->raw, DVD_RAW_FRAME, &sense);
    if (rc != RIP_OK || !dvd_frame_fix(n->raw, 0)) {
      RIP_ERROR("OmniDrive: the first sector of this Nintendo disc can't be read (%d, sense %02x/%02x/%02x)", rc,
                sense.key, sense.asc, sense.ascq);
      nintendo_close(n);
      return rc != RIP_OK ? rc : RIP_ERR;
    }
    n->key = nintendo_key_derive(n->raw + 6);
    RIP_INFO("Nintendo disc: key %d", n->key);
  }
  r->ops = &nintendo_ops;
  r->ctx = n;
  r->sector_size = 2048;
  r->sector_count = sectors;
  r->max_sectors = RAW_PER_READ;
  return RIP_OK;
}
