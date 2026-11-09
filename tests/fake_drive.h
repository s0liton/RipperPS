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
};

void fake_drive_transport(struct fake_drive *fd, struct rip_transport *t);

#endif
