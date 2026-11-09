// SMB shares (Windows, Samba, NAS), through libsmb2: straight to the share,
// nothing mounted on the console. "host:port" reaches a server off port 445.

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "deliver.h"
#include "remote.h"
#include "ripper/common.h"
#include "ripper/log.h"

#ifdef RIPPER_HAVE_SMB2

#include <fcntl.h>
#include <stdint.h>
#include <time.h>
#include <unistd.h>

// libsmb2.h uses smb2.h's types and time_t without including them, so these
// stay in this order (and out of the formatter's include sorting).
// clang-format off
#include <smb2/smb2.h>
#include <smb2/libsmb2.h>
// clang-format on

#define CHUNK (1024u * 1024)

struct smb {
  struct smb2_context *c;
  const cJSON *dest;  // to connect again
};

#define SMB(r) (((struct smb *)(r)->ctx)->c)

static struct smb2_context *smb_connect(const cJSON *dest, char *err, size_t en);

// Share paths have no leading slash.
static const char *rel(const char *path) {
  while (*path == '/') {
    path++;
  }
  return path;
}

static int smb_fail(struct remote *r, const char *what, const char *path) {
  snprintf(r->err, sizeof(r->err), "%s %.200s: %s", what, path, smb2_get_error(SMB(r)));
  return RIP_ERR;
}

static int smb_exists(struct remote *r, const char *path, int *is_dir) {
  struct smb2_stat_64 st;

  if (smb2_stat(SMB(r), rel(path), &st) < 0) {
    return RIP_ENOENT;
  }
  *is_dir = st.smb2_type == SMB2_TYPE_DIRECTORY;
  return RIP_OK;
}

static int smb_mkdir(struct remote *r, const char *path) {
  int is_dir = 0;

  // Looked for first: some servers drop the connection over a folder
  // that's there already.
  if (smb_exists(r, path, &is_dir) == RIP_OK) {
    if (is_dir) {
      return RIP_OK;
    }
    snprintf(r->err, sizeof(r->err), "%.200s is a file", path);
    return RIP_ERR;
  }
  return smb2_mkdir(SMB(r), rel(path)) == 0 ? RIP_OK : smb_fail(r, "can't create", path);
}

