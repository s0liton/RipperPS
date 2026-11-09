#include "ripper/hash.h"

#include <string.h>

#include "ripper/common.h"

/* CRC-32 (IEEE 802.3, reflected) */

static uint32_t crc_table[256];
static int crc_ready;

static void crc_init(void) {
  uint32_t i;
  int k;

  for (i = 0; i < 256; i++) {
    uint32_t c = i;
    for (k = 0; k < 8; k++) {
      c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
    }
    crc_table[i] = c;
  }
  crc_ready = 1;
}

uint32_t rip_crc32(uint32_t crc, const void *data, size_t len) {
  const uint8_t *p = data;

  if (!crc_ready) {
    crc_init();
  }
  crc = ~crc;
  while (len--) {
    crc = crc_table[(crc ^ *p++) & 0xFF] ^ (crc >> 8);
  }
  return ~crc;
}

/* MD5 (RFC 1321) */

static uint32_t rol(uint32_t v, int n) {
  return (v << n) | (v >> (32 - n));
}

static void md5_block(struct rip_md5 *ctx, const uint8_t *p) {
  static const uint32_t k[64] = {
      0xd76aa478, 0xe8c7b756, 0x242070db, 0xc1bdceee, 0xf57c0faf, 0x4787c62a, 0xa8304613, 0xfd469501,
      0x698098d8, 0x8b44f7af, 0xffff5bb1, 0x895cd7be, 0x6b901122, 0xfd987193, 0xa679438e, 0x49b40821,
      0xf61e2562, 0xc040b340, 0x265e5a51, 0xe9b6c7aa, 0xd62f105d, 0x02441453, 0xd8a1e681, 0xe7d3fbc8,
      0x21e1cde6, 0xc33707d6, 0xf4d50d87, 0x455a14ed, 0xa9e3e905, 0xfcefa3f8, 0x676f02d9, 0x8d2a4c8a,
      0xfffa3942, 0x8771f681, 0x6d9d6122, 0xfde5380c, 0xa4beea44, 0x4bdecfa9, 0xf6bb4b60, 0xbebfbc70,
      0x289b7ec6, 0xeaa127fa, 0xd4ef3085, 0x04881d05, 0xd9d4d039, 0xe6db99e5, 0x1fa27cf8, 0xc4ac5665,
      0xf4292244, 0x432aff97, 0xab9423a7, 0xfc93a039, 0x655b59c3, 0x8f0ccc92, 0xffeff47d, 0x85845dd1,
      0x6fa87e4f, 0xfe2ce6e0, 0xa3014314, 0x4e0811a1, 0xf7537e82, 0xbd3af235, 0x2ad7d2bb, 0xeb86d391,
  };
  static const int r[64] = {7,  12, 17, 22, 7,  12, 17, 22, 7,  12, 17, 22, 7,  12, 17, 22, 5,  9,  14, 20, 5,  9,
                            14, 20, 5,  9,  14, 20, 5,  9,  14, 20, 4,  11, 16, 23, 4,  11, 16, 23, 4,  11, 16, 23,
                            4,  11, 16, 23, 6,  10, 15, 21, 6,  10, 15, 21, 6,  10, 15, 21, 6,  10, 15, 21};
  uint32_t w[16];
  uint32_t a = ctx->h[0], b = ctx->h[1], c = ctx->h[2], d = ctx->h[3];
  int i;

  for (i = 0; i < 16; i++) {
    w[i] = rip_le32(p + 4 * i);
  }
  for (i = 0; i < 64; i++) {
    uint32_t f;
    int g;
    if (i < 16) {
      f = (b & c) | (~b & d);
      g = i;
    } else if (i < 32) {
      f = (d & b) | (~d & c);
      g = (5 * i + 1) & 15;
    } else if (i < 48) {
      f = b ^ c ^ d;
      g = (3 * i + 5) & 15;
    } else {
      f = c ^ (b | ~d);
      g = (7 * i) & 15;
    }
    f += a + k[i] + w[g];
    a = d;
    d = c;
    c = b;
    b += rol(f, r[i]);
  }
  ctx->h[0] += a;
  ctx->h[1] += b;
  ctx->h[2] += c;
  ctx->h[3] += d;
}

void rip_md5_init(struct rip_md5 *ctx) {
  ctx->h[0] = 0x67452301;
  ctx->h[1] = 0xefcdab89;
  ctx->h[2] = 0x98badcfe;
  ctx->h[3] = 0x10325476;
  ctx->len = 0;
  ctx->used = 0;
}

