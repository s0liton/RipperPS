#ifndef RIPPER_WBFS_H_
#define RIPPER_WBFS_H_

#include <stdint.h>

#include "ripper/iso9660.h"

// Writes a Wii disc image (.iso) as a one-disc WBFS file, laid out the way
// wit and USB Loader GX write one: 512-byte hd sectors, 2 MiB WBFS sectors,
// the disc header copy and block table in the first block, then the used
// blocks in disc order. A block is used when it holds the disc header, the
// partition tables, the region info or any part of a partition up to the end
// of its data. Partitions stay encrypted and whole, so nothing needs a key.
//
// With split_size > 0 the file is cut into parts of that size, rounded down to
// 32 KiB: "X.wbfs", "X.wbf1", "X.wbf2"... at most 10 of them, the way USB
// Loader GX reads them back. out_path must then end in ".wbfs".

// One cluster short of 4 GiB, what USB Loader GX and wit use on FAT32.
#define WBFS_SPLIT_FAT32 (4ull * 1024 * 1024 * 1024 - 32 * 1024)

// RIP_OK, RIP_ECANCEL, RIP_ENOTSUP when the image isn't a Wii disc, or RIP_ERR
// with a log line. Partial output is removed on failure.
int wbfs_from_iso(const char *iso_path, const char *out_path, uint64_t split_size, iso_progress_fn progress, void *ud);

#endif
