#ifndef RIPPER_SFO_H_
#define RIPPER_SFO_H_

#include <stddef.h>
#include <stdint.h>

// PARAM.SFO (PS3, PS4): a little-endian key/value table starting "\0PSF".

int sfo_valid(const uint8_t *sfo, size_t len);
// Copies a UTF-8 string value. RIP_ENOENT when the key is missing or not a string.
int sfo_get_string(const uint8_t *sfo, size_t len, const char *key, char *out, size_t out_len);
// Replaces a string value in place, when it fits the room the file gives it.
// RIP_ENOENT when the key is missing, RIP_ERR when it isn't a string or doesn't fit.
int sfo_set_string(uint8_t *sfo, size_t len, const char *key, const char *value);

#endif
