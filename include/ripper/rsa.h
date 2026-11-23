#ifndef RIPPER_RSA_H_
#define RIPPER_RSA_H_

#include <stddef.h>
#include <stdint.h>

// Textbook RSA over big-endian byte strings, for the few operations a PS4
// package needs. Not constant time: it only ever handles keys that are public
// or already leaked.

#define RIP_RSA_MAX_BYTES 512  // 4096-bit moduli

// out = base^exp mod n. n is odd and n_len bytes; base and out are n_len
// bytes too (base may be any value below 2^(8*n_len)). RIP_OK or RIP_ERR.
int rip_rsa_modexp(const uint8_t *base, const uint8_t *exp, size_t exp_len, const uint8_t *n, size_t n_len,
                   uint8_t *out);

// RSASSA-PKCS1-v1_5 with SHA-256: sig = EM^d mod n, n_len bytes.
int rip_rsa_sign_sha256(const uint8_t *n, const uint8_t *d, size_t n_len, const uint8_t hash[32], uint8_t *sig);
// RIP_OK when sig^65537 mod n is the PKCS#1 v1.5 encoding of hash.
int rip_rsa_verify_sha256(const uint8_t *n, size_t n_len, const uint8_t hash[32], const uint8_t *sig);

#endif
