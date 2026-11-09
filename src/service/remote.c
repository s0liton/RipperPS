#include "remote.h"

#include <dirent.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "ripper/common.h"
#include "ripper/log.h"
#include "platform.h"
#include "svc.h"

void remote_join(char *out, size_t n, const char *a, const char *b) {
  size_t al;

  while (*b == '/') {
    b++;
  }
  svc_str_copy(out, n, a);
  al = strlen(out);
  while (al && out[al - 1] == '/') {
    out[--al] = '\0';
  }
  if (al && *b) {
    snprintf(out + al, n - al, "/%s", b);
  } else if (*b) {
    svc_str_copy(out, n, b);
  }
}

int remote_sent(struct remote *r, uint64_t n) {
  r->done += n;
  if (r->progress && r->progress(r->ud, r->done, r->total)) {
    r->stop = 1;
  }
  return r->stop;
}

// Each missing folder along path.
static int mkdir_p(struct remote *r, const char *path) {
  char part[1024];
  size_t i;

  for (i = 0; path[i]; i++) {
    if (path[i] == '/' && i) {
      memcpy(part, path, i);
      part[i] = '\0';
      if (r->ops->mkdir(r, part) != RIP_OK) {
        return RIP_ERR;
      }
    }
    if (i + 1 >= sizeof(part)) {
      return RIP_ERR;
    }
  }
  return path[0] ? r->ops->mkdir(r, path) : RIP_OK;
}

static uint64_t local_size(const char *path) {
  struct stat st;
  DIR *d;
  struct dirent *e;
  uint64_t n = 0;

  if (stat(path, &st) != 0) {
    return 0;
  }
  if (!S_ISDIR(st.st_mode)) {
    return (uint64_t)st.st_size;
  }
  d = opendir(path);
  while (d && (e = readdir(d)) != NULL) {
    char sub[1024];
    if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) {
      continue;
    }
    snprintf(sub, sizeof(sub), "%s/%s", path, e->d_name);
    n += local_size(sub);
  }
  if (d) {
    closedir(d);
  }
  return n;
}

// A local folder's contents into the remote folder dst (made already).
static int put_tree(struct remote *r, const char *src, const char *dst) {
  DIR *d = opendir(src);
  struct dirent *e;
  int rc = RIP_OK;

  if (!d) {
    snprintf(r->err, sizeof(r->err), "can't read %s", src);
    return RIP_ERR;
  }
  while (rc == RIP_OK && !r->stop && (e = readdir(d)) != NULL) {
    char from[1024];
    char to[1024];
    struct stat st;
    if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) {
      continue;
    }
    snprintf(from, sizeof(from), "%s/%s", src, e->d_name);
    remote_join(to, sizeof(to), dst, e->d_name);
    if (stat(from, &st) != 0) {
      continue;
    }
    if (S_ISDIR(st.st_mode)) {
      rc = r->ops->mkdir(r, to);
      if (rc == RIP_OK) {
        rc = put_tree(r, from, to);
      }
    } else {
      rc = r->ops->put(r, from, to, (uint64_t)st.st_size);
    }
  }
  closedir(d);
  return r->stop ? RIP_ECANCEL : rc;
}

// One of the rip's files or folders, under its .part name first.
static int put_item(struct remote *r, const char *local, const char *dir, const char *name) {
  char final[1024];
  char part[1100];
  struct stat st;
  int is_dir = 0;
  int rc;

  remote_join(final, sizeof(final), dir, name);
  snprintf(part, sizeof(part), "%s.part", final);
  if (stat(local, &st) != 0) {
    snprintf(r->err, sizeof(r->err), "%s is missing", local);
    return RIP_ERR;
  }
  if (S_ISDIR(st.st_mode)) {
    // A folder isn't merged into one that's there: say so instead.
    if (r->ops->exists(r, final, &is_dir) == RIP_OK) {
      snprintf(r->err, sizeof(r->err), "%.100s%s%.120s is there already", r->shown, final[0] == '/' ? "" : "/", final);
      return RIP_ERR;
    }
    rc = r->ops->mkdir(r, part);
    if (rc == RIP_OK) {
      rc = put_tree(r, local, part);
    }
  } else {
    rc = r->ops->put(r, local, part, (uint64_t)st.st_size);
    // A file of the same name is replaced, as a local rip would be.
    if (rc == RIP_OK && r->ops->exists(r, final, &is_dir) == RIP_OK && !is_dir) {
      r->ops->remove(r, final);
    }
  }
  if (rc == RIP_OK && r->ops->rename(r, part, final) != RIP_OK) {
    if (!r->err[0]) {
      snprintf(r->err, sizeof(r->err), "couldn't rename %.220s", part);
    }
    rc = RIP_ERR;
  }
  return rc;
}

