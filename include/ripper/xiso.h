#ifndef RIPPER_XISO_H_
#define RIPPER_XISO_H_

#include <stdint.h>

#include "ripper/iso9660.h"

// XISO: an Xbox disc's game partition alone (XDVDFS), what xemu and modded
// consoles load. Taken from a full image (Redump's layout: the video
// partition, then the game partition at XGD1's, XGD2's or XGD3's offset) or
// from an image that's already the partition, and cut after the last byte
// its files use.

// Where the game partition starts in the image, in bytes. RIP_ENOENT when
// there's no XDVDFS at any of the known places.
int xiso_partition(int fd, uint64_t *offset);

// The partition's used length from its start: the end of its last file or
// directory table, whole sectors. RIP_ERR when the tables don't hold up.
int xiso_used(int fd, uint64_t offset, uint64_t *length);

// Writes the XISO of the image at iso_path to out_path. progress gets bytes
// written of the total; a nonzero return stops it (RIP_ECANCEL, out_path
// removed).
int xiso_from_iso(const char *iso_path, const char *out_path, iso_progress_fn progress, void *ud);

#endif
