// FTP and FTPS (explicit TLS) destinations, through libcurl. One connection
// for the whole delivery: curl keeps it open between requests.

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "deliver.h"
#include "remote.h"
#include "ripper/common.h"
#include "ripper/log.h"

#ifdef RIPPER_HAVE_CURL

#include <curl/curl.h>
#include <pthread.h>

struct ftp {
  CURL *curl;
  char base[300];  // ftp://host:port
  char user[128];
  char password[128];
  int tls;
  uint64_t reported;  // bytes of the current upload already passed to remote_sent
  struct remote *r;
};

static pthread_once_t g_once = PTHREAD_ONCE_INIT;

static void global_init(void) {
  curl_global_init(CURL_GLOBAL_DEFAULT);
}

// A path into the URL: every component escaped, the slashes kept. A path
// from the server's top keeps its leading slash ("//rips" in the URL).
static void url_for(struct ftp *f, const char *path, int dir, char *out, size_t n) {
  size_t at = (size_t)snprintf(out, n, "%s/", f->base);
  const char *p = path;

  if (*p == '/') {
    at += (size_t)snprintf(out + at, n > at ? n - at : 0, "/");
    p++;
  }
  while (*p && at < n) {
    const char *slash = strchr(p, '/');
    size_t len = slash ? (size_t)(slash - p) : strlen(p);
    char *part = curl_easy_escape(f->curl, p, (int)len);
    at += (size_t)snprintf(out + at, n > at ? n - at : 0, "%s%s", part ? part : "", slash ? "/" : "");
    curl_free(part);
    p += len + (slash ? 1 : 0);
  }
  if (dir && at < n && at && out[at - 1] != '/') {
    snprintf(out + at, n - at, "/");
  }
}

static size_t discard_cb(char *buf, size_t size, size_t n, void *ud) {
  (void)buf;
  (void)ud;
  return size * n;
}

static void setup(struct ftp *f) {
  curl_easy_reset(f->curl);
  curl_easy_setopt(f->curl, CURLOPT_WRITEFUNCTION, discard_cb);
  curl_easy_setopt(f->curl, CURLOPT_HEADERFUNCTION, discard_cb);
  curl_easy_setopt(f->curl, CURLOPT_USERNAME, f->user[0] ? f->user : "anonymous");
  curl_easy_setopt(f->curl, CURLOPT_PASSWORD, f->password);
  curl_easy_setopt(f->curl, CURLOPT_CONNECTTIMEOUT, 10L);
  curl_easy_setopt(f->curl, CURLOPT_LOW_SPEED_LIMIT, 1L);
  curl_easy_setopt(f->curl, CURLOPT_LOW_SPEED_TIME, 60L);
  curl_easy_setopt(f->curl, CURLOPT_NOSIGNAL, 1L);
  curl_easy_setopt(f->curl, CURLOPT_FTP_FILEMETHOD, (long)CURLFTPMETHOD_NOCWD);
  if (f->tls) {
    // NAS FTPS certificates are self-signed nearly always; the link is
    // encrypted, the server not vouched for.
    curl_easy_setopt(f->curl, CURLOPT_USE_SSL, (long)CURLUSESSL_ALL);
    curl_easy_setopt(f->curl, CURLOPT_SSL_VERIFYPEER, 0L);
    curl_easy_setopt(f->curl, CURLOPT_SSL_VERIFYHOST, 0L);
  }
}

static int fail(struct ftp *f, CURLcode c, const char *what) {
  long code = 0;

  curl_easy_getinfo(f->curl, CURLINFO_RESPONSE_CODE, &code);
  if (code) {
    snprintf(f->r->err, sizeof(f->r->err), "%s: %s (FTP %ld)", what, curl_easy_strerror(c), code);
  } else {
    snprintf(f->r->err, sizeof(f->r->err), "%s: %s", what, curl_easy_strerror(c));
  }
  return c == CURLE_ABORTED_BY_CALLBACK ? RIP_ECANCEL : RIP_ERR;
}

