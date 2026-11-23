#ifndef RIPPER_AES_H_
#define RIPPER_AES_H_

#include <stddef.h>
#include <stdint.h>

// Portable table-driven AES-128: CBC for PS3 discs, XTS for PS4 packages.
// TODO: add an AES-NI path; the PS5's Zen 2 cores support it.

struct rip_aes128 {
  uint32_t rk[44];
};

void rip_aes128_init_enc(struct rip_aes128 *ctx, const uint8_t key[16]);
void rip_aes128_init_dec(struct rip_aes128 *ctx, const uint8_t key[16]);

void rip_aes128_encrypt_block(const struct rip_aes128 *ctx, const uint8_t in[16], uint8_t out[16]);
void rip_aes128_decrypt_block(const struct rip_aes128 *ctx, const uint8_t in[16], uint8_t out[16]);

// In place. len must be a multiple of 16. iv is updated so calls can be chained.
void rip_aes128_cbc_encrypt(const struct rip_aes128 *ctx, uint8_t iv[16], uint8_t *buf, size_t len);
void rip_aes128_cbc_decrypt(const struct rip_aes128 *ctx, uint8_t iv[16], uint8_t *buf, size_t len);

// XTS-AES-128 (IEEE P1619) on one data unit, in place. data encrypts (or, for
// decrypt, decrypts) the blocks; tweak is always an encryption key and takes the
// unit number as a little-endian 128-bit value. len must be a multiple of 16.
void rip_aes128_xts_encrypt(const struct rip_aes128 *data, const struct rip_aes128 *tweak, uint64_t unit, uint8_t *buf,
                            size_t len);
void rip_aes128_xts_decrypt(const struct rip_aes128 *data, const struct rip_aes128 *tweak, uint64_t unit, uint8_t *buf,
                            size_t len);

#endif
