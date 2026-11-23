#include "svc.h"

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <time.h>
#include <unistd.h>

#include "platform.h"
#include "ripper/log.h"
#include "ripper/util.h"

int svc_thread(void *(*fn)(void *), void *arg) {
  pthread_attr_t attr;
  pthread_t t;
  int rc;

  pthread_attr_init(&attr);
  pthread_attr_setstacksize(&attr, 1024 * 1024);
  pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
  rc = pthread_create(&t, &attr, fn, arg);
  pthread_attr_destroy(&attr);
  return rc == 0 ? 0 : -1;
}

/* Logging */

#define LOG_LINES 400
#define LOG_FILE_MAX (2 * 1024 * 1024)

static pthread_mutex_t g_log_lock = PTHREAD_MUTEX_INITIALIZER;
static char g_log_ring[LOG_LINES][256];
static unsigned g_log_next;
static unsigned g_log_count;
static char g_log_path[SVC_PATH];
static FILE *g_log_file;

static void log_sink(enum rip_log_level level, const char *msg, void *user) {
  static const char *names[] = {"DEBUG", "INFO", "WARN", "ERROR"};
  char when[32];
  char line[300];
  time_t now = time(NULL);
  struct tm tm;

  (void)user;
  localtime_r(&now, &tm);
  strftime(when, sizeof(when), "%Y-%m-%d %H:%M:%S", &tm);
  snprintf(line, sizeof(line), "%s [%s] %s", when, names[level], msg);
  plat_log_line(level, msg);
  pthread_mutex_lock(&g_log_lock);
  svc_str_copy(g_log_ring[g_log_next], sizeof(g_log_ring[0]), line);
  g_log_next = (g_log_next + 1) % LOG_LINES;
  if (g_log_count < LOG_LINES) {
    g_log_count++;
  }
  if (g_log_file) {
    fprintf(g_log_file, "%s\n", line);
    fflush(g_log_file);
    if (ftell(g_log_file) > LOG_FILE_MAX) {
      char old[SVC_PATH + 4];
      fclose(g_log_file);
      snprintf(old, sizeof(old), "%s.1", g_log_path);
      rename(g_log_path, old);
      g_log_file = fopen(g_log_path, "a");
    }
  }
  pthread_mutex_unlock(&g_log_lock);
}

void svc_log_init(const char *path) {
  pthread_mutex_lock(&g_log_lock);
  svc_str_copy(g_log_path, sizeof(g_log_path), path);
  g_log_file = fopen(path, "a");
  pthread_mutex_unlock(&g_log_lock);
  rip_log_set_sink(log_sink, NULL);
}

char *svc_log_recent(void) {
  char *out;
  size_t used = 0;
  unsigned i;

  pthread_mutex_lock(&g_log_lock);
  out = malloc((size_t)g_log_count * 257 + 1);
  if (out) {
    for (i = 0; i < g_log_count; i++) {
      const char *line = g_log_ring[(g_log_next + LOG_LINES - g_log_count + i) % LOG_LINES];
      size_t n = strlen(line);
      memcpy(out + used, line, n);
      used += n;
      out[used++] = '\n';
    }
    out[used] = '\0';
  }
  pthread_mutex_unlock(&g_log_lock);
  return out;
}

/* Files */

char *svc_read_file(const char *path, size_t *len) {
  FILE *f = fopen(path, "rb");
  char *buf;
  long n;

  if (len) {
    *len = 0;
  }
  if (!f) {
    return NULL;
  }
  if (fseek(f, 0, SEEK_END) != 0 || (n = ftell(f)) < 0 || fseek(f, 0, SEEK_SET) != 0) {
    fclose(f);
    return NULL;
  }
  buf = malloc((size_t)n + 1);
  if (buf && fread(buf, 1, (size_t)n, f) != (size_t)n) {
    free(buf);
    buf = NULL;
  }
  fclose(f);
  if (buf) {
    buf[n] = '\0';
    if (len) {
      *len = (size_t)n;
    }
  }
  return buf;
}

int svc_write_file_atomic(const char *path, const void *data, size_t len) {
  char tmp[SVC_PATH + 8];
  int fd;
  int ok;

  snprintf(tmp, sizeof(tmp), "%s.tmp", path);
  fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC, 0644);
  if (fd < 0) {
    return -1;
  }
  ok = rip_write_all(fd, data, len) == 0 && fsync(fd) == 0;
  ok = close(fd) == 0 && ok;
  if (!ok || rename(tmp, path) != 0) {
    unlink(tmp);
    return -1;
  }
  return 0;
}

void svc_path_join(char *out, size_t n, const char *a, const char *b) {
  size_t la = strlen(a);

  while (*b == '/') {
    b++;
  }
  if (!b[0]) {
    snprintf(out, n, "%s", a);
  } else if (la && a[la - 1] == '/') {
    snprintf(out, n, "%s%s", a, b);
  } else {
    snprintf(out, n, "%s/%s", a, b);
  }
}

int svc_dir_exists(const char *path) {
  struct stat st;

  return stat(path, &st) == 0 && S_ISDIR(st.st_mode);
}