// Commands sent on their own, from the login folder.
static int quote(struct ftp *f, const char *cmd1, const char *cmd2) {
  struct curl_slist *list = NULL;
  char url[400];
  CURLcode c;

  setup(f);
  snprintf(url, sizeof(url), "%s/", f->base);
  list = curl_slist_append(list, cmd1);
  if (cmd2) {
    list = curl_slist_append(list, cmd2);
  }
  curl_easy_setopt(f->curl, CURLOPT_URL, url);
  curl_easy_setopt(f->curl, CURLOPT_NOBODY, 1L);
  curl_easy_setopt(f->curl, CURLOPT_QUOTE, list);
  c = curl_easy_perform(f->curl);
  curl_slist_free_all(list);
  return c == CURLE_OK ? RIP_OK : fail(f, c, cmd1);
}

static int ftp_exists(struct remote *r, const char *path, int *is_dir) {
  struct ftp *f = r->ctx;
  char url[1400];
  CURLcode c;

  // A folder: can it be changed into?
  setup(f);
  url_for(f, path, 1, url, sizeof(url));
  curl_easy_setopt(f->curl, CURLOPT_URL, url);
  curl_easy_setopt(f->curl, CURLOPT_FTP_FILEMETHOD, (long)CURLFTPMETHOD_SINGLECWD);
  curl_easy_setopt(f->curl, CURLOPT_NOBODY, 1L);
  if (curl_easy_perform(f->curl) == CURLE_OK) {
    *is_dir = 1;
    return RIP_OK;
  }
  // A file: does it have a size?
  setup(f);
  url_for(f, path, 0, url, sizeof(url));
  curl_easy_setopt(f->curl, CURLOPT_URL, url);
  curl_easy_setopt(f->curl, CURLOPT_NOBODY, 1L);
  c = curl_easy_perform(f->curl);
  if (c == CURLE_OK) {
    *is_dir = 0;
    return RIP_OK;
  }
  return c == CURLE_REMOTE_FILE_NOT_FOUND || c == CURLE_FTP_COULDNT_RETR_FILE || c == CURLE_REMOTE_ACCESS_DENIED
             ? RIP_ENOENT
             : fail(f, c, "can't look");
}

static int ftp_mkdir(struct remote *r, const char *path) {
  struct ftp *f = r->ctx;
  char cmd[1100];
  int is_dir = 0;

  snprintf(cmd, sizeof(cmd), "MKD %s", path);
  if (quote(f, cmd, NULL) == RIP_OK) {
    return RIP_OK;
  }
  if (ftp_exists(r, path, &is_dir) == RIP_OK && is_dir) {
    r->err[0] = '\0';
    return RIP_OK;
  }
  return RIP_ERR;
}

static size_t read_cb(char *buf, size_t size, size_t n, void *ud) {
  return fread(buf, size, n, ud);
}

static int xfer_cb(void *ud, curl_off_t dltotal, curl_off_t dlnow, curl_off_t ultotal, curl_off_t ulnow) {
  struct ftp *f = ud;
  (void)dltotal;
  (void)dlnow;
  (void)ultotal;
  if ((uint64_t)ulnow > f->reported) {
    uint64_t step = (uint64_t)ulnow - f->reported;
    f->reported = (uint64_t)ulnow;
    return remote_sent(f->r, step);
  }
  return f->r->stop;
}

static int ftp_put(struct remote *r, const char *local, const char *path, uint64_t size) {
  struct ftp *f = r->ctx;
  char url[1400];
  FILE *in = fopen(local, "rb");
  CURLcode c;

  if (!in) {
    snprintf(r->err, sizeof(r->err), "can't read %s", local);
    return RIP_ERR;
  }
  setup(f);
  url_for(f, path, 0, url, sizeof(url));
  f->reported = 0;
  curl_easy_setopt(f->curl, CURLOPT_URL, url);
  curl_easy_setopt(f->curl, CURLOPT_UPLOAD, 1L);
  curl_easy_setopt(f->curl, CURLOPT_READFUNCTION, read_cb);
  curl_easy_setopt(f->curl, CURLOPT_READDATA, in);
  curl_easy_setopt(f->curl, CURLOPT_INFILESIZE_LARGE, (curl_off_t)size);
  curl_easy_setopt(f->curl, CURLOPT_NOPROGRESS, 0L);
  curl_easy_setopt(f->curl, CURLOPT_XFERINFOFUNCTION, xfer_cb);
  curl_easy_setopt(f->curl, CURLOPT_XFERINFODATA, f);
  curl_easy_setopt(f->curl, CURLOPT_UPLOAD_BUFFERSIZE, 512L * 1024);
  c = curl_easy_perform(f->curl);
  fclose(in);
  if (c != CURLE_OK) {
    return fail(f, c, path);
  }
  if (f->reported < size) {
    remote_sent(r, size - f->reported);
  }
  return RIP_OK;
}

