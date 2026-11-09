#include "ripper/aes.h"

#include <string.h>

#include "ripper/common.h"

static const uint8_t SBOX[256] = {
    0x63, 0x7c, 0x77, 0x7b, 0xf2, 0x6b, 0x6f, 0xc5, 0x30, 0x01, 0x67, 0x2b, 0xfe, 0xd7, 0xab, 0x76, 0xca, 0x82, 0xc9,
    0x7d, 0xfa, 0x59, 0x47, 0xf0, 0xad, 0xd4, 0xa2, 0xaf, 0x9c, 0xa4, 0x72, 0xc0, 0xb7, 0xfd, 0x93, 0x26, 0x36, 0x3f,
    0xf7, 0xcc, 0x34, 0xa5, 0xe5, 0xf1, 0x71, 0xd8, 0x31, 0x15, 0x04, 0xc7, 0x23, 0xc3, 0x18, 0x96, 0x05, 0x9a, 0x07,
    0x12, 0x80, 0xe2, 0xeb, 0x27, 0xb2, 0x75, 0x09, 0x83, 0x2c, 0x1a, 0x1b, 0x6e, 0x5a, 0xa0, 0x52, 0x3b, 0xd6, 0xb3,
    0x29, 0xe3, 0x2f, 0x84, 0x53, 0xd1, 0x00, 0xed, 0x20, 0xfc, 0xb1, 0x5b, 0x6a, 0xcb, 0xbe, 0x39, 0x4a, 0x4c, 0x58,
    0xcf, 0xd0, 0xef, 0xaa, 0xfb, 0x43, 0x4d, 0x33, 0x85, 0x45, 0xf9, 0x02, 0x7f, 0x50, 0x3c, 0x9f, 0xa8, 0x51, 0xa3,
    0x40, 0x8f, 0x92, 0x9d, 0x38, 0xf5, 0xbc, 0xb6, 0xda, 0x21, 0x10, 0xff, 0xf3, 0xd2, 0xcd, 0x0c, 0x13, 0xec, 0x5f,
    0x97, 0x44, 0x17, 0xc4, 0xa7, 0x7e, 0x3d, 0x64, 0x5d, 0x19, 0x73, 0x60, 0x81, 0x4f, 0xdc, 0x22, 0x2a, 0x90, 0x88,
    0x46, 0xee, 0xb8, 0x14, 0xde, 0x5e, 0x0b, 0xdb, 0xe0, 0x32, 0x3a, 0x0a, 0x49, 0x06, 0x24, 0x5c, 0xc2, 0xd3, 0xac,
    0x62, 0x91, 0x95, 0xe4, 0x79, 0xe7, 0xc8, 0x37, 0x6d, 0x8d, 0xd5, 0x4e, 0xa9, 0x6c, 0x56, 0xf4, 0xea, 0x65, 0x7a,
    0xae, 0x08, 0xba, 0x78, 0x25, 0x2e, 0x1c, 0xa6, 0xb4, 0xc6, 0xe8, 0xdd, 0x74, 0x1f, 0x4b, 0xbd, 0x8b, 0x8a, 0x70,
    0x3e, 0xb5, 0x66, 0x48, 0x03, 0xf6, 0x0e, 0x61, 0x35, 0x57, 0xb9, 0x86, 0xc1, 0x1d, 0x9e, 0xe1, 0xf8, 0x98, 0x11,
    0x69, 0xd9, 0x8e, 0x94, 0x9b, 0x1e, 0x87, 0xe9, 0xce, 0x55, 0x28, 0xdf, 0x8c, 0xa1, 0x89, 0x0d, 0xbf, 0xe6, 0x42,
    0x68, 0x41, 0x99, 0x2d, 0x0f, 0xb0, 0x54, 0xbb, 0x16,
};

static uint8_t inv_sbox[256];
static uint32_t te[4][256];
static uint32_t td[4][256];
static int tables_ready;

static uint8_t gmul(uint8_t a, uint8_t b) {
  uint8_t r = 0;

  while (b) {
    if (b & 1) {
      r ^= a;
    }
    a = (uint8_t)((a << 1) ^ ((a & 0x80) ? 0x1B : 0));
    b >>= 1;
  }
  return r;
}

static uint32_t ror8(uint32_t v) {
  return (v >> 8) | (v << 24);
}