int64_t svc_file_size(const char *path) {
  struct stat st;

  return stat(path, &st) == 0 ? (int64_t)st.st_size : -1;
}

int svc_copy_file(const char *from, const char *to, svc_progress_fn progress, void *ud) {
  enum { BUF = 4 << 20 };
  uint8_t *buf = malloc(BUF);
  int64_t total = svc_file_size(from);
  uint64_t done = 0;
  char part[SVC_PATH + 8];
  int in = open(from, O_RDONLY);
  int out;
  int rc = 0;
  long n;

  snprintf(part, sizeof(part), "%s.part", to);
  out = open(part, O_WRONLY | O_CREAT | O_TRUNC, 0644);
  if (!buf || in < 0 || out < 0) {
    rc = -1;
  }
  while (rc == 0 && (n = rip_read_full(in, buf, BUF)) > 0) {
    if (rip_write_all(out, buf, (size_t)n) != 0) {
      rc = -1;
      break;
    }
    done += (uint64_t)n;
    if (progress && progress(ud, done, total > 0 ? (uint64_t)total : 0)) {
      rc = -1;
    }
  }
  if (rc == 0 && n < 0) {
    rc = -1;
  }
  free(buf);
  if (in >= 0) {
    close(in);
  }
  if (out >= 0 && (fsync(out) != 0 || close(out) != 0)) {
    rc = -1;
  }
  if (rc == 0 && rename(part, to) != 0) {
    rc = -1;
  }
  if (rc != 0) {
    unlink(part);
  }
  return rc;
}

int svc_move_file(const char *from, const char *to, svc_progress_fn progress, void *ud) {
  if (rename(from, to) == 0) {
    return 0;
  }
  if (errno != EXDEV) {
    return -1;
  }
  if (svc_copy_file(from, to, progress, ud) != 0) {
    return -1;
  }
  unlink(from);
  return 0;
}

void svc_remove_tree(const char *path) {
  DIR *d = opendir(path);
  struct dirent *e;

  if (!d) {
    unlink(path);
    return;
  }
  while ((e = readdir(d)) != NULL) {
    char child[SVC_PATH];
    if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) {
      continue;
    }
    svc_path_join(child, sizeof(child), path, e->d_name);
    svc_remove_tree(child);
  }
  closedir(d);
  rmdir(path);
}

int64_t svc_free_space(const char *path) {
  struct statvfs st;

  if (statvfs(path, &st) != 0) {
    return -1;
  }
  return (int64_t)st.f_bavail * (int64_t)st.f_frsize;
}

/* Strings */

void svc_str_copy(char *dst, size_t n, const char *src) {
  if (n == 0) {
    return;
  }
  snprintf(dst, n, "%s", src ? src : "");
}

void svc_template(char *out, size_t n, const char *tpl, const char *const *vars) {
  size_t used = 0;
  const char *p = tpl;

  if (n == 0) {
    return;
  }
  out[0] = '\0';
  while (*p && used + 1 < n) {
    const char *close = *p == '{' ? strchr(p, '}') : NULL;
    if (close) {
      size_t klen = (size_t)(close - p - 1);
      const char *const *v;
      for (v = vars; v[0]; v += 2) {
        if (strlen(v[0]) == klen && !strncmp(v[0], p + 1, klen)) {
          char value[512];
          svc_str_copy(value, sizeof(value), v[1]);
          rip_sanitize_name(value, sizeof(value));
          used += (size_t)snprintf(out + used, n - used, "%s", value);
          if (used >= n) {
            used = n - 1;
          }
          break;
        }
      }
      p = close + 1;
      continue;
    }
    out[used++] = *p++;
    out[used] = '\0';
  }
  // No empty segments, no "..", no leading or trailing slashes.
  {
    char clean[SVC_PATH];
    char *save = NULL;
    char *seg;
    size_t c = 0;
    svc_str_copy(clean, sizeof(clean), out);
    out[0] = '\0';
    for (seg = strtok_r(clean, "/", &save); seg; seg = strtok_r(NULL, "/", &save)) {
      if (!strcmp(seg, ".") || !strcmp(seg, "..")) {
        continue;
      }
      c += (size_t)snprintf(out + c, n > c ? n - c : 0, "%s%s", c ? "/" : "", seg);
      if (c >= n) {
        break;
      }
    }
  }
}

void svc_pretty_label(const char *label, char *out, size_t n) {
  char buf[128];
  size_t len;
  size_t i;
  int word_start = 1;

  svc_str_copy(buf, sizeof(buf), label);
  for (i = 0; buf[i]; i++) {
    if (buf[i] == '_' || buf[i] == '.') {
      buf[i] = ' ';
    }
  }
  // Disc labels end in a disc number more often than not: "..._D1", "DISC_2".
  len = strlen(buf);
  while (len && buf[len - 1] == ' ') {
    buf[--len] = '\0';
  }
  if (len > 3 && buf[len - 3] == ' ' && (buf[len - 2] == 'D' || buf[len - 2] == 'd') &&
      isdigit((unsigned char)buf[len - 1])) {
    buf[len - 3] = '\0';
  }
  for (i = 0; buf[i]; i++) {
    unsigned char c = (unsigned char)buf[i];
    if (isalpha(c)) {
      buf[i] = (char)(word_start ? toupper(c) : tolower(c));
    }
    word_start = c == ' ' || c == '-';
  }
  // Words people expect in lower case inside a title.
  {
    static const char *small[] = {" Of ", " The ", " And ", " A ", " An ", " In ", " On ", " To "};
    size_t k;
    for (k = 0; k < sizeof(small) / sizeof(small[0]); k++) {
      char *at = buf + 1;
      while ((at = strstr(at, small[k])) != NULL) {
        at[1] = (char)tolower((unsigned char)at[1]);
        at++;
      }
    }
  }
  svc_str_copy(out, n, buf);
}

