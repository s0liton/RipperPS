#ifndef RIPPER_READER_H_
#define RIPPER_READER_H_

#include <stdint.h>

#include "ripper/transport.h"

// A sector source: the drive through READ(12), a block device such as the
// PS5's /dev/cd0, or an image file. Dumping and parsing only see this.

struct rip_reader_ops {
  int (*read)(void *ctx, uint64_t lba, uint32_t count, void *buf);
  void (*close)(void *ctx);
};

struct rip_reader {
  const struct rip_reader_ops *ops;
  void *ctx;
  uint32_t sector_size;
  uint64_t sector_count;
  // Largest count a single read accepts (kernel transfer limit).
  uint32_t max_sectors;
};

// Sizes the medium with READ CAPACITY and reads with READ(12).
int rip_reader_open_mmc(struct rip_reader *r, struct rip_transport *t);
// A block device or an image file. size_bytes is the medium/image size.
// The reader closes fd only when owns_fd is set.
int rip_reader_open_fd(struct rip_reader *r, int fd, uint32_t sector_size, uint64_t size_bytes, int owns_fd);

int rip_reader_read(struct rip_reader *r, uint64_t lba, uint32_t count, void *buf);
void rip_reader_close(struct rip_reader *r);

#endif
