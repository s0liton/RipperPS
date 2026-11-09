#ifndef RIPPER_REMUX_H_
#define RIPPER_REMUX_H_

#include <stdint.h>

#include "ripper/dvd.h"
#include "ripper/reader.h"

// A DVD title to a file people can play anywhere, through FFmpeg (the
// console's SDK has it; builds without it say so):
//
//   MKV  the title as it is on the disc: MPEG-2 video, every audio track
//        and subtitle with its language, the chapters. Fast, lossless.
//   MP4  H.264 and AAC, for phones, TVs and browsers: re-encoded on the CPU.
//
// The title's cells are read in order through r: the CSS reader on a disc,
// a plain reader on a decrypted ISO.

enum remux_format { REMUX_MKV, REMUX_MP4 };

typedef int (*remux_progress_fn)(void *ud, uint64_t done, uint64_t total);  // nonzero stops

struct remux_opts {
  enum remux_format format;
  int crf;               // MP4: x264 quality, 18 (better) to 28 (smaller); 0 picks 20
  const char *preset;    // MP4: x264 preset, NULL picks "veryfast"
  remux_progress_fn progress;
  void *ud;
};

// 1 when this build can write format.
int remux_available(enum remux_format format);

// RIP_OK, RIP_ECANCEL, RIP_ENOTSUP (no FFmpeg here), or RIP_ERR with a log line.
int remux_title(struct rip_reader *r, const struct dvd_title *t, const char *out_path, const struct remux_opts *opts);

#endif
