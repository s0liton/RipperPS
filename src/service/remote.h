#ifndef RIPPER_REMOTE_H_
#define RIPPER_REMOTE_H_

// A folder on another machine the rips are copied into: FTP, SMB or NFS each
// give the few operations below, and what's common is done here once: the
// destination's folders made, every file sent as "<name>.part" and renamed
// when whole (a game folder as "<name>.part/" and renamed at the end), the
// progress over all of it, the Test button's check.
//
// Paths given to the operations are '/'-separated and relative to what the
// backend connected to (the share, the export, the FTP login's folder).

#include <stddef.h>
#include <stdint.h>

#include "cJSON.h"
#include "deliver.h"

struct remote;

struct remote_ops {
  // One level; RIP_OK when it is there already.
  int (*mkdir)(struct remote *r, const char *path);
  // RIP_OK with *is_dir set, RIP_ENOENT when there's nothing, RIP_ERR.
  int (*exists)(struct remote *r, const char *path, int *is_dir);
  // A local file to path, replacing it. Calls remote_sent() as it goes.
  int (*put)(struct remote *r, const char *local, const char *path, uint64_t size);
  // Fails when to exists.
  int (*rename)(struct remote *r, const char *from, const char *to);
  // A file.
  int (*remove)(struct remote *r, const char *path);
  // Bytes free under path, -1 when the server doesn't say.
  int64_t (*free_space)(struct remote *r, const char *path);
  void (*close)(struct remote *r);
};

struct remote {
  const struct remote_ops *ops;
  void *ctx;
  char root[512];   // the destination's folder: "Movies", "" for the top
  char shown[512];  // how a path is shown: "smb://nas/media"
  char err[256];    // what went wrong, by the backend
  svc_progress_fn progress;
  void *ud;
  uint64_t done;
  uint64_t total;
  int stop;
};

// Bytes sent; nonzero when the job was cancelled.
int remote_sent(struct remote *r, uint64_t n);

// Copies a rip's files under root/folder. where gets the shown path.
int remote_deliver(struct remote *r, const struct deliver_req *req, char *where, size_t wn, char *err, size_t en);

// Makes root, writes and removes a small file, says what it found.
int remote_test(struct remote *r, char *msg, size_t n);

// "a/b" + "c" with single slashes, either side may be empty.
void remote_join(char *out, size_t n, const char *a, const char *b);

#endif
