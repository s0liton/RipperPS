#include "deliver.h"

#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "ripper/common.h"
#include "ripper/util.h"

int deliver_is_local(const cJSON *dest) {
  const char *type = jstr(dest, "type", "");
  return !strcmp(type, "local") || !strcmp(type, "usb");
}

// A USB drive's folder is only there while the drive is: without this, its
// folder would be made on the console under /mnt instead.
static int usb_missing(const cJSON *dest, char *msg, size_t n) {
  const char *path = jstr(dest, "path", "");
  char root[16];
  size_t l = 0;

  if (strcmp(jstr(dest, "type", ""), "usb") != 0) {
    return 0;
  }
  if (!strncmp(path, "/mnt/usb", 8)) {
    for (l = 8; path[l] >= '0' && path[l] <= '9' && l < sizeof(root) - 1; l++) {
    }
  }
  if (l <= 8 || (path[l] != '/' && path[l] != '\0')) {
    snprintf(msg, n, "pick a folder on a USB drive");
    return 1;
  }
  memcpy(root, path, l);
  root[l] = '\0';
  if (!svc_is_mount(root)) {
    snprintf(msg, n, "the USB drive (%s) isn't plugged in", root);
    return 1;
  }
  return 0;
}

static int local_test(const cJSON *dest, char *msg, size_t n) {
  const char *path = jstr(dest, "path", "");
  char probe[SVC_PATH];
  int64_t free_bytes;
  FILE *f;

  if (path[0] != '/') {
    snprintf(msg, n, "the folder must be a full path");
    return RIP_ERR;
  }
  if (usb_missing(dest, msg, n)) {
    return RIP_ERR;
  }
  if (rip_mkdir_p(path) != 0) {
    snprintf(msg, n, "can't create %s", path);
    return RIP_ERR;
  }
  svc_path_join(probe, sizeof(probe), path, ".ripper-test");
  f = fopen(probe, "w");
  if (!f) {
    snprintf(msg, n, "can't write in %s", path);
    return RIP_ERR;
  }
  fclose(f);
  unlink(probe);
  free_bytes = svc_free_space(path);
  if (free_bytes >= 0) {
    snprintf(msg, n, "Writable, %.1f GB free", (double)free_bytes / 1e9);
  } else {
    snprintf(msg, n, "Writable");
  }
  return RIP_OK;
}

// A local destination: the rip moves from staging into the folder. On the same
// filesystem that's a rename; to another (a USB drive) it's a copy, with
// progress. A name already there gets " (2)", " (3)"... rather than replacing it.

struct local_move {
  svc_progress_fn fn;
  void *ud;
  uint64_t base;
  uint64_t total;
};

static uint64_t tree_size(const char *path) {
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
    char child[SVC_PATH];
    if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) {
      continue;
    }
    svc_path_join(child, sizeof(child), path, e->d_name);
    n += tree_size(child);
  }
  if (d) {
    closedir(d);
  }
  return n;
}

static int local_progress(void *ud, uint64_t done, uint64_t total) {
  struct local_move *m = ud;
  (void)total;
  return m->fn ? m->fn(m->ud, m->base + done, m->total) : 0;
}

static int move_tree(const char *from, const char *to, struct local_move *m) {
  uint64_t size = tree_size(from);
  struct stat st;

  if (rename(from, to) == 0) {
    m->base += size;
    return local_progress(m, 0, 0) ? -2 : 0;
  }
  if (errno != EXDEV || stat(from, &st) != 0) {
    return -1;
  }
  if (S_ISDIR(st.st_mode)) {
    DIR *d = opendir(from);
    struct dirent *e;
    int rc = rip_mkdir_p(to) == 0 && d ? 0 : -1;
    while (rc == 0 && (e = readdir(d)) != NULL) {
      char a[SVC_PATH];
      char b[SVC_PATH];
      if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) {
        continue;
      }
      svc_path_join(a, sizeof(a), from, e->d_name);
      svc_path_join(b, sizeof(b), to, e->d_name);
      rc = move_tree(a, b, m);
    }
    if (d) {
      closedir(d);
    }
    if (rc == 0) {
      rmdir(from);
    }
    return rc;
  }
  if (svc_copy_file(from, to, local_progress, m) != 0) {
    unlink(to);
    return m->fn && m->fn(m->ud, m->base, m->total) ? -2 : -1;
  }
  m->base += size;
  unlink(from);
  return 0;
}

