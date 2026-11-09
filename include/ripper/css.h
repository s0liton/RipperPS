#ifndef RIPPER_CSS_H_
#define RIPPER_CSS_H_

#include "ripper/reader.h"
#include "ripper/transport.h"

// DVD-Video's CSS, through libdvdcss (third_party/libdvdcss, GPL-2.0-or-later).
// The drive is authenticated over the transport, then a reader is handed out
// that gives the disc's sectors with the video descrambled: an ISO made from
// it plays anywhere.
//
// A drive refuses scrambled sectors until it has been authenticated, so the
// base reader must read from the same drive as t.

// Wraps base (which stays the caller's, and must outlive the result). The
// title keys are kept in cache_dir when it isn't NULL, so the next rip of the
// same disc needn't ask the drive again.
int rip_reader_open_css(struct rip_reader *out, struct rip_reader *base, struct rip_transport *t,
                        const char *cache_dir);

#endif
