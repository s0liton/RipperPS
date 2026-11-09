#include "sgio_transport.h"

#include <fcntl.h>
#include <scsi/sg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include "ripper/common.h"

struct sgio_ctx {
  int fd;
};

static int sgio_exec(void *vctx, const uint8_t *cdb, size_t cdb_len, enum rip_dir dir, void *buf, size_t len,
                     unsigned timeout_ms, struct rip_sense *sense) {
  struct sgio_ctx *c = vctx;
  sg_io_hdr_t io;
  uint8_t sense_buf[64];
  uint8_t cdb_copy[16];

  memcpy(cdb_copy, cdb, cdb_len);
  memset(&io, 0, sizeof(io));
  memset(sense_buf, 0, sizeof(sense_buf));
  io.interface_id = 'S';
  io.cmdp = cdb_copy;
  io.cmd_len = (unsigned char)cdb_len;
  io.dxferp = buf;
  io.dxfer_len = (unsigned int)len;
  io.dxfer_direction = dir == RIP_DIR_IN ? SG_DXFER_FROM_DEV : dir == RIP_DIR_OUT ? SG_DXFER_TO_DEV : SG_DXFER_NONE;
  io.sbp = sense_buf;
  io.mx_sb_len = sizeof(sense_buf);
  io.timeout = timeout_ms;

  if (ioctl(c->fd, SG_IO, &io) < 0) {
    return RIP_ERR;
  }
  if (io.status == 0x02 || (io.driver_status & 0x08) /* DRIVER_SENSE */) {
    rip_sense_decode(sense_buf, io.sb_len_wr, sense);
    return RIP_ESENSE;
  }
  if (io.status != 0 || io.host_status != 0 || (io.driver_status & 0x0F) != 0) {
    return RIP_ERR;
  }
  return RIP_OK;
}

static void sgio_close(void *vctx) {
  struct sgio_ctx *c = vctx;

  close(c->fd);
  free(c);
}

static const struct rip_transport_ops sgio_ops = {sgio_exec, sgio_close};

int sgio_transport_open(struct rip_transport *t, const char *device) {
  struct sgio_ctx *c;

  memset(t, 0, sizeof(*t));
  c = calloc(1, sizeof(*c));
  if (!c) {
    return RIP_ENOMEM;
  }
  // O_NONBLOCK opens the drive even with an empty tray.
  c->fd = open(device, O_RDWR | O_NONBLOCK);
  if (c->fd < 0) {
    c->fd = open(device, O_RDONLY | O_NONBLOCK);
  }
  if (c->fd < 0) {
    free(c);
    return RIP_ERR;
  }
  t->ops = &sgio_ops;
  t->ctx = c;
  snprintf(t->name, sizeof(t->name), "%s", device);
  return RIP_OK;
}
