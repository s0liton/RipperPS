#include "ripper/rsa.h"

#include <string.h>

#include "ripper/common.h"

// Little-endian arrays of 32-bit limbs, multiplied in Montgomery form (CIOS).
#define LIMBS (RIP_RSA_MAX_BYTES / 4)

static void from_be(uint32_t *x, size_t s, const uint8_t *b, size_t len) {
  size_t i;

  memset(x, 0, s * sizeof(*x));
  for (i = 0; i < len; i++) {
    x[i / 4] |= (uint32_t)b[len - 1 - i] << (8 * (i % 4));
  }
}

static void to_be(uint8_t *b, size_t len, const uint32_t *x) {
  size_t i;

  for (i = 0; i < len; i++) {
    b[len - 1 - i] = (uint8_t)(x[i / 4] >> (8 * (i % 4)));
  }
}

// a >= b over s limbs plus a top limb for a.
static int geq(const uint32_t *a, uint32_t a_top, const uint32_t *b, size_t s) {
  size_t i = s;

  if (a_top) {
    return 1;
  }
  while (i--) {
    if (a[i] != b[i]) {
      return a[i] > b[i];
    }
  }
  return 1;
}

static void sub(uint32_t *a, const uint32_t *b, size_t s) {
  uint64_t borrow = 0;
  size_t i;

  for (i = 0; i < s; i++) {
    uint64_t d = (uint64_t)a[i] - b[i] - borrow;
    a[i] = (uint32_t)d;
    borrow = (d >> 32) & 1;
  }
}

// r = a * b / 2^(32s) mod n, for a below 2^(32s) and b below n.
static void mont_mul(uint32_t *r, const uint32_t *a, const uint32_t *b, const uint32_t *n, uint32_t n0, size_t s) {
  uint32_t t[LIMBS + 2];
  size_t i;
  size_t j;

  memset(t, 0, sizeof(t));
  for (i = 0; i < s; i++) {
    uint64_t c = 0;
    uint32_t m;
    for (j = 0; j < s; j++) {
      c += (uint64_t)a[j] * b[i] + t[j];
      t[j] = (uint32_t)c;
      c >>= 32;
    }
    c += t[s];
    t[s] = (uint32_t)c;
    t[s + 1] = (uint32_t)(c >> 32);
    m = t[0] * n0;
    c = ((uint64_t)m * n[0] + t[0]) >> 32;
    for (j = 1; j < s; j++) {
      c += (uint64_t)m * n[j] + t[j];
      t[j - 1] = (uint32_t)c;
      c >>= 32;
    }
    c += t[s];
    t[s - 1] = (uint32_t)c;
    t[s] = t[s + 1] + (uint32_t)(c >> 32);
  }
  if (geq(t, t[s], n, s)) {
    sub(t, n, s);
  }
  memcpy(r, t, s * sizeof(*r));
}

int rip_rsa_modexp(const uint8_t *base, const uint8_t *exp, size_t exp_len, const uint8_t *n, size_t n_len,
                   uint8_t *out) {
  uint32_t nn[LIMBS];
  uint32_t r2[LIMBS];
  uint32_t b[LIMBS];
  uint32_t x[LIMBS];
  uint32_t one[LIMBS];
  uint32_t inv;
  size_t s = (n_len + 3) / 4;
  size_t i;
  int k;

  if (n_len == 0 || n_len > RIP_RSA_MAX_BYTES || !(n[n_len - 1] & 1)) {
    return RIP_ERR;
  }
  from_be(nn, s, n, n_len);
  // -1/n mod 2^32 by Newton's iteration; an odd n is its own inverse mod 8.
  inv = nn[0];
  for (k = 0; k < 4; k++) {
    inv *= 2 - nn[0] * inv;
  }
  inv = (uint32_t)0 - inv;

  // 2^(64s) mod n by doubling, for converting into Montgomery form.
  memset(r2, 0, sizeof(r2));
  r2[0] = 1;
  for (i = 0; i < 64 * s; i++) {
    uint32_t carry = 0;
    size_t j;
    for (j = 0; j < s; j++) {
      uint32_t next = r2[j] >> 31;
      r2[j] = (r2[j] << 1) | carry;
      carry = next;
    }
    if (geq(r2, carry, nn, s)) {
      sub(r2, nn, s);
    }
  }

  from_be(b, s, base, n_len);
  memset(one, 0, sizeof(one));
  one[0] = 1;
  mont_mul(b, b, r2, nn, inv, s);
  mont_mul(x, r2, one, nn, inv, s);  // 1 in Montgomery form
  for (i = 0; i < exp_len; i++) {
    for (k = 7; k >= 0; k--) {
      mont_mul(x, x, x, nn, inv, s);
      if ((exp[i] >> k) & 1) {
        mont_mul(x, x, b, nn, inv, s);
      }
    }
  }
  mont_mul(x, x, one, nn, inv, s);
  to_be(out, n_len, x);
  return RIP_OK;
}

static const uint8_t SHA256_INFO[19] = {0x30, 0x31, 0x30, 0x0d, 0x06, 0x09, 0x60, 0x86, 0x48, 0x01,
                                        0x65, 0x03, 0x04, 0x02, 0x01, 0x05, 0x00, 0x04, 0x20};

static int encode_sha256(uint8_t *em, size_t n_len, const uint8_t hash[32]) {
  size_t t = sizeof(SHA256_INFO) + 32;

  if (n_len < t + 11 || n_len > RIP_RSA_MAX_BYTES) {
    return RIP_ERR;
  }
  em[0] = 0;
  em[1] = 1;
  memset(em + 2, 0xFF, n_len - t - 3);
  em[n_len - t - 1] = 0;
  memcpy(em + n_len - t, SHA256_INFO, sizeof(SHA256_INFO));
  memcpy(em + n_len - 32, hash, 32);
  return RIP_OK;
}

int rip_rsa_sign_sha256(const uint8_t *n, const uint8_t *d, size_t n_len, const uint8_t hash[32], uint8_t *sig) {
  uint8_t em[RIP_RSA_MAX_BYTES];

  if (encode_sha256(em, n_len, hash) != RIP_OK) {
    return RIP_ERR;
  }
  return rip_rsa_modexp(em, d, n_len, n, n_len, sig);
}

int rip_rsa_verify_sha256(const uint8_t *n, size_t n_len, const uint8_t hash[32], const uint8_t *sig) {
  static const uint8_t e[3] = {0x01, 0x00, 0x01};
  uint8_t em[RIP_RSA_MAX_BYTES];
  uint8_t got[RIP_RSA_MAX_BYTES];

  if (encode_sha256(em, n_len, hash) != RIP_OK || rip_rsa_modexp(sig, e, sizeof(e), n, n_len, got) != RIP_OK) {
    return RIP_ERR;
  }
  return memcmp(em, got, n_len) == 0 ? RIP_OK : RIP_ERR;
}
