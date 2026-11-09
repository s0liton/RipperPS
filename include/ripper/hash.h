#ifndef RIPPER_HASH_H_
#define RIPPER_HASH_H_

#include <stddef.h>
#include <stdint.h>

// Redump verifies images by CRC-32, MD5 and SHA-1, so all three run during a dump.

struct rip_md5 {
  uint32_t h[4];
  uint64_t len;
  uint8_t buf[64];
  size_t used;
};

struct rip_sha1 {
  uint32_t h[5];
  uint64_t len;
  uint8_t buf[64];
  size_t used;
};

// zlib-compatible: start with crc = 0 and feed the previous result back in.
uint32_t rip_crc32(uint32_t crc, const void *data, size_t len);

void rip_md5_init(struct rip_md5 *ctx);
void rip_md5_update(struct rip_md5 *ctx, const void *data, size_t len);
void rip_md5_final(struct rip_md5 *ctx, uint8_t out[16]);

void rip_sha1_init(struct rip_sha1 *ctx);
void rip_sha1_update(struct rip_sha1 *ctx, const void *data, size_t len);
void rip_sha1_final(struct rip_sha1 *ctx, uint8_t out[20]);

struct rip_sha256 {
  uint32_t h[8];
  uint64_t len;
  uint8_t buf[64];
  size_t used;
};

void rip_sha256_init(struct rip_sha256 *ctx);
void rip_sha256_update(struct rip_sha256 *ctx, const void *data, size_t len);
void rip_sha256_final(struct rip_sha256 *ctx, uint8_t out[32]);

struct rip_hashes {
  uint64_t size;
  uint32_t crc32;
  uint8_t md5[16];
  uint8_t sha1[20];
};

struct rip_hash_ctx {
  uint64_t size;
  uint32_t crc;
  struct rip_md5 md5;
  struct rip_sha1 sha1;
};

void rip_hash_init(struct rip_hash_ctx *ctx);
void rip_hash_update(struct rip_hash_ctx *ctx, const void *data, size_t len);
void rip_hash_final(struct rip_hash_ctx *ctx, struct rip_hashes *out);

#endif
