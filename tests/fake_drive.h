#ifndef RIPPER_TEST_FAKE_DRIVE_H_
#define RIPPER_TEST_FAKE_DRIVE_H_

#include <stddef.h>
#include <stdint.h>

#include "ripper/transport.h"

// An in-memory MMC drive serving an image, enough for probe, identify and rip.
struct fake_drive {
  const uint8_t *image;  // NULL = empty tray
  size_t image_len;
  uint16_t profile;
  int cd;
  int dvd;
  int bd;
  int sony;          // also list a PlayStation vendor profile
  uint8_t cps_type;  // DVD copyright: 1 = CSS
  int fail_lba;      // >= 0: READ(12) covering this LBA fails once per attempt
  unsigned reads;
  // Custom firmware's marks in INQUIRY: "KREON V1.00" at byte 36, "OmniDrive"
  // at 74 with its version (1.0.4) at 83.
  int kreon;
  int omnidrive;
  // READ DISC STRUCTURE format 0: a DVD's physical format for each layer
  // (2048 bytes, without the header), and a Blu-ray's disc information.
  // NULL answers all zeros, as before.
  const uint8_t *pfi[2];
  const uint8_t *bd_di;
  size_t bd_di_len;
  // Kreon: the Xbox security sector its own READ DISC STRUCTURE hands out, and
  // the lock state its FF 08 01 11 command sets.
  const uint8_t *xbox_ss;
  int unlocked;
  // OmniDrive's raw read (C0) hands out the image's sectors as frames
  // scrambled the way the disc has them: 1 a Nintendo DVD, 2 a Wii U Blu-ray.
  int nintendo;
};

void fake_drive_transport(struct fake_drive *fd, struct rip_transport *t);

#endif
