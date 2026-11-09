#ifndef RIPPER_XBOX_H_
#define RIPPER_XBOX_H_

#include <stdint.h>

#include "ripper/drive.h"
#include "ripper/reader.h"

// Xbox and Xbox 360 discs (XGD 1 to 3) on a drive that can reach past the
// video partition: Kreon firmware (unlocked with its own command) or
// OmniDrive. The security sector says how long the game partition is and
// which ranges are the protection, left as zeros in the image as Redump does.

#define XBOX_MAX_RANGES 24

struct xbox_layout {
  uint8_t ss[2048];  // the security sector, as read
  unsigned version;  // 1 Xbox, 2 or 3 Xbox 360
  uint32_t sectors;  // the whole image
  // Kreon: past lock_lba the drive is locked again and reads the second
  // layer's video partition from l1_video_lba.
  int kreon;
  uint32_t lock_lba;
  uint32_t l1_video_lba;
  struct {
    uint32_t start;
    uint32_t end;  // exclusive
  } ranges[XBOX_MAX_RANGES];
  unsigned range_count;
};

// RIP_ENOTSUP when the drive has neither firmware, RIP_ERR when the disc
// isn't an XGD. Kreon drives are left unlocked; xbox_relock locks them again.
int xbox_read_layout(struct rip_drive *d, struct xbox_layout *out);
void xbox_relock(struct rip_drive *d, const struct xbox_layout *x);

// The image: protected ranges read as zeros, the rest through READ(12).
int rip_reader_open_xbox(struct rip_reader *r, struct rip_drive *d, const struct xbox_layout *x);

// Exposed for tests: the layout from a security sector and the video
// partition's physical format (READ DISC STRUCTURE format 0, after its header).
int xbox_layout_from(struct xbox_layout *x, const uint8_t pfi[17], uint32_t capacity, int kreon);

#endif
