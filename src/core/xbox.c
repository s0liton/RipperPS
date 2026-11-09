// Xbox and Xbox 360 discs on Kreon or OmniDrive firmware. After redumper
// (GPL-3): dvd/xbox.ixx and the XGD parts of dvd/dvd_dump.ixx.

#include "ripper/xbox.h"

#include <stdlib.h>
#include <string.h>

#include "ripper/common.h"
#include "ripper/log.h"
#include "ripper/mmc.h"
#include "ripper/omnidrive.h"

#define SS_LEADOUT_PSN 0x00FD021E
#define SS_RANGE_COUNT 0x660
#define SS_DRIVE_TABLE 0x661
#define SS_DRIVE_TABLE_COPY 0x730
#define SS_DRIVE_ENTRY 9
#define SS_DRIVE_ENTRIES 23

enum kreon_lock { KREON_LOCKED = 0, KREON_WXRIPPER = 2 };

static int32_t s24(const uint8_t *p) {
  uint32_t v = ((uint32_t)p[0] << 16) | ((uint32_t)p[1] << 8) | p[2];

  return (v & 0x800000u) ? (int32_t)(v | 0xFF000000u) : (int32_t)v;
}

// The 24-bit sector numbers of a layer descriptor sit in the low bytes of 32-bit fields.
static int32_t ld_field(const uint8_t *ld, unsigned at) {
  return s24(ld + at + 1);
}

static unsigned xgd_version(int32_t layer0_last) {
  switch (layer0_last) {
  case 2110383:
    return 1;
  case 2110367:
    return 2;
  case 2330127:
    return 3;
  default:
    return 0;
  }
}

static int32_t psn_to_lba(int32_t psn, int32_t layer0_last) {
  psn -= 0x30000;
  if (psn < 0) {
    psn += 2 * (layer0_last + 1);
  }
  return psn;
}

static void add_range(struct xbox_layout *x, int64_t start, int64_t end) {
  if (x->range_count < XBOX_MAX_RANGES && start >= 0 && end > start) {
    x->ranges[x->range_count].start = (uint32_t)start;
    x->ranges[x->range_count].end = (uint32_t)end;
    x->range_count++;
  }
}

int xbox_layout_from(struct xbox_layout *x, const uint8_t pfi[17], uint32_t capacity, int kreon) {
  int32_t ss_l0_last = ld_field(x->ss, 12);
  int32_t psn_first = ld_field(pfi, 4);
  int32_t psn_last = ld_field(pfi, 8);
  int32_t l0_last = ld_field(pfi, 12);
  int32_t ss_psn_first = ld_field(x->ss, 4);
  int64_t length;
  int64_t padding;
  unsigned count = x->ss[SS_RANGE_COUNT];
  unsigned i;

  x->version = xgd_version(ss_l0_last);
  x->kreon = kreon;
  x->range_count = 0;
  if (!x->version) {
    return RIP_ERR;
  }
  // The video partition's descriptor with the game partition's layer break.
  length = (int64_t)psn_last + 1 - psn_first;
  if (pfi[2] & 0x10) {
    length += 2 * ((int64_t)ss_l0_last + 1);
  }
  if (length <= 0 || length > 0xFFFFFFFFLL) {
    return RIP_ERR;
  }
  x->sectors = (uint32_t)length;
  for (i = 0; i < count && i < SS_DRIVE_ENTRIES; i++) {
    const uint8_t *e = x->ss + SS_DRIVE_TABLE + i * SS_DRIVE_ENTRY;
    if ((x->version == 1 && i >= 16) || (x->version != 1 && i != 0 && i != 3)) {
      continue;
    }
    add_range(x, psn_to_lba(s24(e + 3), ss_l0_last), (int64_t)psn_to_lba(s24(e + 6), ss_l0_last) + 1);
  }
  padding = (int64_t)ss_psn_first - l0_last - 1;
  if (x->version == 3) {
    padding += 4096;
  }
  if (kreon) {
    // Kreon can't read the padding before the second layer's video partition.
    add_range(x, capacity, (int64_t)capacity + padding);
  }
  x->lock_lba = (uint32_t)((int64_t)capacity + padding);
  x->l1_video_lba = (uint32_t)(l0_last + 1 - psn_first);
  return RIP_OK;
}

