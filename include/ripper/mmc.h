#ifndef RIPPER_MMC_H_
#define RIPPER_MMC_H_

#include <stddef.h>
#include <stdint.h>

#include "ripper/transport.h"

// Standard MMC profiles (GET CONFIGURATION "current profile").
#define MMC_PROFILE_NONE 0x0000
#define MMC_PROFILE_CD_ROM 0x0008
#define MMC_PROFILE_CD_R 0x0009
#define MMC_PROFILE_CD_RW 0x000A
#define MMC_PROFILE_DVD_ROM 0x0010
#define MMC_PROFILE_BD_ROM 0x0040
#define MMC_PROFILE_BD_R_SRM 0x0041
#define MMC_PROFILE_BD_R_RRM 0x0042
#define MMC_PROFILE_BD_RE 0x0043

// Vendor profiles reported by the PlayStation's own drive for PlayStation media.
// A standard PC drive reports the plain CD/DVD/BD profile for the same disc.
#define MMC_PROFILE_PS1_CD 0xFF50
#define MMC_PROFILE_PS2_CD 0xFF60
#define MMC_PROFILE_PS2_DVD 0xFF61
#define MMC_PROFILE_PS3_DVD 0xFF70
#define MMC_PROFILE_PS3_BD 0xFF71
#define MMC_PROFILE_PS4_BD 0xFF80
#define MMC_PROFILE_PS5_BD 0xFF90

// Features (GET CONFIGURATION feature codes).
#define MMC_FEATURE_PROFILE_LIST 0x0000
#define MMC_FEATURE_CD_READ 0x001E
#define MMC_FEATURE_DVD_READ 0x001F
#define MMC_FEATURE_BD_READ 0x0040

#define MMC_TIMEOUT_MS 10000u
#define MMC_READ_TIMEOUT_MS 30000u

struct mmc_inquiry {
  uint8_t peripheral_type;  // 0x05 for an optical drive
  char vendor[9];
  char product[17];
  char revision[5];
};

struct mmc_feature {
  uint16_t code;
  uint8_t current;
  uint8_t version;
  uint8_t data[32];  // first bytes of the feature-dependent data
  uint8_t data_len;
};

int mmc_inquiry(struct rip_transport *t, struct mmc_inquiry *out);
int mmc_test_unit_ready(struct rip_transport *t, struct rip_sense *sense);

int mmc_current_profile(struct rip_transport *t, uint16_t *profile, struct rip_sense *sense);
// Every profile the drive supports (from the profile list feature).
int mmc_supported_profiles(struct rip_transport *t, uint16_t *profiles, size_t max, size_t *count);
// RIP_OK with *out filled, RIP_ENOENT when the drive does not report the feature.
int mmc_get_feature(struct rip_transport *t, uint16_t code, struct mmc_feature *out);

// Last addressable LBA and the logical block length.
int mmc_read_capacity(struct rip_transport *t, uint32_t *last_lba, uint32_t *block_len);
int mmc_read12(struct rip_transport *t, uint32_t lba, uint32_t count, void *buf, size_t block_len,
               struct rip_sense *sense);

int mmc_read_disc_structure(struct rip_transport *t, uint8_t media_type, uint32_t address, uint8_t layer,
                            uint8_t format, void *buf, uint16_t len, struct rip_sense *sense);
// DVD copyright information (format 0x01). cps_type 0 = none, 1 = CSS/CPPM.
int mmc_dvd_copyright(struct rip_transport *t, uint8_t *cps_type, uint8_t *region_mask);

// Asks for the fastest read: SET STREAMING, falling back to SET CD SPEED.
int mmc_set_max_speed(struct rip_transport *t, uint32_t end_lba);
// SET CD SPEED to a read speed in kB/s (176 is 1x); scratched discs often
// read at a low speed when they don't at full.
int mmc_set_cd_speed(struct rip_transport *t, uint16_t kbps);

const char *mmc_profile_name(uint16_t profile);
int mmc_profile_is_cd(uint16_t profile);
int mmc_profile_is_dvd(uint16_t profile);
int mmc_profile_is_bd(uint16_t profile);

#endif
