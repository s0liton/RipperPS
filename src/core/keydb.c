#include "ripper/keydb.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ripper/common.h"
#include "ripper/log.h"
#include "ripper/util.h"

static char *skip_space(char *p) {
  while (*p == ' ' || *p == '\t') {
    p++;
  }
  return p;
}

static char *next_token(char **p) {
  char *start = skip_space(*p);
  char *end = start;

  while (*end && *end != ' ' && *end != '\t' && *end != '\r' && *end != '\n') {
    end++;
  }
  if (end == start) {
    return NULL;
  }
  if (*end) {
    *end++ = '\0';
  }
  *p = end;
  return start;
}

static int parse_line(char *line, struct rip_key_entry *e) {
  char *p = line;
  char *sha1 = next_token(&p);
  char *size = sha1 ? next_token(&p) : NULL;
  char *key = size ? next_token(&p) : NULL;
  char *name;
  size_t n;

  if (!key || strlen(sha1) != 40 || strlen(key) != 32) {
    return RIP_ERR;
  }
  memset(e, 0, sizeof(*e));
  if (rip_unhex(sha1, e->sha1, 20) != 0 || rip_unhex(key, e->key, 16) != 0) {
    return RIP_ERR;
  }
  e->size = strtoull(size, NULL, 10);
  name = skip_space(p);
  n = strcspn(name, "\r\n");
  if (n >= sizeof(e->name)) {
    n = sizeof(e->name) - 1;
  }
  memcpy(e->name, name, n);
  e->name[n] = '\0';
  return RIP_OK;
}

int rip_keydb_load(struct rip_keydb *db, const char *path) {
  char line[512];
  size_t cap = 0;
  unsigned lineno = 0;
  FILE *f;

  memset(db, 0, sizeof(*db));
  f = fopen(path, "r");
  if (!f) {
    return RIP_ENOENT;
  }
  while (fgets(line, sizeof(line), f)) {
    struct rip_key_entry e;
    char *p = skip_space(line);

    lineno++;
    if (*p == '#' || *p == '\r' || *p == '\n' || *p == '\0') {
      continue;
    }
    if (parse_line(p, &e) != RIP_OK) {
      RIP_WARN("%s:%u: malformed key line skipped", path, lineno);
      continue;
    }
    if (db->count == cap) {
      size_t ncap = cap ? cap * 2 : 1024;
      struct rip_key_entry *grown = realloc(db->entries, ncap * sizeof(*grown));
      if (!grown) {
        fclose(f);
        rip_keydb_free(db);
        return RIP_ENOMEM;
      }
      db->entries = grown;
      cap = ncap;
    }
    db->entries[db->count++] = e;
  }
  fclose(f);
  RIP_INFO("loaded %zu PS3 keys from %s", db->count, path);
  return RIP_OK;
}

const struct rip_key_entry *rip_keydb_find(const struct rip_keydb *db, const uint8_t sha1[20], uint64_t size) {
  size_t i;

  for (i = 0; i < db->count; i++) {
    if (memcmp(db->entries[i].sha1, sha1, 20) == 0 && (db->entries[i].size == 0 || db->entries[i].size == size)) {
      return &db->entries[i];
    }
  }
  return NULL;
}

void rip_keydb_free(struct rip_keydb *db) {
  free(db->entries);
  memset(db, 0, sizeof(*db));
}

int rip_key_from_sidecar(const char *stem_path, uint8_t key[16]) {
  char path[1024];
  char hex[64];
  FILE *f;
  size_t n;

  snprintf(path, sizeof(path), "%s.key", stem_path);
  f = fopen(path, "rb");
  if (f) {
    n = fread(key, 1, 16, f);
    fclose(f);
    return n == 16 ? RIP_OK : RIP_ERR;
  }
  snprintf(path, sizeof(path), "%s.dkey", stem_path);
  f = fopen(path, "r");
  if (!f) {
    return RIP_ENOENT;
  }
  n = fread(hex, 1, sizeof(hex) - 1, f);
  fclose(f);
  hex[n] = '\0';
  n = strspn(hex, "0123456789abcdefABCDEF");
  if (n != 32) {
    return RIP_ERR;
  }
  return rip_unhex(hex, key, 16) == 0 ? RIP_OK : RIP_ERR;
}
