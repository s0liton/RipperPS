#ifndef RIPPER_ZIP_H_
#define RIPPER_ZIP_H_

#include <stdint.h>

// A folder's contents into a .zip, stored (disc images don't compress much,
// and emulators read stored zips fastest), ZIP64 so nothing is too big. For
// RomM, which takes one file per game: a BIN/CUE folder goes up as a zip.

typedef int (*zip_progress_fn)(void *ud, uint64_t done, uint64_t total);  // nonzero stops

// RIP_OK, RIP_ECANCEL, or RIP_ERR with a log line.
int zip_build_dir(const char *src_dir, const char *zip_path, zip_progress_fn progress, void *ud);

#endif