static int ftp_rename(struct remote *r, const char *from, const char *to) {
  char a[1100];
  char b[1100];

  snprintf(a, sizeof(a), "RNFR %s", from);
  snprintf(b, sizeof(b), "RNTO %s", to);
  return quote(r->ctx, a, b);
}

static int ftp_remove(struct remote *r, const char *path) {
  char cmd[1100];

  snprintf(cmd, sizeof(cmd), "DELE %s", path);
  return quote(r->ctx, cmd, NULL);
}

static void ftp_close(struct remote *r) {
  struct ftp *f = r->ctx;

  if (f) {
    curl_easy_cleanup(f->curl);
    free(f);
  }
  r->ctx = NULL;
}

static const struct remote_ops FTP_OPS = {ftp_mkdir, ftp_exists, ftp_put, ftp_rename, ftp_remove, NULL, ftp_close};

static int ftp_open(const cJSON *dest, struct remote *r, char *err, size_t en) {
  struct ftp *f;
  const char *host = jstr(dest, "host", "");
  int port = jint(dest, "port", 21);

  memset(r, 0, sizeof(*r));
  if (!host[0]) {
    snprintf(err, en, "no server set");
    return RIP_ERR;
  }
  pthread_once(&g_once, global_init);
  f = calloc(1, sizeof(*f));
  if (!f || !(f->curl = curl_easy_init())) {
    free(f);
    snprintf(err, en, "out of memory");
    return RIP_ENOMEM;
  }
  f->r = r;
  f->tls = jbool(dest, "tls", 0);
  snprintf(f->base, sizeof(f->base), "ftp://%s:%d", host, port > 0 ? port : 21);
  svc_str_copy(f->user, sizeof(f->user), jstr(dest, "user", ""));
  svc_str_copy(f->password, sizeof(f->password), jstr(dest, "password", ""));
  r->ops = &FTP_OPS;
  r->ctx = f;
  svc_str_copy(r->root, sizeof(r->root), jstr(dest, "path", ""));
  if (port > 0 && port != 21) {
    snprintf(r->shown, sizeof(r->shown), "%s://%s:%d", f->tls ? "ftps" : "ftp", host, port);
  } else {
    snprintf(r->shown, sizeof(r->shown), "%s://%s", f->tls ? "ftps" : "ftp", host);
  }
  return RIP_OK;
}

int deliver_ftp(const struct deliver_req *req, char *where, size_t wn, char *err, size_t en) {
  struct remote r;
  int rc = ftp_open(req->dest, &r, err, en);

  if (rc != RIP_OK) {
    return rc;
  }
  rc = remote_deliver(&r, req, where, wn, err, en);
  r.ops->close(&r);
  return rc;
}

int deliver_ftp_test(const cJSON *dest, char *msg, size_t n) {
  struct remote r;
  int rc = ftp_open(dest, &r, msg, n);

  if (rc != RIP_OK) {
    return rc;
  }
  rc = remote_test(&r, msg, n);
  r.ops->close(&r);
  return rc;
}

#else

int deliver_ftp(const struct deliver_req *r, char *where, size_t wn, char *err, size_t en) {
  (void)r;
  (void)where;
  (void)wn;
  snprintf(err, en, "FTP destinations aren't in this build (no libcurl)");
  return RIP_ENOTSUP;
}

int deliver_ftp_test(const cJSON *dest, char *msg, size_t n) {
  (void)dest;
  snprintf(msg, n, "FTP destinations aren't in this build (no libcurl)");
  return RIP_ENOTSUP;
}

#endif