static int smb_put(struct remote *r, const char *local, const char *path, uint64_t size) {
  struct smb2fh *fh;
  uint8_t *buf;
  uint32_t most = smb2_get_max_write_size(SMB(r));
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
  buf = malloc(CHUNK);
  fh = buf ? smb2_open(SMB(r), rel(path), O_WRONLY | O_CREAT | O_TRUNC) : NULL;
  if (!fh) {
    free(buf);
    close(in);
    return buf ? smb_fail(r, "can't write", path) : RIP_ENOMEM;
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
      uint32_t want = (uint32_t)(got - at) < most ? (uint32_t)(got - at) : most;
      int put = smb2_write(SMB(r), fh, buf + at, want);
      if (put <= 0) {
        rc = smb_fail(r, "write failed on", path);
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
  if (smb2_close(SMB(r), fh) < 0 && rc == RIP_OK) {
    rc = smb_fail(r, "can't finish", path);
  }
  free(buf);
  close(in);
  return rc;
}

static int smb_rename(struct remote *r, const char *from, const char *to) {
  return smb2_rename(SMB(r), rel(from), rel(to)) == 0 ? RIP_OK : smb_fail(r, "can't rename", from);
}

static int smb_remove(struct remote *r, const char *path) {
  return smb2_unlink(SMB(r), rel(path)) == 0 ? RIP_OK : smb_fail(r, "can't remove", path);
}

static void disconnect(struct smb2_context *c) {
  smb2_disconnect_share(c);
  smb2_destroy_context(c);
}

static int64_t smb_free(struct remote *r, const char *path) {
  struct smb *s = r->ctx;
  struct smb2_statvfs vfs;
  char err[256];

  if (smb2_statvfs(s->c, rel(path), &vfs) == 0) {
    return (int64_t)(vfs.f_bavail * vfs.f_bsize);
  }
  // Not every server answers this, and some hang up over it: connect again.
  RIP_INFO("smb: the server doesn't say how much room it has (%s)", smb2_get_error(s->c));
  disconnect(s->c);
  s->c = smb_connect(s->dest, err, sizeof(err));
  if (!s->c) {
    // Every operation after this fails with the reason.
    snprintf(r->err, sizeof(r->err), "%s", err);
    s->c = smb2_init_context();
  }
  return -1;
}

static void smb_close(struct remote *r) {
  struct smb *s = r->ctx;

  if (s) {
    if (s->c) {
      disconnect(s->c);
    }
    free(s);
  }
  r->ctx = NULL;
}

static const struct remote_ops SMB_OPS = {smb_mkdir, smb_exists, smb_put, smb_rename, smb_remove, smb_free, smb_close};

static struct smb2_context *smb_connect(const cJSON *dest, char *err, size_t en) {
  struct smb2_context *smb2;
  const char *host = jstr(dest, "host", "");
  const char *share = jstr(dest, "share", "");
  const char *user = jstr(dest, "user", "");
  const char *domain = jstr(dest, "domain", "");

  while (*share == '/' || *share == '\\') {
    share++;
  }
  if (!host[0] || !share[0]) {
    snprintf(err, en, "the server and the share must be set");
    return NULL;
  }
  smb2 = smb2_init_context();
  if (!smb2) {
    snprintf(err, en, "out of memory");
    return NULL;
  }
  smb2_set_security_mode(smb2, SMB2_NEGOTIATE_SIGNING_ENABLED);
  smb2_set_timeout(smb2, 30);
  smb2_set_password(smb2, jstr(dest, "password", ""));
  if (domain[0]) {
    smb2_set_domain(smb2, domain);
  }
  if (smb2_connect_share(smb2, host, share, user[0] ? user : "Guest") < 0) {
    snprintf(err, en, "can't connect to \\\\%s\\%s: %s", host, share, smb2_get_error(smb2));
    smb2_destroy_context(smb2);
    return NULL;
  }
  return smb2;
}

static int smb_open(const cJSON *dest, struct remote *r, char *err, size_t en) {
  struct smb *s;
  const char *share = jstr(dest, "share", "");

  memset(r, 0, sizeof(*r));
  while (*share == '/' || *share == '\\') {
    share++;
  }
  s = calloc(1, sizeof(*s));
  if (!s) {
    snprintf(err, en, "out of memory");
    return RIP_ENOMEM;
  }
  s->dest = dest;
  s->c = smb_connect(dest, err, en);
  if (!s->c) {
    free(s);
    return RIP_ERR;
  }
  r->ops = &SMB_OPS;
  r->ctx = s;
  svc_str_copy(r->root, sizeof(r->root), rel(jstr(dest, "path", "")));
  snprintf(r->shown, sizeof(r->shown), "smb://%s/%s", jstr(dest, "host", ""), share);
  return RIP_OK;
}

int deliver_smb(const struct deliver_req *req, char *where, size_t wn, char *err, size_t en) {
  struct remote r;
  int rc = smb_open(req->dest, &r, err, en);

  if (rc != RIP_OK) {
    return rc;
  }
  rc = remote_deliver(&r, req, where, wn, err, en);
  r.ops->close(&r);
  return rc;
}

int deliver_smb_test(const cJSON *dest, char *msg, size_t n) {
  struct remote r;
  int rc = smb_open(dest, &r, msg, n);

  if (rc != RIP_OK) {
    return rc;
  }
  rc = remote_test(&r, msg, n);
  r.ops->close(&r);
  return rc;
}

#else

int deliver_smb(const struct deliver_req *r, char *where, size_t wn, char *err, size_t en) {
  (void)r;
  (void)where;
  (void)wn;
  snprintf(err, en, "SMB destinations aren't in this build (no libsmb2)");
  return RIP_ENOTSUP;
}

int deliver_smb_test(const cJSON *dest, char *msg, size_t n) {
  (void)dest;
  snprintf(msg, n, "SMB destinations aren't in this build (no libsmb2)");
  return RIP_ENOTSUP;
}

#endif