int svc_is_id(const char *s) {
  if (!s || !*s || strlen(s) > 48) {
    return 0;
  }
  for (; *s; s++) {
    if (!(islower((unsigned char)*s) || isdigit((unsigned char)*s) || *s == '-' || *s == '_')) {
      return 0;
    }
  }
  return 1;
}

/* JSON */

const char *jstr(const cJSON *o, const char *k, const char *def) {
  const cJSON *v = cJSON_GetObjectItemCaseSensitive(o, k);

  return cJSON_IsString(v) && v->valuestring ? v->valuestring : def;
}

int jint(const cJSON *o, const char *k, int def) {
  const cJSON *v = cJSON_GetObjectItemCaseSensitive(o, k);

  if (cJSON_IsNumber(v)) {
    return v->valueint;
  }
  if (cJSON_IsBool(v)) {
    return cJSON_IsTrue(v);
  }
  if (cJSON_IsString(v) && v->valuestring[0]) {
    return atoi(v->valuestring);
  }
  return def;
}

int jbool(const cJSON *o, const char *k, int def) {
  const cJSON *v = cJSON_GetObjectItemCaseSensitive(o, k);

  if (cJSON_IsBool(v)) {
    return cJSON_IsTrue(v);
  }
  if (cJSON_IsNumber(v)) {
    return v->valueint != 0;
  }
  return def;
}

void jset_str(cJSON *o, const char *k, const char *v) {
  cJSON_DeleteItemFromObjectCaseSensitive(o, k);
  cJSON_AddStringToObject(o, k, v ? v : "");
}

void jset_num(cJSON *o, const char *k, double v) {
  cJSON_DeleteItemFromObjectCaseSensitive(o, k);
  cJSON_AddNumberToObject(o, k, v);
}

void jset_bool(cJSON *o, const char *k, int v) {
  cJSON_DeleteItemFromObjectCaseSensitive(o, k);
  cJSON_AddBoolToObject(o, k, v);
}

cJSON *jload(const char *path) {
  char *txt = svc_read_file(path, NULL);
  cJSON *j = txt ? cJSON_Parse(txt) : NULL;

  free(txt);
  return j;
}

int jsave(const char *path, const cJSON *j) {
  char *txt = cJSON_Print(j);
  int rc = txt ? svc_write_file_atomic(path, txt, strlen(txt)) : -1;

  free(txt);
  return rc;
}

int64_t svc_now(void) {
  return (int64_t)time(NULL);
}

uint64_t svc_mono_ms(void) {
  return rip_now_ms();
}

void svc_time_left(double seconds, char *out, size_t n) {
  int min = (int)(seconds / 60.0 + 0.5);

  if (seconds <= 0) {
    out[0] = '\0';
  } else if (min < 1) {
    snprintf(out, n, " · under a minute left");
  } else if (min < 60) {
    snprintf(out, n, " · %d min left", min);
  } else {
    snprintf(out, n, " · %d h %d min left", min / 60, min % 60);
  }
}

int svc_is_mount(const char *path) {
  char parent[SVC_PATH];
  char *slash;
  struct stat a;
  struct stat b;

  svc_str_copy(parent, sizeof(parent), path);
  slash = strrchr(parent, '/');
  if (!slash) {
    return 0;
  }
  if (slash == parent) {
    slash[1] = '\0';
  } else {
    *slash = '\0';
  }
  return stat(path, &a) == 0 && S_ISDIR(a.st_mode) && stat(parent, &b) == 0 && a.st_dev != b.st_dev;
}

void svc_squeeze(const char *in, char *out, size_t n) {
  size_t k = 0;
  int space = 0;

  if (!n) {
    return;
  }
  for (; *in && k + 1 < n; in++) {
    if (*in == ' ') {
      space = k > 0;
      continue;
    }
    if (space && k + 2 < n) {
      out[k++] = ' ';
    }
    space = 0;
    out[k++] = *in;
  }
  out[k] = '\0';
}

void svc_slug(const char *in, char *out, size_t n) {
  size_t k = 0;
  int dash = 0;

  if (!n) {
    return;
  }
  for (; *in && k + 1 < n; in++) {
    if (isalnum((unsigned char)*in)) {
      out[k++] = *in;
      dash = 0;
    } else if (!dash && k) {
      out[k++] = '-';
      dash = 1;
    }
  }
  while (k && out[k - 1] == '-') {
    k--;
  }
  out[k] = '\0';
}
