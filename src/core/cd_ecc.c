#include <string.h>

#include "ripper/cdrom.h"

// The tables follow ECMA-130: GF(2^8) with x^8 + x^4 + x^3 + x^2 + 1 for the
// Reed-Solomon product code, and the EDC polynomial
// (x^16 + x^15 + x^2 + 1)(x^16 + x^2 + x + 1), processed LSB first.

const uint8_t cd_sync[12] = {0x00, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x00};

static uint8_t ecc_f[256];
static uint8_t ecc_b[256];
static uint32_t edc_lut[256];
static int tables_ready;

static void init_tables(void) {
  unsigned i;

  if (tables_ready) {
    return;
  }
  for (i = 0; i < 256; i++) {
    unsigned j = (i << 1) ^ ((i & 0x80) ? 0x11D : 0);
    uint32_t edc = i;
    int k;

    ecc_f[i] = (uint8_t)j;
    ecc_b[i ^ j] = (uint8_t)i;
    for (k = 0; k < 8; k++) {
      edc = (edc >> 1) ^ ((edc & 1) ? 0xD8018001u : 0);
    }
    edc_lut[i] = edc;
  }
  tables_ready = 1;
}

uint32_t cd_edc(uint32_t edc, const uint8_t *p, size_t len) {
  init_tables();
  while (len--) {
    edc = (edc >> 8) ^ edc_lut[(edc ^ *p++) & 0xFF];
  }
  return edc;
}

// One parity block: P is 86 columns of 24 bytes, Q 52 diagonals of 43.
static void ecc_block(const uint8_t *src, unsigned major_count, unsigned minor_count, unsigned major_mult,
                      unsigned minor_inc, uint8_t *dest) {
  unsigned size = major_count * minor_count;
  unsigned major;

  for (major = 0; major < major_count; major++) {
    unsigned index = (major >> 1) * major_mult + (major & 1);
    uint8_t a = 0;
    uint8_t b = 0;
    unsigned minor;

    for (minor = 0; minor < minor_count; minor++) {
      uint8_t v = src[index];
      index += minor_inc;
      if (index >= size) {
        index -= size;
      }
      a ^= v;
      b ^= v;
      a = ecc_f[a];
    }
    a = ecc_b[ecc_f[a] ^ b];
    dest[major] = a;
    dest[major + major_count] = a ^ b;
  }
}

void cd_ecc_generate(uint8_t sector[CD_FRAME]) {
  init_tables();
  ecc_block(sector + 12, 86, 24, 2, 86, sector + 2076);
  ecc_block(sector + 12, 52, 43, 86, 88, sector + 2248);
}

int cd_ecc_verify(const uint8_t sector[CD_FRAME]) {
  uint8_t copy[CD_FRAME];

  memcpy(copy, sector, CD_FRAME);
  cd_ecc_generate(copy);
  return memcmp(copy + 2076, sector + 2076, CD_FRAME - 2076) == 0;
}

int cd_sector_ok(const uint8_t s[CD_FRAME]) {
  uint32_t edc;

  if (memcmp(s, cd_sync, sizeof(cd_sync)) != 0) {
    return -1;
  }
  switch (s[15]) {
  case 1:
    edc = cd_edc(0, s, 2064);
    return edc == ((uint32_t)s[2064] | (uint32_t)s[2065] << 8 | (uint32_t)s[2066] << 16 | (uint32_t)s[2067] << 24);
  case 2:
    if (s[18] & 0x20) {  // form 2: EDC over subheader and 2324 bytes, or none at all
      uint32_t stored = (uint32_t)s[2348] | (uint32_t)s[2349] << 8 | (uint32_t)s[2350] << 16 | (uint32_t)s[2351] << 24;
      return stored == 0 || cd_edc(0, s + 16, 2332) == stored;
    }
    edc = cd_edc(0, s + 16, 2056);
    return edc == ((uint32_t)s[2072] | (uint32_t)s[2073] << 8 | (uint32_t)s[2074] << 16 | (uint32_t)s[2075] << 24);
  default:
    return -1;
  }
}

void cd_make_mode1(uint8_t s[CD_FRAME], uint32_t lba) {
  uint32_t f = lba + CD_PREGAP;
  uint32_t edc;

  memcpy(s, cd_sync, sizeof(cd_sync));
  s[12] = cd_bcd((uint8_t)(f / (60 * CD_FRAMES_PER_SEC)));
  s[13] = cd_bcd((uint8_t)((f / CD_FRAMES_PER_SEC) % 60));
  s[14] = cd_bcd((uint8_t)(f % CD_FRAMES_PER_SEC));
  s[15] = 1;
  edc = cd_edc(0, s, 2064);
  s[2064] = (uint8_t)edc;
  s[2065] = (uint8_t)(edc >> 8);
  s[2066] = (uint8_t)(edc >> 16);
  s[2067] = (uint8_t)(edc >> 24);
  memset(s + 2068, 0, 8);
  cd_ecc_generate(s);
}
