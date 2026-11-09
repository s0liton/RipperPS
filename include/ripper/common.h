#ifndef RIPPER_COMMON_H_
#define RIPPER_COMMON_H_

#include <stddef.h>
#include <stdint.h>

// Return codes shared by every module. Success is 0, failures are negative.
enum {
  RIP_OK = 0,
  RIP_ERR = -1,      // generic failure (I/O, bad input)
  RIP_ESENSE = -2,   // the drive answered CHECK CONDITION; see the sense data
  RIP_ENOTSUP = -3,  // the drive, disc, or platform cannot do this
  RIP_ENOENT = -4,   // a file, key, or entry was not found
  RIP_ECANCEL = -5,  // the caller asked to stop
  RIP_ENOMEM = -6
};

static inline uint16_t rip_be16(const uint8_t *p) {
  return (uint16_t)((p[0] << 8) | p[1]);
}

static inline uint32_t rip_be32(const uint8_t *p) {
  return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
}

static inline uint16_t rip_le16(const uint8_t *p) {
  return (uint16_t)(p[0] | (p[1] << 8));
}

static inline uint32_t rip_le32(const uint8_t *p) {
  return p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static inline void rip_put_be16(uint8_t *p, uint16_t v) {
  p[0] = (uint8_t)(v >> 8);
  p[1] = (uint8_t)v;
}

static inline void rip_put_be32(uint8_t *p, uint32_t v) {
  p[0] = (uint8_t)(v >> 24);
  p[1] = (uint8_t)(v >> 16);
  p[2] = (uint8_t)(v >> 8);
  p[3] = (uint8_t)v;
}

static inline void rip_put_le16(uint8_t *p, uint16_t v) {
  p[0] = (uint8_t)v;
  p[1] = (uint8_t)(v >> 8);
}

static inline void rip_put_le32(uint8_t *p, uint32_t v) {
  p[0] = (uint8_t)v;
  p[1] = (uint8_t)(v >> 8);
  p[2] = (uint8_t)(v >> 16);
  p[3] = (uint8_t)(v >> 24);
}

#define RIP_ARRAY_LEN(a) (sizeof(a) / sizeof((a)[0]))

#endif
