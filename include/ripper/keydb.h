#ifndef RIPPER_KEYDB_H_
#define RIPPER_KEYDB_H_

#include <stddef.h>
#include <stdint.h>

// PS3 disc keys, matched by the SHA-1 of the encrypted image (the hash Redump
// publishes). The file is plain text, one disc per line:
//
//   <sha1 hex> <size in bytes> <disc key hex> <Redump name>
//
// Lines starting with '#' are comments. tools/build_ps3_keydb.py builds it from
// a Redump PS3 DAT and the matching key files. Keys are not shipped with the app.

struct rip_key_entry {
  uint8_t sha1[20];
  uint64_t size;
  uint8_t key[16];
  char name[240];
};

struct rip_keydb {
  struct rip_key_entry *entries;
  size_t count;
};

int rip_keydb_load(struct rip_keydb *db, const char *path);
const struct rip_key_entry *rip_keydb_find(const struct rip_keydb *db, const uint8_t sha1[20], uint64_t size);
void rip_keydb_free(struct rip_keydb *db);

// A key next to an image: "<stem>.key" (16 raw bytes) or "<stem>.dkey" (32 hex chars).
int rip_key_from_sidecar(const char *stem_path, uint8_t key[16]);

#endif
