#ifndef RIPPER_TRANSPORT_H_
#define RIPPER_TRANSPORT_H_

#include <stddef.h>
#include <stdint.h>

// A transport sends one SCSI/MMC command to one drive. The core never knows
// whether that is FreeBSD CAM on the PS5, SG_IO on Linux, or a test fake.

enum rip_dir {
  RIP_DIR_NONE,
  RIP_DIR_IN,  // drive to host
  RIP_DIR_OUT  // host to drive
};

struct rip_sense {
  uint8_t key;
  uint8_t asc;
  uint8_t ascq;
};

struct rip_transport_ops {
  // Returns RIP_OK, RIP_ESENSE (sense is filled), or RIP_ERR.
  int (*exec)(void *ctx, const uint8_t *cdb, size_t cdb_len, enum rip_dir dir, void *buf, size_t len,
              unsigned timeout_ms, struct rip_sense *sense);
  void (*close)(void *ctx);
};

struct rip_transport {
  const struct rip_transport_ops *ops;
  void *ctx;
  char name[64];  // device path for logs, e.g. "/dev/pass0" or "/dev/sr0"
  // Why the last command failed below SCSI (RIP_ERR without sense): the
  // platform's own status ("CAM status 0x0b"). "" after a success.
  char last_error[64];
};

int rip_scsi(struct rip_transport *t, const uint8_t *cdb, size_t cdb_len, enum rip_dir dir, void *buf, size_t len,
             unsigned timeout_ms, struct rip_sense *sense);
void rip_transport_close(struct rip_transport *t);

// Fixed (0x70/0x71) or descriptor (0x72/0x73) sense data to key/ASC/ASCQ.
void rip_sense_decode(const uint8_t *raw, size_t len, struct rip_sense *out);

#endif
