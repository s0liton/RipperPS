#include "cam_transport.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/types.h>
#include <unistd.h>

// CAM's headers want sys/types.h's BSD types first, and cam.h before the rest,
// so they stay in this order (and out of the formatter's include sorting).
// clang-format off
#include <cam/cam.h>
#include <cam/cam_ccb.h>
#include <cam/scsi/scsi_message.h>
#include <cam/scsi/scsi_pass.h>
// clang-format on

#include "ripper/common.h"
#include "ripper/log.h"

// The PS5 kernel is FreeBSD-based and keeps CAM's pass(4) interface. Its
// headers report CAM_VERSION 0x19, whose CCB is 0x4E0 bytes; the ioctl numbers
// encode that size, so a mismatch fails with ENOTTY rather than corrupting memory.
_Static_assert(sizeof(union ccb) == 0x4E0, "CCB size differs from the PS5 kernel");

struct cam_ctx {
  struct rip_transport *t;  // for last_error
  char periph[16];          // "cd", and its unit: found again after a USB reset
  unsigned unit;
  int fd;
  path_id_t path;
  target_id_t target;
  lun_id_t lun;
  union ccb ccb;  // one command at a time, so one CCB is reused
};

static int cam_locate(struct cam_ctx *c, char *dev, size_t dn);

static int get_devlist(int fd, const char *periph, unsigned unit, union ccb *ccb) {
  memset(ccb, 0, sizeof(*ccb));
  ccb->cgdl.ccb_h.func_code = XPT_GDEVLIST;
  if (periph) {
    snprintf(ccb->cgdl.periph_name, sizeof(ccb->cgdl.periph_name), "%s", periph);
    ccb->cgdl.unit_number = unit;
  }
  if (ioctl(fd, CAMGETPASSTHRU, ccb) < 0 || ccb->cgdl.status == CAM_GDEVLIST_ERROR) {
    return RIP_ERR;
  }
  return RIP_OK;
}

static int cam_exec(void *vctx, const uint8_t *cdb, size_t cdb_len, enum rip_dir dir, void *buf, size_t len,
                    unsigned timeout_ms, struct rip_sense *sense) {
  struct cam_ctx *c = vctx;
  union ccb *ccb = &c->ccb;
  uint32_t flags = dir == RIP_DIR_IN ? CAM_DIR_IN : dir == RIP_DIR_OUT ? CAM_DIR_OUT : CAM_DIR_NONE;
  uint32_t status;

  int again = 1;

retry:
  memset(ccb, 0, sizeof(*ccb));
  ccb->csio.ccb_h.func_code = XPT_SCSI_IO;
  ccb->csio.ccb_h.path_id = c->path;
  ccb->csio.ccb_h.target_id = c->target;
  ccb->csio.ccb_h.target_lun = c->lun;
  ccb->csio.ccb_h.flags = flags;
  ccb->csio.ccb_h.retry_count = 1;
  ccb->csio.ccb_h.timeout = timeout_ms;
  ccb->csio.data_ptr = buf;
  ccb->csio.dxfer_len = (uint32_t)len;
  ccb->csio.sense_len = SSD_FULL_SIZE;
  ccb->csio.cdb_len = (uint8_t)cdb_len;
  ccb->csio.tag_action = MSG_SIMPLE_Q_TAG;
  memcpy(ccb->csio.cdb_io.cdb_bytes, cdb, cdb_len);

  if (ioctl(c->fd, CAMIOCOMMAND, ccb) < 0) {
    int err = errno;
    char dev[64];
    snprintf(c->t->last_error, sizeof(c->t->last_error), "CAMIOCOMMAND failed (errno %d)", err);
    // The device went away under us (a USB drive reset): find it again once.
    if (again && (err == ENOTTY || err == ENXIO || err == EBADF)) {
      again = 0;
      if (c->fd >= 0) {
        close(c->fd);
        c->fd = -1;
      }
      if (cam_locate(c, dev, sizeof(dev)) == RIP_OK) {
        snprintf(c->t->name, sizeof(c->t->name), "%s (%s%u)", dev, c->periph, c->unit);
        goto retry;
      }
    }
    return RIP_ERR;
  }
  status = ccb->csio.ccb_h.status;
  if ((status & CAM_STATUS_MASK) == CAM_REQ_CMP) {
    return RIP_OK;
  }
  if ((status & CAM_STATUS_MASK) == CAM_SCSI_STATUS_ERROR && (status & CAM_AUTOSNS_VALID) &&
      ccb->csio.scsi_status == 0x02) {
    size_t n = ccb->csio.sense_len > ccb->csio.sense_resid ? ccb->csio.sense_len - ccb->csio.sense_resid : 0;
    rip_sense_decode((const uint8_t *)&ccb->csio.sense_data, n, sense);
    return RIP_ESENSE;
  }
  snprintf(c->t->last_error, sizeof(c->t->last_error), "CAM status 0x%02x, SCSI status 0x%02x",
           status & CAM_STATUS_MASK, ccb->csio.scsi_status);
  return RIP_ERR;
}

static void cam_close(void *vctx) {
  struct cam_ctx *c = vctx;

  if (c->fd >= 0) {
    close(c->fd);
  }
  free(c);
}

static const struct rip_transport_ops cam_ops = {cam_exec, cam_close};

// Finds the pass device behind <periph><unit> and opens it: at the start, and
// again when a USB drive was reset and came back under a new pass device.
static int cam_locate(struct cam_ctx *c, char *dev, size_t dn) {
  int xpt = open("/dev/xpt0", O_RDWR);

  if (xpt < 0) {
    return RIP_ERR;
  }
  if (get_devlist(xpt, c->periph, c->unit, &c->ccb) != RIP_OK) {
    close(xpt);
    return RIP_ENOENT;
  }
  close(xpt);
  snprintf(dev, dn, "/dev/%s%u", c->ccb.cgdl.periph_name, c->ccb.cgdl.unit_number);
  c->fd = open(dev, O_RDWR);
  if (c->fd < 0) {
    return RIP_ERR;
  }
  if (get_devlist(c->fd, NULL, 0, &c->ccb) != RIP_OK) {
    close(c->fd);
    c->fd = -1;
    return RIP_ERR;
  }
  c->path = c->ccb.cgdl.ccb_h.path_id;
  c->target = c->ccb.cgdl.ccb_h.target_id;
  c->lun = c->ccb.cgdl.ccb_h.target_lun;
  return RIP_OK;
}

int cam_transport_open(struct rip_transport *t, const char *periph, unsigned unit) {
  struct cam_ctx *c;
  char dev[64];
  int rc;

  memset(t, 0, sizeof(*t));
  c = calloc(1, sizeof(*c));
  if (!c) {
    return RIP_ENOMEM;
  }
  snprintf(c->periph, sizeof(c->periph), "%s", periph);
  c->unit = unit;
  c->fd = -1;
  rc = cam_locate(c, dev, sizeof(dev));
  if (rc != RIP_OK) {
    free(c);
    return rc;
  }
  t->ops = &cam_ops;
  t->ctx = c;
  c->t = t;
  snprintf(t->name, sizeof(t->name), "%s (%s%u)", dev, periph, unit);
  return RIP_OK;
}
