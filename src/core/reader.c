#include "ripper/reader.h"

#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "ripper/common.h"
#include "ripper/log.h"
#include "ripper/mmc.h"
#include "ripper/util.h"

/* Drive via READ(12) */

struct mmc_ctx {
  struct rip_transport *t;
  uint32_t block_len;
};

static int mmc_read(void *ctx, uint64_t lba, uint32_t count, void *buf) {
  struct mmc_ctx *m = ctx;
  struct rip_sense sense;
  int rc = mmc_read12(m->t, (uint32_t)lba, count, buf, m->block_len, &sense);

  if (rc == RIP_ESENSE) {
    RIP_DEBUG("READ(12) lba %llu x%u: sense %02x/%02x/%02x", (unsigned long long)lba, count, sense.key, sense.asc,
              sense.ascq);
  }
  return rc;
}

static void mmc_close(void *ctx) {
  free(ctx);
}

static const struct rip_reader_ops mmc_ops = {mmc_read, mmc_close};

int rip_reader_open_mmc(struct rip_reader *r, struct rip_transport *t) {
  struct mmc_ctx *m;
  uint32_t last_lba = 0;
  uint32_t block_len = 0;
  int rc;

  memset(r, 0, sizeof(*r));
  rc = mmc_read_capacity(t, &last_lba, &block_len);
  if (rc != RIP_OK) {
    return rc;
  }
  if (block_len == 0 || block_len > 4096) {
    return RIP_ERR;
  }
  m = calloc(1, sizeof(*m));
  if (!m) {
    return RIP_ENOMEM;
  }
  m->t = t;
  m->block_len = block_len;
  r->ops = &mmc_ops;
  r->ctx = m;
  r->sector_size = block_len;
  r->sector_count = (uint64_t)last_lba + 1;
  // 64 KiB: the PS5's CAM pass-through refuses 128 KiB transfers (checked on a
  // console); larger requests buy nothing on a drive that streams anyway.
  r->max_sectors = 32;
  return RIP_OK;
}

/* File descriptor (block device or image) */

struct fd_ctx {
  int fd;
  int owns;
  uint32_t sector_size;
};

static int fd_read(void *ctx, uint64_t lba, uint32_t count, void *buf) {
  struct fd_ctx *f = ctx;

  return rip_seek_read(f->fd, lba * f->sector_size, buf, (size_t)count * f->sector_size) == 0 ? RIP_OK : RIP_ERR;
}

static void fd_close(void *ctx) {
  struct fd_ctx *f = ctx;

  if (f->owns) {
    close(f->fd);
  }
  free(f);
}

static const struct rip_reader_ops fd_ops = {fd_read, fd_close};

int rip_reader_open_fd(struct rip_reader *r, int fd, uint32_t sector_size, uint64_t size_bytes, int owns_fd) {
  struct fd_ctx *f;

  memset(r, 0, sizeof(*r));
  if (fd < 0 || sector_size == 0) {
    return RIP_ERR;
  }
  f = calloc(1, sizeof(*f));
  if (!f) {
    return RIP_ENOMEM;
  }
  f->fd = fd;
  f->owns = owns_fd;
  f->sector_size = sector_size;
  r->ops = &fd_ops;
  r->ctx = f;
  r->sector_size = sector_size;
  r->sector_count = size_bytes / sector_size;
  r->max_sectors = (128 * 1024) / sector_size;
  return RIP_OK;
}

int rip_reader_read(struct rip_reader *r, uint64_t lba, uint32_t count, void *buf) {
  if (!r->ops || count == 0 || lba + count > r->sector_count) {
    return RIP_ERR;
  }
  return r->ops->read(r->ctx, lba, count, buf);
}

void rip_reader_close(struct rip_reader *r) {
  if (r->ops && r->ops->close) {
    r->ops->close(r->ctx);
  }
  memset(r, 0, sizeof(*r));
}
