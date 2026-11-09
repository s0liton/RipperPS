#include "ripper/pkg.h"

#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "ripper/common.h"
#include "ripper/log.h"
#include "ripper/util.h"

#define HEADER_BYTES 0x80
#define ENTRY_BYTES 32
#define MAX_ENTRIES 4096
#define MAX_ENTRY_BYTES (64u << 20)

static uint64_t be64(const uint8_t *p) {
  return ((uint64_t)rip_be32(p) << 32) | rip_be32(p + 4);
}

int pkg_open(struct pkg *p, int fd) {
  uint8_t h[HEADER_BYTES];
  uint32_t table;
  uint8_t *raw;
  const struct pkg_entry *names = NULL;
  uint8_t *name_table = NULL;
  size_t name_len = 0;
  unsigned i;

  memset(p, 0, sizeof(*p));
  p->fd = fd;
  if (rip_seek_read(fd, 0, h, sizeof(h)) != 0 || rip_be32(h) != PKG_MAGIC) {
    return RIP_ENOENT;
  }
  p->type = rip_be32(h + 0x04);
  p->count = rip_be32(h + 0x10);
  table = rip_be32(h + 0x18);
  p->body_offset = be64(h + 0x20);
  p->body_size = be64(h + 0x28);
  if (p->count == 0 || p->count > MAX_ENTRIES) {
    return RIP_ERR;
  }
  raw = malloc((size_t)p->count * ENTRY_BYTES);
  p->entries = calloc(p->count, sizeof(*p->entries));
  if (!raw || !p->entries) {
    free(raw);
    pkg_close(p);
    return RIP_ENOMEM;
  }
  if (rip_seek_read(fd, table, raw, (size_t)p->count * ENTRY_BYTES) != 0) {
    free(raw);
    pkg_close(p);
    return RIP_ERR;
  }
  for (i = 0; i < p->count; i++) {
    const uint8_t *e = raw + (size_t)i * ENTRY_BYTES;
    p->entries[i].id = rip_be32(e);
    p->entries[i].name_offset = rip_be32(e + 4);
    p->entries[i].flags1 = rip_be32(e + 8);
    p->entries[i].offset = rip_be32(e + 16);
    p->entries[i].size = rip_be32(e + 20);
  }
  free(raw);
  // Names come from the name table entry: NUL-terminated, by offset.
  names = pkg_find(p, PKG_ENTRY_NAMES);
  if (names && !pkg_entry_encrypted(names) && pkg_read_entry(p, names, &name_table, &name_len) == RIP_OK) {
    for (i = 0; i < p->count; i++) {
      struct pkg_entry *e = &p->entries[i];
      if (e->name_offset && e->name_offset < name_len) {
        size_t n = strnlen((const char *)name_table + e->name_offset, name_len - e->name_offset);
        if (n >= sizeof(e->name)) {
          n = sizeof(e->name) - 1;
        }
        memcpy(e->name, name_table + e->name_offset, n);
        e->name[n] = '\0';
      }
    }
    free(name_table);
  }
  return RIP_OK;
}

void pkg_close(struct pkg *p) {
  free(p->entries);
  p->entries = NULL;
  p->count = 0;
}

const struct pkg_entry *pkg_find(const struct pkg *p, uint32_t id) {
  unsigned i;

  for (i = 0; i < p->count; i++) {
    if (p->entries[i].id == id) {
      return &p->entries[i];
    }
  }
  return NULL;
}

int pkg_read_entry(const struct pkg *p, const struct pkg_entry *e, uint8_t **out, size_t *len) {
  uint8_t *buf;

  *out = NULL;
  *len = 0;
  if (pkg_entry_encrypted(e) || e->size > MAX_ENTRY_BYTES) {
    return RIP_ENOTSUP;
  }
  buf = malloc(e->size ? e->size : 1);
  if (!buf) {
    return RIP_ENOMEM;
  }
  if (e->size && rip_seek_read(p->fd, e->offset, buf, e->size) != 0) {
    free(buf);
    return RIP_ERR;
  }
  *out = buf;
  *len = e->size;
  return RIP_OK;
}

// A name from the table as a path under dir: no absolute paths, no "..".
static int safe_name(const char *name) {
  return name[0] && name[0] != '/' && !strstr(name, "..");
}

int pkg_extract_named(const struct pkg *p, const char *dir) {
  int written = 0;
  unsigned i;

  for (i = 0; i < p->count; i++) {
    const struct pkg_entry *e = &p->entries[i];
    char path[1024];
    char *slash;
    uint8_t *data;
    size_t len;
    int fd;
    int ok;

    if (!e->name[0] || pkg_entry_encrypted(e) || !safe_name(e->name)) {
      continue;
    }
    if (pkg_read_entry(p, e, &data, &len) != RIP_OK) {
      RIP_WARN("pkg: can't read entry %s", e->name);
      continue;
    }
    snprintf(path, sizeof(path), "%s/%s", dir, e->name);
    slash = strrchr(path, '/');
    if (slash) {
      *slash = '\0';
      rip_mkdir_p(path);
      *slash = '/';
    }
    fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    ok = fd >= 0 && rip_write_all(fd, data, len) == 0;
    if (fd >= 0 && close(fd) != 0) {
      ok = 0;
    }
    free(data);
    if (!ok) {
      RIP_WARN("pkg: can't write %s", path);
      return RIP_ERR;
    }
    written++;
  }
  return written;
}
