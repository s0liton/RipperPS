#ifndef RIPPER_IMAGE_DRIVE_H_
#define RIPPER_IMAGE_DRIVE_H_

#include <stdint.h>

#include "ripper/transport.h"

// A virtual drive that answers MMC commands from a disc image, so everything
// above the transport (probe, identify, every rip format, the service and its
// UI) runs without a drive: on a development machine, in tests, and as the
// "image" drive of the host build.
//
//   .iso   a DVD or BD (2048-byte sectors)
//   .cue   a CD of raw 2352-byte tracks, with a .sub beside a single .bin used
//          for its subchannel when present (else Q is made from the sheet)
//
// It starts empty or with an image; load and eject change the disc as a tray would.

struct rip_image_drive_opts {
  // Profile a DVD/BD image reports: 0 picks DVD-ROM or BD-ROM by size, as a PC
  // drive would. A PlayStation vendor profile (0xFF61, 0xFF71) acts like the
  // PS5's own drive.
  uint16_t profile;
  int cd;   // the drive has a CD laser (default on)
  int sony; // lists PlayStation vendor profiles, like the PS5's drive
};

int rip_image_drive_open(struct rip_transport *t, const struct rip_image_drive_opts *opts);
// Inserts an image (replacing the one in it). RIP_OK, or RIP_ERR if unreadable.
int rip_image_drive_load(struct rip_transport *t, const char *path);
void rip_image_drive_eject(struct rip_transport *t);
// The image in the drive, "" when empty.
const char *rip_image_drive_path(struct rip_transport *t);

#endif