// The tables are derived once from the S-box. Key setup is the only entry point,
// so set up the first key before starting worker threads.
static void init_tables(void) {
  int i;
  int r;

  if (tables_ready) {
    return;
  }
  for (i = 0; i < 256; i++) {
    inv_sbox[SBOX[i]] = (uint8_t)i;
  }
  for (i = 0; i < 256; i++) {
    uint8_t s = SBOX[i];
    uint8_t si = inv_sbox[i];
    te[0][i] = ((uint32_t)gmul(s, 2) << 24) | ((uint32_t)s << 16) | ((uint32_t)s << 8) | gmul(s, 3);
    td[0][i] =
        ((uint32_t)gmul(si, 14) << 24) | ((uint32_t)gmul(si, 9) << 16) | ((uint32_t)gmul(si, 13) << 8) | gmul(si, 11);
    for (r = 1; r < 4; r++) {
      te[r][i] = ror8(te[r - 1][i]);
      td[r][i] = ror8(td[r - 1][i]);
    }
  }
  tables_ready = 1;
}

void rip_aes128_init_enc(struct rip_aes128 *ctx, const uint8_t key[16]) {
  static const uint32_t rcon[10] = {0x01000000, 0x02000000, 0x04000000, 0x08000000, 0x10000000,
                                    0x20000000, 0x40000000, 0x80000000, 0x1B000000, 0x36000000};
  uint32_t *rk = ctx->rk;
  int i;

  init_tables();
  for (i = 0; i < 4; i++) {
    rk[i] = rip_be32(key + 4 * i);
  }
  for (i = 0; i < 10; i++, rk += 4) {
    uint32_t t = rk[3];
    rk[4] = rk[0] ^ ((uint32_t)SBOX[(t >> 16) & 0xFF] << 24) ^ ((uint32_t)SBOX[(t >> 8) & 0xFF] << 16) ^
            ((uint32_t)SBOX[t & 0xFF] << 8) ^ SBOX[t >> 24] ^ rcon[i];
    rk[5] = rk[1] ^ rk[4];
    rk[6] = rk[2] ^ rk[5];
    rk[7] = rk[3] ^ rk[6];
  }
}

// Equivalent inverse cipher: reversed round keys with InvMixColumns on the inner rounds.
void rip_aes128_init_dec(struct rip_aes128 *ctx, const uint8_t key[16]) {
  struct rip_aes128 enc;
  int round;
  int i;

  rip_aes128_init_enc(&enc, key);
  for (round = 0; round <= 10; round++) {
    for (i = 0; i < 4; i++) {
      ctx->rk[4 * round + i] = enc.rk[4 * (10 - round) + i];
    }
  }
  for (i = 4; i < 40; i++) {
    uint32_t w = ctx->rk[i];
    ctx->rk[i] =
        td[0][SBOX[w >> 24]] ^ td[1][SBOX[(w >> 16) & 0xFF]] ^ td[2][SBOX[(w >> 8) & 0xFF]] ^ td[3][SBOX[w & 0xFF]];
  }
}

void rip_aes128_encrypt_block(const struct rip_aes128 *ctx, const uint8_t in[16], uint8_t out[16]) {
  const uint32_t *rk = ctx->rk;
  uint32_t s0 = rip_be32(in) ^ rk[0];
  uint32_t s1 = rip_be32(in + 4) ^ rk[1];
  uint32_t s2 = rip_be32(in + 8) ^ rk[2];
  uint32_t s3 = rip_be32(in + 12) ^ rk[3];
  uint32_t t0, t1, t2, t3;
  int round;

  for (round = 1; round < 10; round++) {
    rk += 4;
    t0 = te[0][s0 >> 24] ^ te[1][(s1 >> 16) & 0xFF] ^ te[2][(s2 >> 8) & 0xFF] ^ te[3][s3 & 0xFF] ^ rk[0];
    t1 = te[0][s1 >> 24] ^ te[1][(s2 >> 16) & 0xFF] ^ te[2][(s3 >> 8) & 0xFF] ^ te[3][s0 & 0xFF] ^ rk[1];
    t2 = te[0][s2 >> 24] ^ te[1][(s3 >> 16) & 0xFF] ^ te[2][(s0 >> 8) & 0xFF] ^ te[3][s1 & 0xFF] ^ rk[2];
    t3 = te[0][s3 >> 24] ^ te[1][(s0 >> 16) & 0xFF] ^ te[2][(s1 >> 8) & 0xFF] ^ te[3][s2 & 0xFF] ^ rk[3];
    s0 = t0;
    s1 = t1;
    s2 = t2;
    s3 = t3;
  }
  rk += 4;
  rip_put_be32(out, ((uint32_t)SBOX[s0 >> 24] << 24) ^ ((uint32_t)SBOX[(s1 >> 16) & 0xFF] << 16) ^
                        ((uint32_t)SBOX[(s2 >> 8) & 0xFF] << 8) ^ SBOX[s3 & 0xFF] ^ rk[0]);
  rip_put_be32(out + 4, ((uint32_t)SBOX[s1 >> 24] << 24) ^ ((uint32_t)SBOX[(s2 >> 16) & 0xFF] << 16) ^
                            ((uint32_t)SBOX[(s3 >> 8) & 0xFF] << 8) ^ SBOX[s0 & 0xFF] ^ rk[1]);
  rip_put_be32(out + 8, ((uint32_t)SBOX[s2 >> 24] << 24) ^ ((uint32_t)SBOX[(s3 >> 16) & 0xFF] << 16) ^
                            ((uint32_t)SBOX[(s0 >> 8) & 0xFF] << 8) ^ SBOX[s1 & 0xFF] ^ rk[2]);
  rip_put_be32(out + 12, ((uint32_t)SBOX[s3 >> 24] << 24) ^ ((uint32_t)SBOX[(s0 >> 16) & 0xFF] << 16) ^
                             ((uint32_t)SBOX[(s1 >> 8) & 0xFF] << 8) ^ SBOX[s2 & 0xFF] ^ rk[3]);
}

