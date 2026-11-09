// NFS exports, through libnfs: straight to the export, nothing mounted on the
// console. Files are written as the user and group ids set for the
// destination (the console has no users the server would know).

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "deliver.h"
#include "remote.h"
#include "ripper/common.h"
#include "ripper/log.h"

#ifdef RIPPER_HAVE_NFS

#include <fcntl.h>
#include <nfsc/libnfs.h>
#include <sys/stat.h>
#include <unistd.h>

#define CHUNK (1024u * 1024)

// Export paths start with a slash.
static void abs_path(const char *path, char *out, size_t n) {
  snprintf(out, n, "%s%s", path[0] == '/' ? "" : "/", path);
}

static int nfs_fail(struct remote *r, const char *what, const char *path) {
  snprintf(r->err, sizeof(r->err), "%s %.200s: %s", what, path, nfs_get_error(r->ctx));
  return RIP_ERR;
}

static int n_exists(struct remote *r, const char *path, int *is_dir) {
  struct nfs_stat_64 st;
  char p[1100];

  abs_path(path, p, sizeof(p));
  if (nfs_stat64(r->ctx, p, &st) < 0) {
    return RIP_ENOENT;
  }
  *is_dir = S_ISDIR(st.nfs_mode);
  return RIP_OK;
}

static int n_mkdir(struct remote *r, const char *path) {
  char p[1100];
  int is_dir = 0;

  abs_path(path, p, sizeof(p));
  if (n_exists(r, path, &is_dir) == RIP_OK) {
    if (is_dir) {
      return RIP_OK;
    }
    snprintf(r->err, sizeof(r->err), "%.200s is a file", path);
    return RIP_ERR;
  }
  return nfs_mkdir(r->ctx, p) == 0 ? RIP_OK : nfs_fail(r, "can't create", path);
}

static int n_put(struct remote *r, const char *local, const char *path, uint64_t size) {
  struct nfsfh *fh = NULL;
  uint8_t *buf;
  uint64_t most = nfs_get_writemax(r->ctx);
  char p[1100];
  int in = open(local, O_RDONLY);
  int rc = RIP_OK;

  (void)size;
  if (in < 0) {
    snprintf(r->err, sizeof(r->err), "can't read %s", local);
    return RIP_ERR;
  }
  if (!most || most > CHUNK) {
    most = CHUNK;
  }
  abs_path(path, p, sizeof(p));
  buf = malloc(CHUNK);
  if (!buf || nfs_open2(r->ctx, p, O_WRONLY | O_CREAT | O_TRUNC, 0644, &fh) < 0) {
    free(buf);
    close(in);
    return buf ? nfs_fail(r, "can't write", path) : RIP_ENOMEM;
  }
  for (;;) {
    long got = read(in, buf, CHUNK);
    long at = 0;
    if (got < 0) {
      snprintf(r->err, sizeof(r->err), "can't read %s", local);
      rc = RIP_ERR;
      break;
    }
    if (got == 0) {
      break;
    }
    while (at < got) {
      uint64_t want = (uint64_t)(got - at) < most ? (uint64_t)(got - at) : most;
      int put = nfs_write(r->ctx, fh, want, buf + at);
      if (put <= 0) {
        rc = nfs_fail(r, "write failed on", path);
        break;
      }
      at += put;
    }
    if (rc != RIP_OK) {
      break;
    }
    if (remote_sent(r, (uint64_t)got)) {
      rc = RIP_ECANCEL;
      break;
    }
  }
  if (nfs_close(r->ctx, fh) < 0 && rc == RIP_OK) {
    rc = nfs_fail(r, "can't finish", path);
  }
  free(buf);
  close(in);
  return rc;
}

static int n_rename(struct remote *r, const char *from, const char *to) {
  char a[1100];
  char b[1100];

  abs_path(from, a, sizeof(a));
  abs_path(to, b, sizeof(b));
  return nfs_rename(r->ctx, a, b) == 0 ? RIP_OK : nfs_fail(r, "can't rename", from);
}

static int n_remove(struct remote *r, const char *path) {
  char p[1100];

  abs_path(path, p, sizeof(p));
  return nfs_unlink(r->ctx, p) == 0 ? RIP_OK : nfs_fail(r, "can't remove", path);
}

static int64_t n_free(struct remote *r, const char *path) {
  struct nfs_statvfs_64 vfs;
  char p[1100];

  abs_path(path, p, sizeof(p));
  if (nfs_statvfs64(r->ctx, p, &vfs) < 0) {
    return -1;
  }
  return (int64_t)(vfs.f_bavail * (vfs.f_frsize ? vfs.f_frsize : vfs.f_bsize));
}

static void n_close(struct remote *r) {
  if (r->ctx) {
    nfs_umount(r->ctx);
    nfs_destroy_context(r->ctx);
  }
  r->ctx = NULL;
}

static const struct remote_ops NFS_OPS = {n_mkdir, n_exists, n_put, n_rename, n_remove, n_free, n_close};

static int nfs_open_dest(const cJSON *dest, struct remote *r, char *err, size_t en) {
  struct nfs_context *nfs;
  const char *host = jstr(dest, "host", "");
  const char *export_path = jstr(dest, "export", "");

  memset(r, 0, sizeof(*r));
  if (!host[0] || !export_path[0]) {
    snprintf(err, en, "the server and the export must be set");
    return RIP_ERR;
  }
  nfs = nfs_init_context();
  if (!nfs) {
    snprintf(err, en, "out of memory");
    return RIP_ENOMEM;
  }
  nfs_set_uid(nfs, jint(dest, "uid", 0));
  nfs_set_gid(nfs, jint(dest, "gid", 0));
  nfs_set_timeout(nfs, 30000);
  if (nfs_mount(nfs, host, export_path) < 0) {
    snprintf(err, en, "can't mount %s:%s: %s", host, export_path, nfs_get_error(nfs));
    nfs_destroy_context(nfs);
    return RIP_ERR;
  }
  r->ops = &NFS_OPS;
  r->ctx = nfs;
  svc_str_copy(r->root, sizeof(r->root), jstr(dest, "path", ""));
  snprintf(r->shown, sizeof(r->shown), "nfs://%s%s%s", host, export_path[0] == '/' ? "" : "/", export_path);
  return RIP_OK;
}

int deliver_nfs(const struct deliver_req *req, char *where, size_t wn, char *err, size_t en) {
  struct remote r;
  int rc = nfs_open_dest(req->dest, &r, err, en);

  if (rc != RIP_OK) {
    return rc;
  }
  rc = remote_deliver(&r, req, where, wn, err, en);
  r.ops->close(&r);
  return rc;
}

int deliver_nfs_test(const cJSON *dest, char *msg, size_t n) {
  struct remote r;
  int rc = nfs_open_dest(dest, &r, msg, n);

  if (rc != RIP_OK) {
    return rc;
  }
  rc = remote_test(&r, msg, n);
  r.ops->close(&r);
  return rc;
}

#else

int deliver_nfs(const struct deliver_req *r, char *where, size_t wn, char *err, size_t en) {
  (void)r;
  (void)where;
  (void)wn;
  snprintf(err, en, "NFS destinations aren't in this build (no libnfs)");
  return RIP_ENOTSUP;
}

int deliver_nfs_test(const cJSON *dest, char *msg, size_t n) {
  (void)dest;
  snprintf(msg, n, "NFS destinations aren't in this build (no libnfs)");
  return RIP_ENOTSUP;
}

#endif
