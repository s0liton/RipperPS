#ifndef RIPPER_PKG_H_
#define RIPPER_PKG_H_

#include <stddef.h>
#include <stdint.h>

// PS4 PKG files (app.pkg, patch.pkg): the entry table up front, read without
// any keys. The game itself sits in an encrypted PFS image that only the
// console can open; what is readable here are the clear entries: sce_sys
// (param.sfo, icons, trophies...), named in the package's own name table.

#define PKG_MAGIC 0x7F434E54u
#define PKG_ENTRY_PARAM_SFO 0x1000u
#define PKG_ENTRY_NAMES 0x0200u

struct pkg_entry {
  uint32_t id;
  uint32_t name_offset;  // into the name table, 0 for unnamed entries
  uint32_t flags1;       // bit 31: encrypted
  uint32_t offset;
  uint32_t size;
  char name[64];         // "param.sfo", "trophy/trophy00.trp"; "" when unnamed
};

struct pkg {
  int fd;
  uint32_t type;
  uint64_t body_offset;
  uint64_t body_size;
  unsigned count;
  struct pkg_entry *entries;
};

static inline int pkg_entry_encrypted(const struct pkg_entry *e) {
  return (e->flags1 & 0x80000000u) != 0;
}

// Reads the header and entry table from fd (which stays the caller's).
int pkg_open(struct pkg *p, int fd);
void pkg_close(struct pkg *p);
const struct pkg_entry *pkg_find(const struct pkg *p, uint32_t id);
// Reads a clear entry whole; the caller frees *out.
int pkg_read_entry(const struct pkg *p, const struct pkg_entry *e, uint8_t **out, size_t *len);
// Writes every clear, named entry under dir ("sce_sys/<name>" is the usual
// target). Returns how many were written, or a negative error.
int pkg_extract_named(const struct pkg *p, const char *dir);

#endif
