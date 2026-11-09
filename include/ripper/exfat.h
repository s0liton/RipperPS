#ifndef RIPPER_EXFAT_H_
#define RIPPER_EXFAT_H_

#include <stdint.h>

// Writes an exFAT image holding a folder's contents at its root: the
// ".exfat" game images ShadowMountPlus mounts on a PS5, and a plain disk
// image anything else can read. 512-byte sectors and 64 KiB clusters (what
// ShadowMountPlus asks for), every file contiguous, FAT chains written in
// full, the size rounded up to 1 MiB. After Microsoft's exFAT specification.

typedef int (*exfat_progress_fn)(void *ud, uint64_t done, uint64_t total);  // nonzero stops

// RIP_OK, RIP_ECANCEL, or RIP_ERR with a log line. label may be NULL.
int exfat_build(const char *src_dir, const char *image_path, const char *label, exfat_progress_fn progress,
                void *ud);

#endif
