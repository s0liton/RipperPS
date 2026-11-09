#include "ripper/drive.h"

#include <stdio.h>
#include <string.h>

#include "ripper/common.h"
#include "ripper/log.h"

// Custom firmware that says so in INQUIRY (redumper's checks): Kreon at bytes
// 36-46, OmniDrive at 74-82 with its version in 83-85. A USB bridge that cuts
// INQUIRY to 36 bytes hides both. LibreDrive can't be seen this way.
static void probe_firmware(struct rip_drive *d) {
  uint8_t cdb[6] = {0x12, 0, 0, 0, 104, 0};
  uint8_t b[104];

  memset(b, 0, sizeof(b));
  if (rip_scsi(d->t, cdb, sizeof(cdb), RIP_DIR_IN, b, sizeof(b), MMC_TIMEOUT_MS, NULL) != RIP_OK) {
    return;
  }
  if (!memcmp(b + 36, "KREON V1.00", 11)) {
    d->caps |= RIP_CAP_KREON | RIP_CAP_XBOX;
    RIP_INFO("drive firmware: Kreon");
  }
  if (!memcmp(b + 74, "OmniDrive", 9)) {
    uint32_t version = ((uint32_t)b[83] << 16) | ((uint32_t)b[84] << 8) | b[85];
    RIP_INFO("drive firmware: OmniDrive %u.%u.%u", b[83], b[84], b[85]);
    // 1.0.3 left DVD frames scrambled after one read.
    if (version >= 0x010004) {
      d->caps |= RIP_CAP_OMNIDRIVE | RIP_CAP_XBOX;
    } else {
      RIP_WARN("OmniDrive %u.%u.%u is too old for DVD and Blu-ray reads; 1.0.4 or newer is", b[83], b[84], b[85]);
    }
  }
}

static void probe_vendor_caps(struct rip_drive *d) {
  if (strcmp(d->inquiry.vendor, "PLEXTOR") == 0) {
    d->caps |= RIP_CAP_PLEXTOR;
  }
  // The PS5's own drive lists CD reading (with C2) among its features, but a
  // CD in it never gets a current profile: there's no CD laser. Checked with
  // a music CD on a PS-SYSTEM 502R.
  if (strcmp(d->inquiry.vendor, "SONY") == 0 && strncmp(d->inquiry.product, "PS-SYSTEM", 9) == 0) {
    d->caps &= ~(uint32_t)(RIP_CAP_READ_CD | RIP_CAP_CD_C2);
  }
  probe_firmware(d);
}

int rip_drive_probe(struct rip_drive *d, struct rip_transport *t, enum rip_drive_source source) {
  struct mmc_feature f;
  uint16_t profiles[64];
  size_t count = 0;
  size_t i;
  int rc;

  memset(d, 0, sizeof(*d));
  d->t = t;
  d->source = source;
  rc = mmc_inquiry(t, &d->inquiry);
  if (rc != RIP_OK) {
    return rc;
  }

  if (mmc_get_feature(t, MMC_FEATURE_CD_READ, &f) == RIP_OK) {
    d->caps |= RIP_CAP_READ_CD;
    if (f.data_len > 0 && (f.data[0] & 0x02)) {
      d->caps |= RIP_CAP_CD_C2;
    }
  }
  if (mmc_get_feature(t, MMC_FEATURE_DVD_READ, &f) == RIP_OK) {
    d->caps |= RIP_CAP_READ_DVD;
  }
  if (mmc_get_feature(t, MMC_FEATURE_BD_READ, &f) == RIP_OK) {
    d->caps |= RIP_CAP_READ_BD;
  }

  // The profile list backs up the feature list; some drives omit read features.
  if (mmc_supported_profiles(t, profiles, RIP_ARRAY_LEN(profiles), &count) == RIP_OK) {
    for (i = 0; i < count; i++) {
      if (mmc_profile_is_cd(profiles[i])) {
        d->caps |= RIP_CAP_READ_CD;
      } else if (mmc_profile_is_dvd(profiles[i])) {
        d->caps |= RIP_CAP_READ_DVD;
      } else if (mmc_profile_is_bd(profiles[i])) {
        d->caps |= RIP_CAP_READ_BD;
      }
      if (profiles[i] >= 0xFF00) {
        d->caps |= RIP_CAP_SONY_PROFILES;
      }
    }
  }

  probe_vendor_caps(d);
  return RIP_OK;
}

