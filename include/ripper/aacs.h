#ifndef RIPPER_AACS_H_
#define RIPPER_AACS_H_

#include <stddef.h>
#include <stdint.h>

#include "ripper/reader.h"

// AACS, the encryption of Blu-ray movies, undone with keys from a KEYDB.cfg
// (the file libaacs reads; RipperPS ships none). A disc is known by its ID,
// the SHA-1 of AACS/Unit_Key_RO.inf. Its KEYDB entry gives the volume unique
// key (VUK), which opens the unit keys stored in that file, or the unit keys
// themselves. The movie's streams (BDMV/STREAM/*.m2ts) are encrypted in
// 6144-byte aligned units, each from the start of its file: the first 16
// bytes are clear, and with the unit key they make the key for the rest.

#define AACS_UNIT 6144u
#define AACS_MAX_UNIT_KEYS 16

struct aacs_keys {
  int have_vuk;
  uint8_t vuk[16];
  unsigned uk_count;  // unit keys given by the entry itself
  uint8_t uk[AACS_MAX_UNIT_KEYS][16];
  char title[128];  // the entry's name for the disc
};

// The entry for disc_id in the KEYDB.cfg at path. RIP_OK, RIP_ENOENT when
// the file has none for this disc, RIP_ERR when the file can't be read.
int aacs_keydb_find(const char *path, const uint8_t disc_id[20], struct aacs_keys *out);

// How many discs the KEYDB.cfg at path has keys for; -1 when it can't be read.
long aacs_keydb_count(const char *path);

// The unit keys in Unit_Key_RO.inf, opened with the VUK. How many, or -1
// when the file is too short for what it says it holds.
int aacs_unit_keys(const uint8_t *inf, size_t len, const uint8_t vuk[16], uint8_t uk[][16], unsigned max);

// Decrypts one aligned unit in place. 1 when it was encrypted and came out
// as transport stream packets, 0 when it wasn't encrypted (left as it is),
// -1 when it didn't decrypt with this key (left as it is).
int aacs_decrypt_unit(const uint8_t uk[16], uint8_t unit[AACS_UNIT]);

// Its inverse, for tests: encrypts a unit of transport stream packets whose
// copy permission bits are set.
void aacs_encrypt_unit(const uint8_t uk[16], uint8_t unit[AACS_UNIT]);

// A reader over base (a Blu-ray movie disc) that gives back the streams
// decrypted. RIP_OK; RIP_ENOTSUP when the disc has no AACS; RIP_ENOENT when
// keydb_path has no key for it (why says so, and the disc ID). base stays
// the caller's and must outlive the result.
int rip_reader_open_aacs(struct rip_reader *out, struct rip_reader *base, const char *keydb_path, char *why, size_t wn);

#endif