int remote_deliver(struct remote *r, const struct deliver_req *req, char *where, size_t wn, char *err, size_t en) {
  char dir[1024];
  int64_t room;
  int i;
  int rc = RIP_OK;

  r->progress = req->progress;
  r->ud = req->ud;
  r->done = 0;
  r->total = 0;
  for (i = 0; i < req->count; i++) {
    char local[1024];
    snprintf(local, sizeof(local), "%s/%s", req->dir, req->files[i]);
    r->total += local_size(local);
  }
  remote_join(dir, sizeof(dir), r->root, req->folder ? req->folder : "");
  if (mkdir_p(r, dir) != RIP_OK) {
    snprintf(err, en, "couldn't create %s/%s%s%s", r->shown, dir, r->err[0] ? ": " : "", r->err);
    return RIP_ERR;
  }
  room = r->ops->free_space ? r->ops->free_space(r, dir) : -1;
  if (room >= 0 && (uint64_t)room < r->total) {
    snprintf(err, en, "%s needs %.1f GB, %s has %.1f GB free", req->name, (double)r->total / 1e9, r->shown,
             (double)room / 1e9);
    return RIP_ERR;
  }
  RIP_INFO("sending %d item(s), %.1f MB, to %s%s%s", req->count, (double)r->total / 1e6, r->shown,
           dir[0] == '/' ? "" : "/", dir);
  for (i = 0; rc == RIP_OK && i < req->count; i++) {
    char local[1024];
    snprintf(local, sizeof(local), "%s/%s", req->dir, req->files[i]);
    rc = put_item(r, local, dir, req->files[i]);
  }
  if (r->stop) {
    rc = RIP_ECANCEL;
  }
  if (rc == RIP_OK) {
    char first[1024];
    remote_join(first, sizeof(first), dir, req->count == 1 ? req->files[0] : "");
    snprintf(where, wn, "%s%s%s", r->shown, first[0] == '/' ? "" : "/", first);
  } else if (rc != RIP_ECANCEL) {
    snprintf(err, en, "%s", r->err[0] ? r->err : "the copy failed");
  }
  return rc;
}

int remote_test(struct remote *r, char *msg, size_t n) {
  char probe[1100];
  char local[256];
  int64_t room;
  FILE *f;
  int rc;

  if (mkdir_p(r, r->root) != RIP_OK) {
    snprintf(msg, n, "couldn't create %s/%s%s%s", r->shown, r->root, r->err[0] ? ": " : "", r->err);
    return RIP_ERR;
  }
  // A few bytes from the console, put and taken away again.
  snprintf(local, sizeof(local), "%s/.ripper-test", plat_data_dir());
  f = fopen(local, "w");
  if (!f) {
    snprintf(msg, n, "can't make a test file here");
    return RIP_ERR;
  }
  fputs("ripper\n", f);
  fclose(f);
  remote_join(probe, sizeof(probe), r->root, ".ripper-test");
  rc = r->ops->put(r, local, probe, 7);
  unlink(local);
  if (rc != RIP_OK) {
    snprintf(msg, n, "can't write in %s/%s%s%s", r->shown, r->root, r->err[0] ? ": " : "", r->err);
    return RIP_ERR;
  }
  r->ops->remove(r, probe);
  room = r->ops->free_space ? r->ops->free_space(r, r->root) : -1;
  if (room >= 0) {
    snprintf(msg, n, "Writable, %.1f GB free", (double)room / 1e9);
  } else {
    snprintf(msg, n, "Writable");
  }
  return RIP_OK;
}
