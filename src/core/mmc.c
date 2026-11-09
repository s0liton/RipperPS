#include "ripper/mmc.h"

#include <string.h>

#include "ripper/common.h"

static void copy_trimmed(char *dst, const uint8_t *src, size_t n) {
  memcpy(dst, src, n);
  dst[n] = '\0';
  while (n > 0 && (dst[n - 1] == ' ' || dst[n - 1] == '\0')) {
    dst[--n] = '\0';
  }
}

int mmc_inquiry(struct rip_transport *t, struct mmc_inquiry *out) {
  uint8_t cdb[6] = {0x12, 0, 0, 0, 36, 0};
  uint8_t data[36];
  int rc;

  memset(data, 0, sizeof(data));
  memset(out, 0, sizeof(*out));
  rc = rip_scsi(t, cdb, sizeof(cdb), RIP_DIR_IN, data, sizeof(data), MMC_TIMEOUT_MS, NULL);
  if (rc != RIP_OK) {
    return rc;
  }
  out->peripheral_type = data[0] & 0x1F;
  copy_trimmed(out->vendor, data + 8, 8);
  copy_trimmed(out->product, data + 16, 16);
  copy_trimmed(out->revision, data + 32, 4);
  return RIP_OK;
}

int mmc_test_unit_ready(struct rip_transport *t, struct rip_sense *sense) {
  uint8_t cdb[6] = {0x00, 0, 0, 0, 0, 0};

  return rip_scsi(t, cdb, sizeof(cdb), RIP_DIR_NONE, NULL, 0, MMC_TIMEOUT_MS, sense);
}

// GET CONFIGURATION. rt: 0 = all features, 2 = the one feature at start.
static int get_configuration(struct rip_transport *t, uint8_t rt, uint16_t start, uint8_t *buf, uint16_t len,
                             struct rip_sense *sense) {
  uint8_t cdb[10] = {0x46, rt, 0, 0, 0, 0, 0, 0, 0, 0};

  rip_put_be16(cdb + 2, start);
  rip_put_be16(cdb + 7, len);
  memset(buf, 0, len);
  return rip_scsi(t, cdb, sizeof(cdb), RIP_DIR_IN, buf, len, MMC_TIMEOUT_MS, sense);
}

int mmc_current_profile(struct rip_transport *t, uint16_t *profile, struct rip_sense *sense) {
  uint8_t data[8];
  int rc = get_configuration(t, 2, MMC_FEATURE_PROFILE_LIST, data, sizeof(data), sense);

  if (rc == RIP_OK) {
    *profile = rip_be16(data + 6);
  }
  return rc;
}

int mmc_get_feature(struct rip_transport *t, uint16_t code, struct mmc_feature *out) {
  uint8_t data[8 + 4 + 252];
  uint32_t total;
  uint8_t add_len;
  int rc;

  memset(out, 0, sizeof(*out));
  rc = get_configuration(t, 2, code, data, sizeof(data), NULL);
  if (rc != RIP_OK) {
    return rc;
  }
  total = rip_be32(data);  // bytes after this field
  if (total < 4 + 4 || rip_be16(data + 8) != code) {
    return RIP_ENOENT;
  }
  add_len = data[11];
  out->code = code;
  out->current = data[10] & 0x01;
  out->version = (data[10] >> 2) & 0x0F;
  out->data_len = add_len < sizeof(out->data) ? add_len : (uint8_t)sizeof(out->data);
  memcpy(out->data, data + 12, out->data_len);
  return RIP_OK;
}

int mmc_supported_profiles(struct rip_transport *t, uint16_t *profiles, size_t max, size_t *count) {
  uint8_t data[8 + 4 + 252];
  size_t n = 0;
  size_t i;
  int rc;

  *count = 0;
  rc = get_configuration(t, 2, MMC_FEATURE_PROFILE_LIST, data, sizeof(data), NULL);
  if (rc != RIP_OK) {
    return rc;
  }
  if (rip_be16(data + 8) != MMC_FEATURE_PROFILE_LIST) {
    return RIP_ENOENT;
  }
  // Profile descriptors are 4 bytes each: profile (2), current flag, reserved.
  for (i = 0; i + 4 <= data[11] && n < max; i += 4) {
    profiles[n++] = rip_be16(data + 12 + i);
  }
  *count = n;
  return RIP_OK;
}

int mmc_read_capacity(struct rip_transport *t, uint32_t *last_lba, uint32_t *block_len) {
  uint8_t cdb[10] = {0x25, 0, 0, 0, 0, 0, 0, 0, 0, 0};
  uint8_t data[8];
  int rc;

  memset(data, 0, sizeof(data));
  rc = rip_scsi(t, cdb, sizeof(cdb), RIP_DIR_IN, data, sizeof(data), MMC_TIMEOUT_MS, NULL);
  if (rc != RIP_OK) {
    return rc;
  }
  *last_lba = rip_be32(data);
  *block_len = rip_be32(data + 4);
  return RIP_OK;
}

int mmc_read12(struct rip_transport *t, uint32_t lba, uint32_t count, void *buf, size_t block_len,
               struct rip_sense *sense) {
  uint8_t cdb[12] = {0xA8, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0};

  rip_put_be32(cdb + 2, lba);
  rip_put_be32(cdb + 6, count);
  return rip_scsi(t, cdb, sizeof(cdb), RIP_DIR_IN, buf, (size_t)count * block_len, MMC_READ_TIMEOUT_MS, sense);
}

