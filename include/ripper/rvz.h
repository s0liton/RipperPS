#ifndef RIPPER_RVZ_H_
#define RIPPER_RVZ_H_

#include "ripper/iso9660.h"

// RVZ writer: Dolphin's compressed GameCube and Wii image (docs/WiaAndRvz.md
// in dolphin-emu/dolphin), laid out as Dolphin's own converter writes it.
//
// The image goes in 128 KiB zstd chunks. The pseudorandom padding between
// files is stored as its 68-byte seed instead of the bytes, which is where
// most of the space goes on a GameCube disc.
//
// Wii partitions are encrypted and are stored as they are, as raw data with
// no partition entries. Dolphin reads that and decrypts the partitions itself,
// but the encrypted data barely compresses.

// Reads the image at iso_path and writes out_path. zstd_level is the zstd
// level (Dolphin's default is 5). Returns RIP_ENOTSUP when the image is not a
// GameCube or Wii disc. On failure or cancel the output file is removed.
int rvz_from_iso(const char *iso_path, const char *out_path, int zstd_level, iso_progress_fn progress, void *ud);

#endif