void rip_md5_update(struct rip_md5 *ctx, const void *data, size_t len) {
  const uint8_t *p = data;

  ctx->len += len;
  if (ctx->used) {
    size_t take = 64 - ctx->used < len ? 64 - ctx->used : len;
    memcpy(ctx->buf + ctx->used, p, take);
    ctx->used += take;
    p += take;
    len -= take;
    if (ctx->used < 64) {
      return;
    }
    md5_block(ctx, ctx->buf);
    ctx->used = 0;
  }
  for (; len >= 64; p += 64, len -= 64) {
    md5_block(ctx, p);
  }
  memcpy(ctx->buf, p, len);
  ctx->used = len;
}

void rip_md5_final(struct rip_md5 *ctx, uint8_t out[16]) {
  uint64_t bits = ctx->len * 8;
  uint8_t pad[72];
  size_t padlen = (ctx->used < 56 ? 56 : 120) - ctx->used;
  int i;

  memset(pad, 0, sizeof(pad));
  pad[0] = 0x80;
  for (i = 0; i < 8; i++) {
    pad[padlen + i] = (uint8_t)(bits >> (8 * i));
  }
  rip_md5_update(ctx, pad, padlen + 8);
  for (i = 0; i < 4; i++) {
    rip_put_le32(out + 4 * i, ctx->h[i]);
  }
}

/* SHA-1 (FIPS 180-4) */

static void sha1_block(struct rip_sha1 *ctx, const uint8_t *p) {
  uint32_t w[80];
  uint32_t a = ctx->h[0], b = ctx->h[1], c = ctx->h[2], d = ctx->h[3], e = ctx->h[4];
  int i;

  for (i = 0; i < 16; i++) {
    w[i] = rip_be32(p + 4 * i);
  }
  for (i = 16; i < 80; i++) {
    w[i] = rol(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
  }
  for (i = 0; i < 80; i++) {
    uint32_t f, k, t;
    if (i < 20) {
      f = (b & c) | (~b & d);
      k = 0x5A827999;
    } else if (i < 40) {
      f = b ^ c ^ d;
      k = 0x6ED9EBA1;
    } else if (i < 60) {
      f = (b & c) | (b & d) | (c & d);
      k = 0x8F1BBCDC;
    } else {
      f = b ^ c ^ d;
      k = 0xCA62C1D6;
    }
    t = rol(a, 5) + f + e + k + w[i];
    e = d;
    d = c;
    c = rol(b, 30);
    b = a;
    a = t;
  }
  ctx->h[0] += a;
  ctx->h[1] += b;
  ctx->h[2] += c;
  ctx->h[3] += d;
  ctx->h[4] += e;
}

void rip_sha1_init(struct rip_sha1 *ctx) {
  ctx->h[0] = 0x67452301;
  ctx->h[1] = 0xEFCDAB89;
  ctx->h[2] = 0x98BADCFE;
  ctx->h[3] = 0x10325476;
  ctx->h[4] = 0xC3D2E1F0;
  ctx->len = 0;
  ctx->used = 0;
}

void rip_sha1_update(struct rip_sha1 *ctx, const void *data, size_t len) {
  const uint8_t *p = data;

  ctx->len += len;
  if (ctx->used) {
    size_t take = 64 - ctx->used < len ? 64 - ctx->used : len;
    memcpy(ctx->buf + ctx->used, p, take);
    ctx->used += take;
    p += take;
    len -= take;
    if (ctx->used < 64) {
      return;
    }
    sha1_block(ctx, ctx->buf);
    ctx->used = 0;
  }
  for (; len >= 64; p += 64, len -= 64) {
    sha1_block(ctx, p);
  }
  memcpy(ctx->buf, p, len);
  ctx->used = len;
}

void rip_sha1_final(struct rip_sha1 *ctx, uint8_t out[20]) {
  uint64_t bits = ctx->len * 8;
  uint8_t pad[72];
  size_t padlen = (ctx->used < 56 ? 56 : 120) - ctx->used;
  int i;

  memset(pad, 0, sizeof(pad));
  pad[0] = 0x80;
  for (i = 0; i < 8; i++) {
    pad[padlen + i] = (uint8_t)(bits >> (56 - 8 * i));
  }
  rip_sha1_update(ctx, pad, padlen + 8);
  for (i = 0; i < 5; i++) {
    rip_put_be32(out + 4 * i, ctx->h[i]);
  }
}

/* All three at once */

void rip_hash_init(struct rip_hash_ctx *ctx) {
  ctx->size = 0;
  ctx->crc = 0;
  rip_md5_init(&ctx->md5);
  rip_sha1_init(&ctx->sha1);
}

void rip_hash_update(struct rip_hash_ctx *ctx, const void *data, size_t len) {
  ctx->size += len;
  ctx->crc = rip_crc32(ctx->crc, data, len);
  rip_md5_update(&ctx->md5, data, len);
  rip_sha1_update(&ctx->sha1, data, len);
}

void rip_hash_final(struct rip_hash_ctx *ctx, struct rip_hashes *out) {
  out->size = ctx->size;
  out->crc32 = ctx->crc;
  rip_md5_final(&ctx->md5, out->md5);
  rip_sha1_final(&ctx->sha1, out->sha1);
}

/* SHA-256 (FIPS 180-4) */

static const uint32_t SHA256_K[64] = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
    0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
    0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
    0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
    0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
    0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
    0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
    0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2};