void rip_aes128_decrypt_block(const struct rip_aes128 *ctx, const uint8_t in[16], uint8_t out[16]) {
  const uint32_t *rk = ctx->rk;
  uint32_t s0 = rip_be32(in) ^ rk[0];
  uint32_t s1 = rip_be32(in + 4) ^ rk[1];
  uint32_t s2 = rip_be32(in + 8) ^ rk[2];
  uint32_t s3 = rip_be32(in + 12) ^ rk[3];
  uint32_t t0, t1, t2, t3;
  int round;

  for (round = 1; round < 10; round++) {
    rk += 4;
    t0 = td[0][s0 >> 24] ^ td[1][(s3 >> 16) & 0xFF] ^ td[2][(s2 >> 8) & 0xFF] ^ td[3][s1 & 0xFF] ^ rk[0];
    t1 = td[0][s1 >> 24] ^ td[1][(s0 >> 16) & 0xFF] ^ td[2][(s3 >> 8) & 0xFF] ^ td[3][s2 & 0xFF] ^ rk[1];
    t2 = td[0][s2 >> 24] ^ td[1][(s1 >> 16) & 0xFF] ^ td[2][(s0 >> 8) & 0xFF] ^ td[3][s3 & 0xFF] ^ rk[2];
    t3 = td[0][s3 >> 24] ^ td[1][(s2 >> 16) & 0xFF] ^ td[2][(s1 >> 8) & 0xFF] ^ td[3][s0 & 0xFF] ^ rk[3];
    s0 = t0;
    s1 = t1;
    s2 = t2;
    s3 = t3;
  }
  rk += 4;
  rip_put_be32(out, ((uint32_t)inv_sbox[s0 >> 24] << 24) ^ ((uint32_t)inv_sbox[(s3 >> 16) & 0xFF] << 16) ^
                        ((uint32_t)inv_sbox[(s2 >> 8) & 0xFF] << 8) ^ inv_sbox[s1 & 0xFF] ^ rk[0]);
  rip_put_be32(out + 4, ((uint32_t)inv_sbox[s1 >> 24] << 24) ^ ((uint32_t)inv_sbox[(s0 >> 16) & 0xFF] << 16) ^
                            ((uint32_t)inv_sbox[(s3 >> 8) & 0xFF] << 8) ^ inv_sbox[s2 & 0xFF] ^ rk[1]);
  rip_put_be32(out + 8, ((uint32_t)inv_sbox[s2 >> 24] << 24) ^ ((uint32_t)inv_sbox[(s1 >> 16) & 0xFF] << 16) ^
                            ((uint32_t)inv_sbox[(s0 >> 8) & 0xFF] << 8) ^ inv_sbox[s3 & 0xFF] ^ rk[2]);
  rip_put_be32(out + 12, ((uint32_t)inv_sbox[s3 >> 24] << 24) ^ ((uint32_t)inv_sbox[(s2 >> 16) & 0xFF] << 16) ^
                             ((uint32_t)inv_sbox[(s1 >> 8) & 0xFF] << 8) ^ inv_sbox[s0 & 0xFF] ^ rk[3]);
}

void rip_aes128_cbc_encrypt(const struct rip_aes128 *ctx, uint8_t iv[16], uint8_t *buf, size_t len) {
  size_t off;
  int i;

  for (off = 0; off + 16 <= len; off += 16) {
    for (i = 0; i < 16; i++) {
      buf[off + i] ^= iv[i];
    }
    rip_aes128_encrypt_block(ctx, buf + off, buf + off);
    memcpy(iv, buf + off, 16);
  }
}

void rip_aes128_cbc_decrypt(const struct rip_aes128 *ctx, uint8_t iv[16], uint8_t *buf, size_t len) {
  uint8_t cipher[16];
  size_t off;
  int i;

  for (off = 0; off + 16 <= len; off += 16) {
    memcpy(cipher, buf + off, 16);
    rip_aes128_decrypt_block(ctx, buf + off, buf + off);
    for (i = 0; i < 16; i++) {
      buf[off + i] ^= iv[i];
    }
    memcpy(iv, cipher, 16);
  }
}
