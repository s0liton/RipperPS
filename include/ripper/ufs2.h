#ifndef RIPPER_UFS2_H_
#define RIPPER_UFS2_H_

#include <stdint.h>

// Writes a UFS2 image holding a folder's contents at its root: the ".ffpkg"
// game images ShadowMountPlus mounts on a PS5 (its recommended format), which
// FreeBSD and the PS5's kernel read natively. Laid out as newfs would with
// "-O 2 -b 65536 -f 65536 -m 0": 64 KiB blocks with no fragments, one inode
// per 256 KiB (more when a game has many small files), every file in order
// and read-only (0555, as the PS5's own game mounts are), the image sized to
// what it holds. Superblocks and cylinder groups after FreeBSD's fs.h and
// mkfs; nothing of makefs is used.

typedef int (*ufs2_progress_fn)(void *ud, uint64_t done, uint64_t total);  // nonzero stops

// RIP_OK, RIP_ECANCEL, or RIP_ERR with a log line. label may be NULL.
int ufs2_build(const char *src_dir, const char *image_path, const char *label, ufs2_progress_fn progress, void *ud);

#endif