static int kreon_lock(struct rip_transport *t, enum kreon_lock state) {
  uint8_t cdb[10] = {0xFF, 0x08, 0x01, 0x11, (uint8_t)state, 0, 0, 0, 0, 0};

  return rip_scsi(t, cdb, sizeof(cdb), RIP_DIR_NONE, NULL, 0, MMC_TIMEOUT_MS, NULL);
}

static int kreon_security_sector(struct rip_transport *t, uint8_t ss[2048]) {
  static const uint8_t parts[] = {0x01, 0x03, 0x05, 0x07};
  unsigned i;

  for (i = 0; i < sizeof(parts); i++) {
    uint8_t cdb[12] = {0xAD, 0x00, 0xFF, 0x02, 0xFD, 0xFF, 0xFE, 0x00, 0x08, 0x00, parts[i], 0xC0};
    if (rip_scsi(t, cdb, sizeof(cdb), RIP_DIR_IN, ss, 2048, MMC_TIMEOUT_MS, NULL) != RIP_OK) {
      if (i == 0) {
        return RIP_ERR;
      }
      RIP_WARN("Kreon: security sector incomplete (%u of 4 parts)", i);
      break;
    }
  }
  return RIP_OK;
}

// OmniDrive reads the security sector from the lead-out; its drive table
// comes shuffled, the order keyed by CPR_MAI.
static int omni_security_sector(struct rip_transport *t, uint8_t ss[2048]) {
  uint8_t *frame = malloc(DVD_RAW_FRAME);
  uint8_t key[4];
  uint8_t table[SS_DRIVE_ENTRIES * SS_DRIVE_ENTRY];
  unsigned k;
  unsigned i;
  int found = 0;

  if (!frame) {
    return RIP_ENOMEM;
  }
  for (k = 0; k < 4 && !found; k++) {
    if (omni_read(t, OMNI_DVD, 1, SS_LEADOUT_PSN + (int32_t)k * 0x40, 1, frame, DVD_RAW_FRAME, NULL) == RIP_OK) {
      found = dvd_frame_fix(frame, -1);
    }
  }
  if (!found) {
    free(frame);
    return RIP_ERR;
  }
  memcpy(ss, frame + 12, 2048);
  memcpy(key, frame + 7, 4);  // CPR_MAI bytes 1 to 4
  free(frame);
  switch (xgd_version(ld_field(ss, 12))) {
  case 1:
  case 2:
    memcpy(ss + 0x2D0, key, 4);
    break;
  case 3:
    memcpy(ss + 0x0F0, key, 4);
    break;
  default:
    break;
  }
  for (i = 0; i < sizeof(table); i++) {
    uint8_t at = (uint8_t)(ss[SS_DRIVE_TABLE_COPY + i] ^ key[i % 4]);
    table[i] = at < sizeof(table) ? ss[SS_DRIVE_TABLE + at] : 0;
  }
  memcpy(ss + SS_DRIVE_TABLE, table, sizeof(table));
  memcpy(ss + SS_DRIVE_TABLE_COPY, table, sizeof(table));
  return RIP_OK;
}

int xbox_read_layout(struct rip_drive *d, struct xbox_layout *out) {
  int kreon = (d->caps & RIP_CAP_KREON) != 0;
  uint8_t pfi[4 + 2048];
  uint32_t last = 0;
  uint32_t block = 0;
  int rc;

  memset(out, 0, sizeof(*out));
  if (!kreon && !(d->caps & RIP_CAP_OMNIDRIVE)) {
    return RIP_ENOTSUP;
  }
  // Kreon first: unlocked, the drive sizes the disc differently.
  if (kreon && kreon_lock(d->t, KREON_WXRIPPER) != RIP_OK) {
    RIP_WARN("Kreon: the drive didn't unlock");
  }
  rc = mmc_read_capacity(d->t, &last, &block);
  if (rc == RIP_OK) {
    rc = mmc_read_disc_structure(d->t, 0, 0, 0, 0x00, pfi, sizeof(pfi), NULL);
  }
  if (rc == RIP_OK) {
    rc = kreon ? kreon_security_sector(d->t, out->ss) : omni_security_sector(d->t, out->ss);
    if (rc != RIP_OK) {
      RIP_WARN("%s: no security sector from this disc", kreon ? "Kreon" : "OmniDrive");
    }
  }
  if (rc == RIP_OK) {
    rc = xbox_layout_from(out, pfi + 4, last + 1, kreon);
  }
  if (rc != RIP_OK) {
    xbox_relock(d, out);
    return rc;
  }
  RIP_INFO("%s: XGD%u, %u sectors, %u protected range(s)", kreon ? "Kreon" : "OmniDrive", out->version, out->sectors,
           out->range_count);
  return RIP_OK;
}