// dir/name, or dir/stem (2).ext and so on when that's taken.
static void free_name(const char *dir, const char *name, char *out, size_t n) {
  const char *dot = strrchr(name, '.');
  struct stat st;
  int i;

  svc_path_join(out, n, dir, name);
  if (!dot || dot == name) {
    dot = name + strlen(name);
  }
  for (i = 2; stat(out, &st) == 0 && i < 1000; i++) {
    char alt[512];
    snprintf(alt, sizeof(alt), "%.*s (%d)%s", (int)(dot - name), name, i, dot);
    svc_path_join(out, n, dir, alt);
  }
}

static int deliver_local(const struct deliver_req *r, char *where, size_t wn, char *err, size_t en) {
  struct local_move m = {r->progress, r->ud, 0, 0};
  char dir[SVC_PATH];
  int i;

  if (usb_missing(r->dest, err, en)) {
    return RIP_ERR;
  }
  svc_path_join(dir, sizeof(dir), jstr(r->dest, "path", ""), r->folder ? r->folder : "");
  if (rip_mkdir_p(dir) != 0) {
    snprintf(err, en, "can't create %s", dir);
    return RIP_ERR;
  }
  for (i = 0; i < r->count; i++) {
    char from[SVC_PATH];
    svc_path_join(from, sizeof(from), r->dir, r->files[i]);
    m.total += tree_size(from);
  }
  for (i = 0; i < r->count; i++) {
    char from[SVC_PATH];
    char to[SVC_PATH];
    int rc;
    svc_path_join(from, sizeof(from), r->dir, r->files[i]);
    free_name(dir, r->files[i], to, sizeof(to));
    rc = move_tree(from, to, &m);
    if (rc == -2) {
      snprintf(err, en, "cancelled");
      return RIP_ECANCEL;
    }
    if (rc != 0) {
      snprintf(err, en, "can't move %s to %s: %s", r->files[i], dir, strerror(errno));
      return RIP_ERR;
    }
    if (i == 0) {
      svc_str_copy(where, wn, r->count == 1 ? to : dir);
    }
  }
  return RIP_OK;
}

int deliver_test(const cJSON *dest, char *msg, size_t n) {
  const char *type = jstr(dest, "type", "");

  msg[0] = '\0';
  if (deliver_is_local(dest)) {
    return local_test(dest, msg, n);
  }
  if (!strcmp(type, "ftp")) {
    return deliver_ftp_test(dest, msg, n);
  }
  if (!strcmp(type, "smb")) {
    return deliver_smb_test(dest, msg, n);
  }
  if (!strcmp(type, "nfs")) {
    return deliver_nfs_test(dest, msg, n);
  }
  if (!strcmp(type, "romm")) {
    return deliver_romm_test(dest, msg, n);
  }
  snprintf(msg, n, "unknown destination type '%s'", type);
  return RIP_ERR;
}

int deliver_run(const struct deliver_req *r, char *where, size_t wn, char *err, size_t en) {
  const char *type = jstr(r->dest, "type", "");

  where[0] = '\0';
  err[0] = '\0';
  if (deliver_is_local(r->dest)) {
    return deliver_local(r, where, wn, err, en);
  }
  if (!strcmp(type, "ftp")) {
    return deliver_ftp(r, where, wn, err, en);
  }
  if (!strcmp(type, "smb")) {
    return deliver_smb(r, where, wn, err, en);
  }
  if (!strcmp(type, "nfs")) {
    return deliver_nfs(r, where, wn, err, en);
  }
  if (!strcmp(type, "romm")) {
    return deliver_romm(r, where, wn, err, en);
  }
  // Local rips are written in place; nothing gets here for them.
  snprintf(err, en, "unknown destination type '%s'", type);
  return RIP_ERR;
}
