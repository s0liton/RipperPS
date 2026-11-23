#include "ripper/sfo.h"

#include <string.h>

#include "ripper/common.h"

#define SFO_HEADER 0x14
#define SFO_ENTRY 0x10
#define SFO_FMT_UTF8_SPECIAL 0x0004
#define SFO_FMT_UTF8 0x0204

int sfo_valid(const uint8_t *sfo, size_t len) {
  uint32_t key_table;
  uint32_t data_table;
  uint32_t entries;

  if (len < SFO_HEADER || memcmp(sfo, "\0PSF", 4) != 0) {
    return 0;
  }
  key_table = rip_le32(sfo + 0x08);
  data_table = rip_le32(sfo + 0x0C);
  entries = rip_le32(sfo + 0x10);
  return entries < 1024 && SFO_HEADER + (size_t)entries * SFO_ENTRY <= len && key_table <= len && data_table <= len;
}

int sfo_get_string(const uint8_t *sfo, size_t len, const char *key, char *out, size_t out_len) {
  uint32_t key_table;
  uint32_t data_table;
  uint32_t entries;
  uint32_t i;

  if (out_len == 0) {
    return RIP_ERR;
  }
  out[0] = '\0';
  if (!sfo_valid(sfo, len)) {
    return RIP_ERR;
  }
  key_table = rip_le32(sfo + 0x08);
  data_table = rip_le32(sfo + 0x0C);
  entries = rip_le32(sfo + 0x10);

  for (i = 0; i < entries; i++) {
    const uint8_t *e = sfo + SFO_HEADER + (size_t)i * SFO_ENTRY;
    size_t key_off = (size_t)key_table + rip_le16(e);
    uint16_t fmt = rip_le16(e + 2);
    uint32_t data_len = rip_le32(e + 4);
    size_t data_off = (size_t)data_table + rip_le32(e + 12);
    size_t key_len = strlen(key);
    size_t n;

    if (key_off + key_len + 1 > len || memcmp(sfo + key_off, key, key_len + 1) != 0) {
      continue;
    }
    if ((fmt != SFO_FMT_UTF8 && fmt != SFO_FMT_UTF8_SPECIAL) || data_off + data_len > len) {
      return RIP_ENOENT;
    }
    n = strnlen((const char *)sfo + data_off, data_len);
    if (n >= out_len) {
      n = out_len - 1;
    }
    memcpy(out, sfo + data_off, n);
    out[n] = '\0';
    return RIP_OK;
  }
  return RIP_ENOENT;
}

int sfo_set_string(uint8_t *sfo, size_t len, const char *key, const char *value) {
  uint32_t key_table;
  uint32_t data_table;
  uint32_t entries;
  uint32_t i;
  size_t vlen = strlen(value);

  if (!sfo_valid(sfo, len)) {
    return RIP_ERR;
  }
  key_table = rip_le32(sfo + 0x08);
  data_table = rip_le32(sfo + 0x0C);
  entries = rip_le32(sfo + 0x10);
  for (i = 0; i < entries; i++) {
    uint8_t *e = sfo + SFO_HEADER + (size_t)i * SFO_ENTRY;
    size_t key_off = (size_t)key_table + rip_le16(e);
    uint16_t fmt = rip_le16(e + 2);
    uint32_t max_len = rip_le32(e + 8);
    size_t data_off = (size_t)data_table + rip_le32(e + 12);
    size_t key_len = strlen(key);

    if (key_off + key_len + 1 > len || memcmp(sfo + key_off, key, key_len + 1) != 0) {
      continue;
    }
    if ((fmt != SFO_FMT_UTF8 && fmt != SFO_FMT_UTF8_SPECIAL) || data_off + max_len > len || vlen + 1 > max_len) {
      return RIP_ERR;
    }
    memset(sfo + data_off, 0, max_len);
    memcpy(sfo + data_off, value, vlen);
    // The used length: a UTF-8 string's counts its terminator.
    e[4] = (uint8_t)(vlen + 1);
    e[5] = (uint8_t)((vlen + 1) >> 8);
    e[6] = 0;
    e[7] = 0;
    return RIP_OK;
  }
  return RIP_ENOENT;
}
