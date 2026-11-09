#include "gamedb.h"

#include <ctype.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <zlib.h>

#include "gamedb_data.h"
#include "ripper/log.h"
#include "svc.h"

static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static char *g_text;    // the table, unpacked: "system\tserial\tname\n", sorted
static char **g_lines;  // where each line starts
static size_t g_count;
static int g_failed;

static int unpack(void) {
  z_stream z;
  size_t cap = 3u << 20;
  size_t len = 0;
  int rc;
  size_t i;

  memset(&z, 0, sizeof(z));
  g_text = malloc(cap);
  if (!g_text || inflateInit2(&z, 16 + MAX_WBITS) != Z_OK) {
    return -1;
  }
  z.next_in = (Bytef *)GAMEDB_GZ;
  z.avail_in = GAMEDB_GZ_LEN;
  do {
    if (len == cap) {
      char *bigger = realloc(g_text, cap * 2);
      if (!bigger) {
        inflateEnd(&z);
        return -1;
      }
      g_text = bigger;
      cap *= 2;
    }
    z.next_out = (Bytef *)g_text + len;
    z.avail_out = (uInt)(cap - len - 1);
    rc = inflate(&z, Z_NO_FLUSH);
    len = cap - 1 - z.avail_out;
  } while (rc == Z_OK);
  inflateEnd(&z);
  if (rc != Z_STREAM_END) {
    return -1;
  }
  g_text[len] = '\0';
  for (i = 0; i < len; i++) {
    g_count += g_text[i] == '\n';
  }
  g_lines = malloc(sizeof(*g_lines) * (g_count + 1));
  if (!g_lines) {
    return -1;
  }
  g_count = 0;
  {
    char *p = g_text;
    char *end = g_text + len;
    while (p < end) {
      char *nl = memchr(p, '\n', (size_t)(end - p));
      g_lines[g_count++] = p;
      if (!nl) {
        break;
      }
      *nl = '\0';
      p = nl + 1;
    }
  }
  RIP_INFO("game names: %zu serials", g_count);
  return 0;
}

// "system\tserial" against a line's start.
static int compare_key(const char *key, const char *line) {
  size_t k = strlen(key);
  int c = strncmp(key, line, k);
  if (c) {
    return c;
  }
  return line[k] == '\t' ? 0 : -1;
}

int gamedb_lookup(const char *system, const char *serial, char *name, size_t n) {
  char key[64];
  size_t lo = 0;
  size_t hi;
  int found = 0;
  size_t i;

  name[0] = '\0';
  if (!system || !serial || !serial[0]) {
    return 0;
  }
  snprintf(key, sizeof(key), "%s\t%s", system, serial);
  for (i = strlen(system) + 1; key[i]; i++) {
    key[i] = (char)toupper((unsigned char)key[i]);
  }
  pthread_mutex_lock(&g_lock);
  if (!g_lines && !g_failed && unpack() != 0) {
    RIP_WARN("game names: the table didn't unpack");
    g_failed = 1;
  }
  hi = g_count;
  while (lo < hi) {
    size_t mid = lo + (hi - lo) / 2;
    int c = compare_key(key, g_lines[mid]);
    if (c == 0) {
      svc_str_copy(name, n, g_lines[mid] + strlen(key) + 1);
      found = 1;
      break;
    }
    if (c < 0) {
      hi = mid;
    } else {
      lo = mid + 1;
    }
  }
  pthread_mutex_unlock(&g_lock);
  return found;
}

void gamedb_plain_title(const char *name, char *out, size_t n) {
  const char *paren = strstr(name, " (");
  size_t len = paren ? (size_t)(paren - name) : strlen(name);

  if (len >= n) {
    len = n - 1;
  }
  memcpy(out, name, len);
  out[len] = '\0';
}