static uint32_t ror32(uint32_t x, int n) {
  return (x >> n) | (x << (32 - n));
}

static void sha256_block(struct rip_sha256 *c, const uint8_t *p) {
  uint32_t w[64];
  uint32_t a, b, cc, d, e, f, g, h;
  int i;

  for (i = 0; i < 16; i++) {
    w[i] = ((uint32_t)p[4 * i] << 24) | ((uint32_t)p[4 * i + 1] << 16) | ((uint32_t)p[4 * i + 2] << 8) | p[4 * i + 3];
  }
  for (i = 16; i < 64; i++) {
    uint32_t s0 = ror32(w[i - 15], 7) ^ ror32(w[i - 15], 18) ^ (w[i - 15] >> 3);
    uint32_t s1 = ror32(w[i - 2], 17) ^ ror32(w[i - 2], 19) ^ (w[i - 2] >> 10);
    w[i] = w[i - 16] + s0 + w[i - 7] + s1;
  }
  a = c->h[0];
  b = c->h[1];
  cc = c->h[2];
  d = c->h[3];
  e = c->h[4];
  f = c->h[5];
  g = c->h[6];
  h = c->h[7];
  for (i = 0; i < 64; i++) {
    uint32_t s1 = ror32(e, 6) ^ ror32(e, 11) ^ ror32(e, 25);
    uint32_t ch = (e & f) ^ (~e & g);
    uint32_t t1 = h + s1 + ch + SHA256_K[i] + w[i];
    uint32_t s0 = ror32(a, 2) ^ ror32(a, 13) ^ ror32(a, 22);
    uint32_t maj = (a & b) ^ (a & cc) ^ (b & cc);
    uint32_t t2 = s0 + maj;
    h = g;
    g = f;
    f = e;
    e = d + t1;
    d = cc;
    cc = b;
    b = a;
    a = t1 + t2;
  }
  c->h[0] += a;
  c->h[1] += b;
  c->h[2] += cc;
  c->h[3] += d;
  c->h[4] += e;
  c->h[5] += f;
  c->h[6] += g;
  c->h[7] += h;
}

void rip_sha256_init(struct rip_sha256 *c) {
  static const uint32_t iv[8] = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
                                 0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};

  memcpy(c->h, iv, sizeof(iv));
  c->len = 0;
  c->used = 0;
}

void rip_sha256_update(struct rip_sha256 *c, const void *data, size_t len) {
  const uint8_t *p = data;

  c->len += len;
  while (len) {
    size_t take = 64 - c->used;
    if (take > len) {
      take = len;
    }
    memcpy(c->buf + c->used, p, take);
    c->used += take;
    p += take;
    len -= take;
    if (c->used == 64) {
      sha256_block(c, c->buf);
      c->used = 0;
    }
  }
}

void rip_sha256_final(struct rip_sha256 *c, uint8_t out[32]) {
  uint64_t bits = c->len * 8;
  uint8_t pad = 0x80;
  uint8_t zero = 0;
  uint8_t len_be[8];
  int i;

  rip_sha256_update(c, &pad, 1);
  while (c->used != 56) {
    rip_sha256_update(c, &zero, 1);
  }
  for (i = 0; i < 8; i++) {
    len_be[i] = (uint8_t)(bits >> (56 - 8 * i));
  }
  rip_sha256_update(c, len_be, 8);
  for (i = 0; i < 8; i++) {
    out[4 * i] = (uint8_t)(c->h[i] >> 24);
    out[4 * i + 1] = (uint8_t)(c->h[i] >> 16);
    out[4 * i + 2] = (uint8_t)(c->h[i] >> 8);
    out[4 * i + 3] = (uint8_t)c->h[i];
  }
}
