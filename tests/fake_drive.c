#include "fake_drive.h"

#include <stdio.h>
#include <string.h>

#include "ripper/common.h"

static int check(struct rip_sense *sense, uint8_t key, uint8_t asc) {
  sense->key = key;
  sense->asc = asc;
  sense->ascq = 0;
  return RIP_ESENSE;
}

static size_t put_feature(uint8_t *p, uint16_t code, const uint8_t *data, uint8_t len) {
  rip_put_be16(p, code);
  p[2] = 0x01;  // current
  p[3] = len;
  memcpy(p + 4, data, len);
  return 4u + len;
}

static int get_configuration(struct fake_drive *d, const uint8_t *cdb, uint8_t *buf, size_t len,
                             struct rip_sense *sense) {
  uint16_t start = rip_be16(cdb + 2);
  uint8_t out[256];
  size_t n = 8;

  memset(out, 0, sizeof(out));
  if (!d->image && start != 0) {
    return check(sense, 0x02, 0x3A);
  }
  rip_put_be16(out + 6, d->image ? d->profile : 0);
  if (start == 0x0000) {
    uint8_t list[32];
    uint8_t l = 0;
    if (d->cd) {
      rip_put_be16(list + l, 0x0008);
      l += 4;
    }
    if (d->dvd) {
      rip_put_be16(list + l, 0x0010);
      l += 4;
    }
    if (d->bd) {
      rip_put_be16(list + l, 0x0040);
      l += 4;
    }
    if (d->sony) {
      rip_put_be16(list + l, 0xFF71);
      l += 4;
    }
    n += put_feature(out + n, 0x0000, list, l);
  } else if ((start == 0x001E && d->cd) || (start == 0x001F && d->dvd) || (start == 0x0040 && d->bd)) {
    uint8_t data[4] = {0, 0, 0, 0};
    n += put_feature(out + n, start, data, sizeof(data));
  }
  rip_put_be32(out, (uint32_t)(n - 4));
  memcpy(buf, out, len < n ? len : n);
  return RIP_OK;
}

static int fake_exec(void *ctx, const uint8_t *cdb, size_t cdb_len, enum rip_dir dir, void *buf, size_t len,
                     unsigned timeout_ms, struct rip_sense *sense) {
  struct fake_drive *d = ctx;

  (void)cdb_len;
  (void)dir;
  (void)timeout_ms;
  switch (cdb[0]) {
  case 0x12: {  // INQUIRY
    uint8_t inq[36];
    memset(inq, ' ', sizeof(inq));
    inq[0] = 0x05;
    memcpy(inq + 8, "FAKE    ", 8);
    memcpy(inq + 16, "TEST DRIVE      ", 16);
    memcpy(inq + 32, "1.00", 4);
    memcpy(buf, inq, len < sizeof(inq) ? len : sizeof(inq));
    return RIP_OK;
  }
  case 0x00:  // TEST UNIT READY
    return d->image ? RIP_OK : check(sense, 0x02, 0x3A);
  case 0x46:
    return get_configuration(d, cdb, buf, len, sense);
  case 0x25: {  // READ CAPACITY
    uint8_t *p = buf;
    if (!d->image) {
      return check(sense, 0x02, 0x3A);
    }
    rip_put_be32(p, (uint32_t)(d->image_len / 2048 - 1));
    rip_put_be32(p + 4, 2048);
    return RIP_OK;
  }
  case 0xA8: {  // READ(12)
    uint32_t lba = rip_be32(cdb + 2);
    uint32_t count = rip_be32(cdb + 6);
    if (!d->image) {
      return check(sense, 0x02, 0x3A);
    }
    if ((uint64_t)(lba + count) * 2048 > d->image_len || (size_t)count * 2048 > len) {
      return check(sense, 0x05, 0x21);
    }
    d->reads++;
    if (d->fail_lba >= 0 && (uint32_t)d->fail_lba >= lba && (uint32_t)d->fail_lba < lba + count && count > 1) {
      // Multi-sector reads over the weak sector fail; single-sector retries succeed.
      return check(sense, 0x03, 0x11);
    }
    memcpy(buf, d->image + (size_t)lba * 2048, (size_t)count * 2048);
    return RIP_OK;
  }
  case 0xAD: {  // READ DISC STRUCTURE
    uint8_t *p = buf;
    memset(p, 0, len);
    if (cdb[7] == 0x01 && len >= 6) {
      p[4] = d->cps_type;
    }
    return RIP_OK;
  }
  case 0xB6:  // SET STREAMING
  case 0xBB:  // SET CD SPEED
    return RIP_OK;
  default:
    return check(sense, 0x05, 0x20);
  }
}

static const struct rip_transport_ops fake_ops = {fake_exec, NULL};

void fake_drive_transport(struct fake_drive *fd, struct rip_transport *t) {
  memset(t, 0, sizeof(*t));
  t->ops = &fake_ops;
  t->ctx = fd;
  snprintf(t->name, sizeof(t->name), "fake");
}
