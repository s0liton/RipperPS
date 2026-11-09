#ifndef RIPPER_DIAG_H_
#define RIPPER_DIAG_H_

#include <stddef.h>
#include <stdint.h>

#include "ripper/transport.h"

// A drive's self-report and how it copes with the disc in it, for drives
// ripper hasn't met: everything the drive says about itself, every way of
// reading the disc ripper may want (raw CD frames with C2 and subchannel,
// overreading the lead-in and lead-out, Plextor reads (on Plextors), the cache,
// DVD/BD structures, transfer sizes, reads past the end), timed. Only
// commands that read: nothing is written, no setting kept, no key material
// recorded.
//
// The results go into a folder: report.txt to read, raw/ with each answer
// as it came. The service zips it to send back.

typedef void (*diag_progress_fn)(void *ud, const char *step, int done, int total);

struct diag_summary {
  char drive[96];       // "HL-DT-ST BD-RE BP50NB40 1.00"
  char disc[64];        // "CD-ROM, 1 track" ...
  int commands;         // commands sent
  int answered;         // that the drive took
  char findings[2048];  // a line each: "C2 error pointers: yes"
};

// RIP_OK (even when the drive refuses much: that's the point), RIP_ECANCEL,
// or RIP_ERR when out_dir can't be written.
int diag_run(struct rip_transport *t, const char *out_dir, diag_progress_fn progress, void *ud,
             const volatile int *cancel, struct diag_summary *summary);

#endif
