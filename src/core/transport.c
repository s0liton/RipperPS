#include "ripper/transport.h"

#include <string.h>

#include "ripper/common.h"

int rip_scsi(struct rip_transport *t, const uint8_t *cdb, size_t cdb_len, enum rip_dir dir, void *buf, size_t len,
             unsigned timeout_ms, struct rip_sense *sense) {
  struct rip_sense scratch;

  if (!sense) {
    sense = &scratch;
  }
  memset(sense, 0, sizeof(*sense));
  if (!t || !t->ops || !t->ops->exec || !cdb || cdb_len == 0 || cdb_len > 16) {
    return RIP_ERR;
  }
  t->last_error[0] = '\0';
  return t->ops->exec(t->ctx, cdb, cdb_len, dir, buf, len, timeout_ms, sense);
}

void rip_transport_close(struct rip_transport *t) {
  if (t && t->ops && t->ops->close) {
    t->ops->close(t->ctx);
  }
  if (t) {
    t->ops = NULL;
    t->ctx = NULL;
  }
}

void rip_sense_decode(const uint8_t *raw, size_t len, struct rip_sense *out) {
  memset(out, 0, sizeof(*out));
  if (len < 4) {
    return;
  }
  if ((raw[0] & 0x7E) == 0x72) {
    out->key = raw[1] & 0x0F;
    out->asc = raw[2];
    out->ascq = raw[3];
    return;
  }
  if (len >= 3) {
    out->key = raw[2] & 0x0F;
  }
  // Fixed format: ASC/ASCQ exist only when the additional length reaches them.
  if (len >= 14 && raw[7] >= 6) {
    out->asc = raw[12];
    out->ascq = raw[13];
  } else if (len >= 13 && raw[7] >= 5) {
    out->asc = raw[12];
  }
}