enum rip_media_state rip_drive_media(struct rip_drive *d, uint16_t *profile) {
  struct rip_sense sense;
  int rc;

  *profile = MMC_PROFILE_NONE;
  memset(&sense, 0, sizeof(sense));
  rc = mmc_current_profile(d->t, profile, &sense);
  d->last_sense = sense;
  d->last_profile = *profile;
  if (rc == RIP_ESENSE) {
    if (sense.key == 0x02 && sense.asc == 0x3A) {
      return RIP_MEDIA_EMPTY;
    }
    if (sense.key == 0x02 && sense.asc == 0x30) {
      return RIP_MEDIA_INCOMPATIBLE;
    }
    return RIP_MEDIA_NOT_READY;
  }
  if (rc != RIP_OK) {
    return RIP_MEDIA_ERROR;
  }
  if (*profile == MMC_PROFILE_NONE) {
    // Some drives answer GET CONFIGURATION with profile 0 for an empty tray,
    // and the PS5's for a disc it won't read: TEST UNIT READY tells them apart.
    rc = mmc_test_unit_ready(d->t, &sense);
    d->last_sense = sense;
    if (rc == RIP_ESENSE && sense.key == 0x02 && sense.asc == 0x3A) {
      return RIP_MEDIA_EMPTY;
    }
    if (rc == RIP_ESENSE && sense.key == 0x02 && sense.asc == 0x30) {
      return RIP_MEDIA_INCOMPATIBLE;
    }
    return rc == RIP_OK ? RIP_MEDIA_EMPTY : RIP_MEDIA_NOT_READY;
  }
  if (*profile == 0xFFFF) {
    return RIP_MEDIA_INCOMPATIBLE;
  }
  // Present only once the unit is ready, so the first read does not fail.
  rc = mmc_test_unit_ready(d->t, &sense);
  d->last_sense = sense;
  return rc == RIP_OK ? RIP_MEDIA_PRESENT : RIP_MEDIA_NOT_READY;
}

void rip_caps_str(uint32_t caps, char *buf, size_t len) {
  static const struct {
    uint32_t bit;
    const char *name;
  } names[] = {
      {RIP_CAP_READ_CD, "cd"},
      {RIP_CAP_READ_DVD, "dvd"},
      {RIP_CAP_READ_BD, "bd"},
      {RIP_CAP_CD_C2, "c2"},
      {RIP_CAP_SONY_PROFILES, "sony-profiles"},
      {RIP_CAP_PLEXTOR, "plextor"},
      {RIP_CAP_MTK_CACHE, "mtk-cache"},
      {RIP_CAP_KREON, "kreon"},
      {RIP_CAP_OMNIDRIVE, "omnidrive"},
      {RIP_CAP_LIBREDRIVE, "libredrive"},
      {RIP_CAP_XBOX, "xbox"},
  };
  size_t used = 0;
  size_t i;

  if (len == 0) {
    return;
  }
  buf[0] = '\0';
  for (i = 0; i < RIP_ARRAY_LEN(names); i++) {
    if (caps & names[i].bit) {
      int n = snprintf(buf + used, len - used, "%s%s", used ? "," : "", names[i].name);
      if (n < 0 || (size_t)n >= len - used) {
        break;
      }
      used += (size_t)n;
    }
  }
  if (used == 0) {
    snprintf(buf, len, "none");
  }
}

const char *rip_drive_source_name(enum rip_drive_source source) {
  return source == RIP_DRIVE_INTERNAL ? "internal" : "external";
}
