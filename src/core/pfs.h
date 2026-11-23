#ifndef RIPPER_CORE_PFS_H_
#define RIPPER_CORE_PFS_H_

#include <stddef.h>
#include <stdint.h>

#include "ripper/iso9660.h"

// The two PFS images of a PS4 package, after LibOrbisPkg's PFS/PFSBuilder.cs,
// PFSCWriter.cs and FlatPathTable.cs: an unsigned inner image of the game
// folder, wrapped as pfs_image.dat (PFSC, blocks stored), inside an outer image
// that is signed (HMAC-SHA256 per 64 KiB block) and encrypted (XTS-AES-128 per
// 4 KiB sector). Used by fpkg.c only.

#define PFS_BLOCK 0x10000u

// Returned by fpkg_entry_id for sce_sys files the package makes itself.
#define FPKG_ENTRY_DROP 0xFFFFFFFFu

// The package entry id of a name below sce_sys ("icon0.png",
// "trophy/trophy00.trp"), 0 when it has none, or FPKG_ENTRY_DROP. In fpkg.c.
uint32_t fpkg_entry_id(const char *name);

// .NET's culture-aware string order for ASCII, which LibOrbisPkg sorts names,
// paths and param.sfo keys in: punctuation, digits, then letters without
// regard to case, then the shorter first, then lowercase first.
int pfs_name_cmp(const char *a, size_t al, const char *b, size_t bl);

// A file below sce_sys that becomes a package entry instead of a PFS file.
struct pfs_entry {
  uint32_t id;
  const char *name;  // below sce_sys
  const char *path;  // to open
  uint64_t size;
};

struct pfs_image;

// Reads the folder's tree. RIP_OK, or an error with the reason in err.
int pfs_scan(const char *folder, struct pfs_image **out, char *err, size_t en);
void pfs_free(struct pfs_image *p);

// The entry files in LibOrbisPkg's order (param.sfo among them).
size_t pfs_entry_count(const struct pfs_image *p);
const struct pfs_entry *pfs_entry_at(const struct pfs_image *p, size_t i);

int pfs_has_keystone(const struct pfs_image *p);
// Adds a generated sce_sys/keystone, as LibOrbisPkg does for a folder without one.
int pfs_add_keystone(struct pfs_image *p, const uint8_t data[96]);

// Lays both images out. time is the inode timestamp (seconds since 1970).
int pfs_plan(struct pfs_image *p, int64_t time, char *err, size_t en);
uint64_t pfs_inner_size(const struct pfs_image *p);
uint64_t pfs_outer_size(const struct pfs_image *p);

// Writes the outer image at offset in fd, which must already be that long.
// chunk_sha gets the first 4 bytes of each written 64 KiB block's SHA-256 (the
// PlayGo chunk hashes). Then reads the image back for its SHA-256 and that of
// its first block. progress sees bytes out of twice the image size.
int pfs_write(struct pfs_image *p, int fd, uint64_t offset, const uint8_t ekpfs[32], uint8_t *chunk_sha,
              uint8_t image_digest[32], uint8_t signed_digest[32], iso_progress_fn progress, void *ud, char *err,
              size_t en);

#endif