void xbox_relock(struct rip_drive *d, const struct xbox_layout *x) {
  (void)x;
  if (d->caps & RIP_CAP_KREON) {
    kreon_lock(d->t, KREON_LOCKED);
  }
}

/* Reader */

struct xbox_ctx {
  struct rip_drive *d;
  struct xbox_layout x;
  int locked;  // Kreon: locked again for the second video partition
};

// Whether lba is in a protected range; *next is where that range ends, or where the next one starts.
static int in_range(const struct xbox_layout *x, uint64_t lba, uint64_t *next) {
  unsigned i;

  *next = UINT64_MAX;
  for (i = 0; i < x->range_count; i++) {
    if (lba >= x->ranges[i].start && lba < x->ranges[i].end) {
      *next = x->ranges[i].end;
      return 1;
    }
    if (x->ranges[i].start > lba && x->ranges[i].start < *next) {
      *next = x->ranges[i].start;
    }
  }
  return 0;
}

static int xbox_run(struct xbox_ctx *c, uint64_t lba, uint32_t n, uint8_t *out) {
  int64_t shift = 0;

  if (c->x.kreon) {
    int want = lba >= c->x.lock_lba;
    if (want != c->locked) {
      if (kreon_lock(c->d->t, want ? KREON_LOCKED : KREON_WXRIPPER) != RIP_OK) {
        RIP_ERROR("Kreon: the drive didn't %s", want ? "lock" : "unlock");
        return RIP_ERR;
      }
      c->locked = want;
    }
    if (want) {
      shift = (int64_t)c->x.l1_video_lba - c->x.lock_lba;
    }
  }
  return mmc_read12(c->d->t, (uint32_t)((int64_t)lba + shift), n, out, 2048, NULL);
}

static int xbox_read(void *ctx, uint64_t lba, uint32_t count, void *buf) {
  struct xbox_ctx *c = ctx;
  uint8_t *out = buf;

  while (count) {
    uint64_t next;
    uint64_t end = lba + count;
    uint32_t n;
    int rc = RIP_OK;
    int zero = in_range(&c->x, lba, &next);

    if (next < end) {
      end = next;
    }
    if (c->x.kreon && lba < c->x.lock_lba && end > c->x.lock_lba) {
      end = c->x.lock_lba;
    }
    n = (uint32_t)(end - lba);
    if (zero) {
      memset(out, 0, (size_t)n * 2048);
    } else {
      rc = xbox_run(c, lba, n, out);
    }
    if (rc != RIP_OK) {
      return rc;
    }
    lba += n;
    count -= n;
    out += (size_t)n * 2048;
  }
  return RIP_OK;
}

static void xbox_close(void *ctx) {
  struct xbox_ctx *c = ctx;

  xbox_relock(c->d, &c->x);
  free(c);
}

static const struct rip_reader_ops xbox_ops = {xbox_read, xbox_close};

int rip_reader_open_xbox(struct rip_reader *r, struct rip_drive *d, const struct xbox_layout *x) {
  struct xbox_ctx *c = calloc(1, sizeof(*c));

  memset(r, 0, sizeof(*r));
  if (!c) {
    return RIP_ENOMEM;
  }
  c->d = d;
  c->x = *x;
  r->ops = &xbox_ops;
  r->ctx = c;
  r->sector_size = 2048;
  r->sector_count = x->sectors;
  r->max_sectors = 32;
  return RIP_OK;
}