int mmc_read_disc_structure(struct rip_transport *t, uint8_t media_type, uint32_t address, uint8_t layer,
                            uint8_t format, void *buf, uint16_t len, struct rip_sense *sense) {
  uint8_t cdb[12] = {0xAD, (uint8_t)(media_type & 0x0F), 0, 0, 0, 0, layer, format, 0, 0, 0, 0};

  rip_put_be32(cdb + 2, address);
  rip_put_be16(cdb + 8, len);
  memset(buf, 0, len);
  return rip_scsi(t, cdb, sizeof(cdb), RIP_DIR_IN, buf, len, MMC_TIMEOUT_MS, sense);
}

int mmc_dvd_copyright(struct rip_transport *t, uint8_t *cps_type, uint8_t *region_mask) {
  uint8_t data[8];
  int rc = mmc_read_disc_structure(t, 0, 0, 0, 0x01, data, sizeof(data), NULL);

  if (rc != RIP_OK) {
    return rc;
  }
  *cps_type = data[4];
  if (region_mask) {
    *region_mask = data[5];
  }
  return RIP_OK;
}

int mmc_set_max_speed(struct rip_transport *t, uint32_t end_lba) {
  uint8_t streaming[12] = {0xB6, 0, 0, 0, 0, 0, 0, 0, 0, 0, 28, 0};
  uint8_t cd_speed[12] = {0xBB, 0, 0xFF, 0xFF, 0xFF, 0xFF, 0, 0, 0, 0, 0, 0};
  uint8_t pd[28];

  if (end_lba > 0) {
    // Performance descriptor: request far more than any drive can do over
    // 1000 ms so the drive settles on its own maximum.
    memset(pd, 0, sizeof(pd));
    rip_put_be32(pd + 8, end_lba);
    rip_put_be32(pd + 12, 0x10000000u);
    rip_put_be32(pd + 16, 1000);
    rip_put_be32(pd + 20, 0x10000000u);
    rip_put_be32(pd + 24, 1000);
    if (rip_scsi(t, streaming, sizeof(streaming), RIP_DIR_OUT, pd, sizeof(pd), MMC_TIMEOUT_MS, NULL) == RIP_OK) {
      return RIP_OK;
    }
  }
  return rip_scsi(t, cd_speed, sizeof(cd_speed), RIP_DIR_NONE, NULL, 0, MMC_TIMEOUT_MS, NULL);
}

int mmc_set_cd_speed(struct rip_transport *t, uint16_t kbps) {
  uint8_t cdb[12] = {0xBB, 0, 0, 0, 0xFF, 0xFF, 0, 0, 0, 0, 0, 0};

  rip_put_be16(cdb + 2, kbps);
  return rip_scsi(t, cdb, sizeof(cdb), RIP_DIR_NONE, NULL, 0, MMC_TIMEOUT_MS, NULL);
}

int mmc_profile_is_cd(uint16_t p) {
  return p == MMC_PROFILE_CD_ROM || p == MMC_PROFILE_CD_R || p == MMC_PROFILE_CD_RW || p == MMC_PROFILE_PS1_CD ||
         p == MMC_PROFILE_PS2_CD;
}

int mmc_profile_is_dvd(uint16_t p) {
  return (p >= 0x0010 && p <= 0x002B) || p == MMC_PROFILE_PS2_DVD || p == MMC_PROFILE_PS3_DVD;
}

int mmc_profile_is_bd(uint16_t p) {
  return (p >= 0x0040 && p <= 0x0043) || p == MMC_PROFILE_PS3_BD || p == MMC_PROFILE_PS4_BD || p == MMC_PROFILE_PS5_BD;
}

const char *mmc_profile_name(uint16_t p) {
  switch (p) {
  case MMC_PROFILE_NONE:
    return "no disc";
  case MMC_PROFILE_CD_ROM:
    return "CD-ROM";
  case MMC_PROFILE_CD_R:
    return "CD-R";
  case MMC_PROFILE_CD_RW:
    return "CD-RW";
  case MMC_PROFILE_DVD_ROM:
    return "DVD-ROM";
  case 0x0011:
    return "DVD-R";
  case 0x0012:
    return "DVD-RAM";
  case 0x0013:
  case 0x0014:
    return "DVD-RW";
  case 0x0015:
  case 0x0016:
    return "DVD-R DL";
  case 0x001A:
    return "DVD+RW";
  case 0x001B:
    return "DVD+R";
  case 0x002A:
    return "DVD+RW DL";
  case 0x002B:
    return "DVD+R DL";
  case MMC_PROFILE_BD_ROM:
    return "BD-ROM";
  case MMC_PROFILE_BD_R_SRM:
  case MMC_PROFILE_BD_R_RRM:
    return "BD-R";
  case MMC_PROFILE_BD_RE:
    return "BD-RE";
  case MMC_PROFILE_PS1_CD:
    return "PS1 CD-ROM";
  case MMC_PROFILE_PS2_CD:
    return "PS2 CD-ROM";
  case MMC_PROFILE_PS2_DVD:
    return "PS2 DVD-ROM";
  case MMC_PROFILE_PS3_DVD:
    return "PS3 DVD-ROM";
  case MMC_PROFILE_PS3_BD:
    return "PS3 BD-ROM";
  case MMC_PROFILE_PS4_BD:
    return "PS4 BD-ROM";
  case MMC_PROFILE_PS5_BD:
    return "PS5 BD-ROM";
  default:
    return "unknown";
  }
}
