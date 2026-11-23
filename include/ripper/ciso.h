#ifndef RIPPER_CISO_H_
#define RIPPER_CISO_H_

#include <stdint.h>

#include "ripper/iso9660.h"

// CISO for GameCube discs, as Nintendont and Dolphin read it: a 32 KiB
// header ("CISO", the block size, one byte per block saying it's stored),
// then the stored 2 MiB blocks in order. Blocks no file uses are left out.

#define CISO_BLOCK (2u << 20)
#define CISO_MAP 1024u  // blocks the header can list: 2 GiB, more than a GameCube disc

// Which of the image's blocks hold something: the disc header and
// apploader, the main executable, the file table and every file. RIP_ERR
// when the image isn't a GameCube disc or its tables don't hold up.
int ciso_used_blocks(int fd, uint64_t size, uint8_t map[CISO_MAP]);

// Writes the CISO of the GameCube image at iso_path to out_path. progress
// gets bytes written of the total; a nonzero return stops it (RIP_ECANCEL,
// out_path removed).
int ciso_from_iso(const char *iso_path, const char *out_path, iso_progress_fn progress, void *ud);

#endif
