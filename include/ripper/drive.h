#ifndef RIPPER_DRIVE_H_
#define RIPPER_DRIVE_H_

#include <stdint.h>

#include "ripper/mmc.h"
#include "ripper/transport.h"

// Where the drive lives. The PS5's built-in drive and a USB drive go through
// the same command path; what differs is what each one can read.
enum rip_drive_source { RIP_DRIVE_INTERNAL, RIP_DRIVE_EXTERNAL };

// What a drive can do. Recipes (rip.h) list the capabilities they need, so a
// better drive unlocks more disc types without changes elsewhere.
enum rip_cap {
  RIP_CAP_READ_CD = 1u << 0,  // CD laser present (CD Read feature)
  RIP_CAP_READ_DVD = 1u << 1,
  RIP_CAP_READ_BD = 1u << 2,
  RIP_CAP_CD_C2 = 1u << 3,          // C2 error pointers in READ CD
  RIP_CAP_SONY_PROFILES = 1u << 4,  // reports PlayStation vendor profiles (0xFFxx)
  // Vendor features and custom firmware, from INQUIRY.
  RIP_CAP_PLEXTOR = 1u << 8,      // D8 reads, lead-in/out
  RIP_CAP_MTK_CACHE = 1u << 9,    // MediaTek cache reads
  RIP_CAP_KREON = 1u << 10,       // Xbox / Xbox 360 unlock
  RIP_CAP_OMNIDRIVE = 1u << 11,   // raw DVD/BD, GameCube, Wii, Wii U, Xbox
  RIP_CAP_LIBREDRIVE = 1u << 12,  // UHD and unrestricted BD reads
  RIP_CAP_XBOX = 1u << 13         // Kreon or OmniDrive: past an Xbox disc's video partition
};

enum rip_media_state {
  RIP_MEDIA_EMPTY,      // no disc (sense 02/3A)
  RIP_MEDIA_NOT_READY,  // spinning up or busy; poll again
  RIP_MEDIA_PRESENT,
  RIP_MEDIA_INCOMPATIBLE,  // the drive cannot read this medium (sense 02/30)
  RIP_MEDIA_ERROR
};

struct rip_drive {
  struct rip_transport *t;
  enum rip_drive_source source;
  struct mmc_inquiry inquiry;
  uint32_t caps;
  // From the last rip_drive_media: why the drive isn't ready, and the profile it gave.
  struct rip_sense last_sense;
  uint16_t last_profile;
};

int rip_drive_probe(struct rip_drive *d, struct rip_transport *t, enum rip_drive_source source);
// The current profile is valid only when the state is RIP_MEDIA_PRESENT.
enum rip_media_state rip_drive_media(struct rip_drive *d, uint16_t *profile);

// Comma-separated names, e.g. "dvd,bd,sony-profiles".
void rip_caps_str(uint32_t caps, char *buf, size_t len);
const char *rip_drive_source_name(enum rip_drive_source source);

#endif
