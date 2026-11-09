#ifndef RIPPER_CUE_H_
#define RIPPER_CUE_H_

#include <stddef.h>
#include <stdint.h>

#include "ripper/cdrom.h"

// CUE sheets: written next to BIN files, and read back by the image drive
// (image_drive.h). Only raw 2352-byte tracks, the kind every dumping tool
// writes, are read.

struct cue_track {
  uint8_t number;
  enum cd_track_mode mode;
  uint8_t control;
  uint8_t session;
  unsigned file;          // index into cue_sheet.files
  uint64_t file_offset;   // byte offset of the track's first frame in its file (INDEX 00, or 01 without one)
  uint32_t pregap;        // INDEX 00 to INDEX 01, frames that are in the file
  uint32_t silence;       // PREGAP: frames before the track that are not in any file
  uint32_t start;         // absolute LBA of INDEX 01
  uint32_t length;        // frames from INDEX 01 to the end of the track
};

struct cue_sheet {
  unsigned count;
  struct cue_track tracks[CD_MAX_TRACKS];
  unsigned file_count;
  char files[CD_MAX_TRACKS][512];  // paths, relative ones resolved against the sheet's folder
  uint32_t leadout;
  char catalog[14];               // UPC/EAN, "" if none
};

// Parses a sheet; dir is where relative FILE names live. File sizes are read
// from disk to place the tracks. RIP_OK or RIP_ERR with a log line.
int cue_parse(const char *text, size_t len, const char *dir, struct cue_sheet *out);
int cue_load(const char *path, struct cue_sheet *out);

// A Redump-style sheet: one FILE per track (names[i] for track i) when
// per_track is set, else one FILE, names[0], with every track in it.
// Returns the bytes written to out (NUL-terminated), or 0 if it did not fit.
size_t cue_write(const struct cd_toc *toc, const char *const *names, int per_track, char *out, size_t len);

const char *cue_mode_name(enum cd_track_mode mode);

#endif
