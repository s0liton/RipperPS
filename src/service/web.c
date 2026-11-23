#include "web.h"

#include <arpa/inet.h>
#include <dirent.h>
#include <errno.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <unistd.h>

#include "bench.h"
#include "catalog.h"
#include "deliver.h"
#include "disc_report.h"
#include "drives.h"
#include "drivetest.h"
#include "jobs.h"
#include "platform.h"
#include "ripper/common.h"
#include "ripper/log.h"
#include "romm.h"
#include "settings.h"
#include "stats.h"
#include "svc.h"
#include "ui_index.h"  // generated from ui/index.html

#define MAX_HEADER (16 * 1024)
#define MAX_BODY (1024 * 1024)
#define ACCEPT_FAILS_BEFORE_REOPEN 3

static int64_t g_started;

struct request {
  int fd;
  char method[8];
  char path[1024];
  char query[2048];
  char *body;
  size_t body_len;
  char host[256];
  char origin[256];
  char fetch_site[32];
};

/* Responses */

static int send_all(int fd, const void *buf, size_t len) {
  const char *p = buf;

  while (len) {
    ssize_t n = send(fd, p, len, 0);
    if (n < 0 && errno == EINTR) {
      continue;
    }
    if (n <= 0) {
      return -1;
    }
    p += n;
    len -= (size_t)n;
  }
  return 0;
}

static const char *reason(int status) {
  switch (status) {
  case 200:
    return "OK";
  case 400:
    return "Bad Request";
  case 403:
    return "Forbidden";
  case 404:
    return "Not Found";
  case 409:
    return "Conflict";
  default:
    return "Error";
  }
}

static void send_response(int fd, int status, const char *ctype, const void *body, size_t len) {
  char hdr[256];
  int n = snprintf(hdr, sizeof(hdr),
                   "HTTP/1.1 %d %s\r\nContent-Type: %s\r\nContent-Length: %zu\r\nCache-Control: no-store\r\n"
                   "Connection: close\r\n\r\n",
                   status, reason(status), ctype, len);

  if (send_all(fd, hdr, (size_t)n) == 0 && len) {
    send_all(fd, body, len);
  }
}

static void send_json(int fd, int status, cJSON *j) {
  char *txt = cJSON_PrintUnformatted(j);

  send_response(fd, status, "application/json", txt ? txt : "null", txt ? strlen(txt) : 4);
  free(txt);
  cJSON_Delete(j);
}

static void send_error(int fd, int status, const char *msg) {
  cJSON *j = cJSON_CreateObject();

  cJSON_AddStringToObject(j, "error", msg);
  send_json(fd, status, j);
}

static void send_ok(int fd) {
  cJSON *j = cJSON_CreateObject();

  cJSON_AddBoolToObject(j, "ok", 1);
  send_json(fd, 200, j);
}

// The page with the theme already on <html>, so the first frame is right.
static void send_page(int fd) {
  char theme[32];
  char attr[64];
  char hdr[256];
  const char *page = UI_INDEX_HTML;
  const char *at = strstr(page, "<html");
  size_t len = sizeof(UI_INDEX_HTML) - 1;
  size_t head = at ? (size_t)(at - page) + 5 : 0;
  int n;

  settings_ui_theme(theme, sizeof(theme));
  attr[0] = '\0';
  if (at && svc_is_id(theme)) {
    snprintf(attr, sizeof(attr), " data-theme=\"%s\"", theme);
  }
  n = snprintf(hdr, sizeof(hdr),
               "HTTP/1.1 200 OK\r\nContent-Type: text/html; charset=utf-8\r\nContent-Length: %zu\r\n"
               "Cache-Control: no-store\r\nConnection: close\r\n\r\n",
               len + strlen(attr));
  if (send_all(fd, hdr, (size_t)n) == 0 && send_all(fd, page, head) == 0 && send_all(fd, attr, strlen(attr)) == 0) {
    send_all(fd, page + head, len - head);
  }
}

/* Requests */

static int hexval(char c) {
  if (c >= '0' && c <= '9') {
    return c - '0';
  }
  if (c >= 'a' && c <= 'f') {
    return c - 'a' + 10;
  }
  if (c >= 'A' && c <= 'F') {
    return c - 'A' + 10;
  }
  return -1;
}

static void url_decode(char *s) {
  char *o = s;
  char *p;

  for (p = s; *p; p++) {
    if (*p == '+') {
      *o++ = ' ';
    } else if (*p == '%' && hexval(p[1]) >= 0 && hexval(p[2]) >= 0) {
      *o++ = (char)(hexval(p[1]) * 16 + hexval(p[2]));
      p += 2;
    } else {
      *o++ = *p;
    }
  }
  *o = '\0';
}

static int qparam(const struct request *r, const char *key, char *out, size_t n) {
  size_t kl = strlen(key);
  const char *p = r->query;

  while (p && *p) {
    if (!strncmp(p, key, kl) && p[kl] == '=') {
      const char *v = p + kl + 1;
      const char *end = strchr(v, '&');
      size_t l = end ? (size_t)(end - v) : strlen(v);
      if (l >= n) {
        l = n - 1;
      }
      memcpy(out, v, l);
      out[l] = '\0';
      url_decode(out);
      return 1;
    }
    p = strchr(p, '&');
    if (p) {
      p++;
    }
  }
  out[0] = '\0';
  return 0;
}

static void header_value(const char *v, char *out, size_t n) {
  size_t len;

  while (*v == ' ' || *v == '\t') {
    v++;
  }
  len = strcspn(v, "\r\n");
  while (len && (v[len - 1] == ' ' || v[len - 1] == '\t')) {
    len--;
  }
  if (len >= n) {
    len = n - 1;
  }
  memcpy(out, v, len);
  out[len] = '\0';
}

// Whether a page on another site made the browser send this. Apps (the PS5
// app, curl) send neither header; our own page comes from this address.
static int cross_site(const struct request *r) {
  const char *o;

  if (!strcasecmp(r->fetch_site, "cross-site") || !strcasecmp(r->fetch_site, "same-site")) {
    return 1;
  }
  if (!r->origin[0]) {
    return 0;
  }
  o = strstr(r->origin, "://");
  return !o || strcasecmp(o + 3, r->host) != 0;
}

static int read_request(int fd, struct request *r) {
  char *buf = malloc(MAX_HEADER + 1);
  char *hdr_end = NULL;
  char target[3072] = "";
  size_t len = 0;
  size_t clen = 0;
  size_t have;
  char *q;
  char *h;

  if (!buf) {
    return -1;
  }
  while (!hdr_end && len < MAX_HEADER) {
    ssize_t n = recv(fd, buf + len, MAX_HEADER - len, 0);
    if (n <= 0) {
      free(buf);
      return -1;
    }
    len += (size_t)n;
    buf[len] = '\0';
    hdr_end = strstr(buf, "\r\n\r\n");
  }
  if (!hdr_end || sscanf(buf, "%7s %3071s", r->method, target) != 2) {
    free(buf);
    return -1;
  }
  q = strchr(target, '?');
  if (q) {
    *q = '\0';
    svc_str_copy(r->query, sizeof(r->query), q + 1);
  }
  svc_str_copy(r->path, sizeof(r->path), target);
  url_decode(r->path);
  for (h = strstr(buf, "\r\n"); h && h < hdr_end; h = strstr(h + 2, "\r\n")) {
    if (!strncasecmp(h + 2, "Content-Length:", 15)) {
      clen = (size_t)strtoul(h + 17, NULL, 10);
    } else if (!strncasecmp(h + 2, "Host:", 5)) {
      header_value(h + 7, r->host, sizeof(r->host));
    } else if (!strncasecmp(h + 2, "Origin:", 7)) {
      header_value(h + 9, r->origin, sizeof(r->origin));
    } else if (!strncasecmp(h + 2, "Sec-Fetch-Site:", 15)) {
      header_value(h + 17, r->fetch_site, sizeof(r->fetch_site));
    }
  }
  if (clen > MAX_BODY) {
    free(buf);
    return -1;
  }
  have = len - (size_t)(hdr_end + 4 - buf);
  r->body = malloc(clen + 1);
  if (!r->body) {
    free(buf);
    return -1;
  }
  memcpy(r->body, hdr_end + 4, have > clen ? clen : have);
  while (have < clen) {
    ssize_t n = recv(fd, r->body + have, clen - have, 0);
    if (n <= 0) {
      break;
    }
    have += (size_t)n;
  }
  r->body_len = have < clen ? have : clen;
  r->body[r->body_len] = '\0';
  free(buf);
  return 0;
}

static cJSON *body_json(const struct request *r) {
  return r->body_len ? cJSON_ParseWithLength(r->body, r->body_len) : NULL;
}

/* Handlers */

static cJSON *status_json(void) {
  cJSON *j = cJSON_CreateObject();
  char ip[64] = "";

  plat_local_ip(ip, sizeof(ip));
  cJSON_AddStringToObject(j, "version", RIPPER_VERSION);
  cJSON_AddStringToObject(j, "build", RIPPER_BUILD);
  cJSON_AddStringToObject(j, "platform", plat_name());
  cJSON_AddStringToObject(j, "ip", ip);
  cJSON_AddNumberToObject(j, "port", settings_web_port());
  cJSON_AddNumberToObject(j, "uptime", (double)(svc_now() - g_started));
  cJSON_AddNumberToObject(j, "jobs_active", jobs_active());
  return j;
}

// Folders below the storage roots, for picking where a local destination goes.
// scope "usb" keeps the USB drives' roots, "local" the others, "" all of them.
// With files, the folder's files are listed too, for picking one (a key file).
static cJSON *folders_json(const char *path, const char *scope, int files, char *err, size_t en) {
  struct plat_root roots[8];
  int nroots = plat_storage_roots(roots, 8);
  int kept = 0;
  cJSON *j = cJSON_CreateObject();
  cJSON *list = cJSON_AddArrayToObject(j, "folders");
  cJSON *file_list = NULL;
  char clean[SVC_PATH];
  size_t l;
  int inside = 0;
  int at_root = 0;
  int i;
  DIR *d;
  struct dirent *e;

  for (i = 0; i < nroots; i++) {
    int usb = !strncmp(roots[i].path, "/mnt/usb", 8);
    if (!scope[0] || (!strcmp(scope, "usb") ? usb : !usb)) {
      roots[kept++] = roots[i];
    }
  }
  nroots = kept;
  if (!path[0]) {
    cJSON_AddStringToObject(j, "path", "");
    cJSON_AddStringToObject(j, "parent", "");
    for (i = 0; i < nroots; i++) {
      cJSON *f = cJSON_CreateObject();
      cJSON_AddStringToObject(f, "path", roots[i].path);
      cJSON_AddStringToObject(f, "name", roots[i].label);
      cJSON_AddItemToArray(list, f);
    }
    return j;
  }
  svc_str_copy(clean, sizeof(clean), path);
  l = strlen(clean);
  while (l > 1 && clean[l - 1] == '/') {
    clean[--l] = '\0';
  }
  for (i = 0; i < nroots; i++) {
    size_t rl = strlen(roots[i].path);
    if (!strncmp(clean, roots[i].path, rl) && (clean[rl] == '\0' || clean[rl] == '/')) {
      inside = 1;
      at_root |= clean[rl] == '\0';
    }
  }
  if (files) {
    file_list = cJSON_AddArrayToObject(j, "files");
  }
  if (clean[0] != '/' || strstr(clean, "/..") || !inside || !svc_dir_exists(clean)) {
    snprintf(err, en, "not a folder RipperPS can use");
    cJSON_Delete(j);
    return NULL;
  }
  d = opendir(clean);
  while (d && (e = readdir(d)) != NULL) {
    char full[SVC_PATH];
    if (e->d_name[0] == '.') {
      continue;
    }
    svc_path_join(full, sizeof(full), clean, e->d_name);
    if (svc_dir_exists(full) && cJSON_GetArraySize(list) < 500) {
      cJSON *f = cJSON_CreateObject();
      cJSON_AddStringToObject(f, "path", full);
      cJSON_AddStringToObject(f, "name", e->d_name);
      cJSON_AddItemToArray(list, f);
    } else if (files && file_list && cJSON_GetArraySize(file_list) < 500) {
      struct stat st;
      if (stat(full, &st) == 0 && S_ISREG(st.st_mode)) {
        cJSON *f = cJSON_CreateObject();
        cJSON_AddStringToObject(f, "path", full);
        cJSON_AddStringToObject(f, "name", e->d_name);
        cJSON_AddNumberToObject(f, "size", (double)st.st_size);
        cJSON_AddItemToArray(file_list, f);
      }
    }
  }
  if (d) {
    closedir(d);
  }
  cJSON_AddStringToObject(j, "path", clean);
  if (at_root) {
    cJSON_AddStringToObject(j, "parent", "");
  } else {
    char parent[SVC_PATH];
    char *slash;
    svc_str_copy(parent, sizeof(parent), clean);
    slash = strrchr(parent, '/');
    if (slash && slash != parent) {
      *slash = '\0';
    }
    cJSON_AddStringToObject(j, "parent", parent);
  }
  return j;
}

// Installed PS4 and PS5 games, each with what a dump of it would take.
static cJSON *games_json(int all) {
  struct plat_game *games = calloc(256, sizeof(*games));
  int n = games ? plat_game_list(games, 256) : 0;
  cJSON *arr = cJSON_CreateArray();
  int i;

  for (i = 0; i < n; i++) {
    struct plat_dump_status st;
    cJSON *g;
    if (games[i].app && !all) {
      continue;
    }
    g = cJSON_CreateObject();
    char shown[24];
    char name[200];
    plat_game_status(games[i].title_id, &st);
    snprintf(shown, sizeof(shown), "%.4s-%.5s", games[i].title_id, games[i].title_id + 4);
    snprintf(name, sizeof(name), "%s (%s)", games[i].name, shown);
    cJSON_AddStringToObject(g, "title_id", games[i].title_id);
    cJSON_AddStringToObject(g, "title", games[i].name);
    cJSON_AddStringToObject(g, "name", name);
    cJSON_AddStringToObject(g, "system", games[i].ps5 ? "ps5" : "ps4");
    cJSON_AddNumberToObject(g, "size", (double)games[i].size);
    cJSON_AddBoolToObject(g, "app", games[i].app);
    cJSON_AddBoolToObject(g, "complete", st.complete);
    cJSON_AddBoolToObject(g, "partial", st.partial);
    cJSON_AddBoolToObject(g, "mounted", st.mounted);
    cJSON_AddBoolToObject(g, "game_running", st.running);
    cJSON_AddItemToArray(arr, g);
  }
  free(games);
  return arr;
}

// "/api/drives/cd0/eject" -> id "cd0", action "eject".
static int split_path(const char *path, const char *prefix, char *id, size_t idn, const char **action) {
  size_t pl = strlen(prefix);
  const char *rest;
  const char *slash;
  size_t n;

  if (strncmp(path, prefix, pl) != 0) {
    return 0;
  }
  rest = path + pl;
  slash = strchr(rest, '/');
  if (!slash) {
    return 0;
  }
  n = (size_t)(slash - rest);
  if (n == 0 || n >= idn) {
    return 0;
  }
  memcpy(id, rest, n);
  id[n] = '\0';
  *action = slash + 1;
  return 1;
}

static void handle_destination_test(struct request *r) {
  cJSON *j = body_json(r);
  cJSON *dest = cJSON_IsObject(j) ? cJSON_GetObjectItemCaseSensitive(j, "destination") : NULL;
  cJSON *stored;
  cJSON *out;
  char msg[512];
  int rc;

  if (!cJSON_IsObject(dest)) {
    dest = j;
  }
  if (!cJSON_IsObject(dest)) {
    cJSON_Delete(j);
    send_error(r->fd, 400, "send the destination to test");
    return;
  }
  // Secrets left blank (the UI never gets them back) come from the stored copy.
  stored = settings_destination(jstr(dest, "id", ""));
  if (stored && !strcmp(jstr(stored, "type", ""), jstr(dest, "type", ""))) {
    cJSON *f;
    cJSON_ArrayForEach(f, stored) {
      if (catalog_is_secret(jstr(dest, "type", ""), f->string) && !jstr(dest, f->string, "")[0]) {
        jset_str(dest, f->string, jstr(stored, f->string, ""));
      }
    }
  }
  cJSON_Delete(stored);
  rc = deliver_test(dest, msg, sizeof(msg));
  cJSON_Delete(j);
  out = cJSON_CreateObject();
  cJSON_AddBoolToObject(out, "ok", rc == RIP_OK);
  cJSON_AddStringToObject(out, "message", msg);
  send_json(r->fd, 200, out);
}

static void route(struct request *r) {
  int fd = r->fd;
  int is_get = !strcmp(r->method, "GET");
  int is_post = !strcmp(r->method, "POST");
  char err[256] = "";
  char id[32];
  const char *action;

  if (!is_get && cross_site(r)) {
    RIP_WARN("refused %s %s from %s", r->method, r->path, r->origin[0] ? r->origin : "another site");
    send_error(fd, 403, "requests from other websites aren't accepted");
    return;
  }
  if (is_get && (!strcmp(r->path, "/") || !strcmp(r->path, "/index.html"))) {
    send_page(fd);
    return;
  }
  if (is_get && !strcmp(r->path, "/api/status")) {
    send_json(fd, 200, status_json());
    return;
  }
  if (is_get && !strcmp(r->path, "/api/stats")) {
    send_json(fd, 200, stats_json());
    return;
  }
  if (is_post && !strcmp(r->path, "/api/keep-awake")) {
    // {"minutes": 60}: the console stays out of rest mode that long; 0 ends it.
    cJSON *j = body_json(r);
    cJSON *out = cJSON_CreateObject();
    jobs_keep_awake(jint(j, "minutes", 0) * 60);
    cJSON_Delete(j);
    cJSON_AddNumberToObject(out, "until", (double)jobs_awake_until());
    send_json(fd, 200, out);
    return;
  }
  if (is_post && !strcmp(r->path, "/api/notify")) {
    // A notification for RipperPS, which can't send one itself. "Title\nbody"
    // shows the title in bold.
    cJSON *j = body_json(r);
    const cJSON *m = cJSON_GetObjectItemCaseSensitive(j, "message");
    if (cJSON_IsString(m) && m->valuestring[0]) {
      char title[256];
      const char *nl = strchr(m->valuestring, '\n');
      svc_str_copy(title, sizeof(title), m->valuestring);
      if (nl && (size_t)(nl - m->valuestring) < sizeof(title)) {
        title[nl - m->valuestring] = '\0';
      }
      plat_notify(title, nl ? nl + 1 : "");
      send_ok(fd);
    } else {
      send_error(fd, 400, "no message");
    }
    cJSON_Delete(j);
    return;
  }
  if (is_get && !strcmp(r->path, "/api/catalog")) {
    send_json(fd, 200, catalog_json());
    return;
  }
  if (is_get && !strcmp(r->path, "/api/drives")) {
    send_json(fd, 200, drives_json());
    return;
  }
  if (is_get && split_path(r->path, "/api/drives/", id, sizeof(id), &action) && !strcmp(action, "cover")) {
    uint8_t *bytes = NULL;
    size_t len = 0;
    char type[24];
    if (drives_cover(id, &bytes, &len, type, sizeof(type)) == RIP_OK) {
      send_response(fd, 200, type, bytes, len);
      free(bytes);
    } else {
      send_error(fd, 404, "no cover");
    }
    return;
  }
  if (is_post && split_path(r->path, "/api/drives/", id, sizeof(id), &action)) {
    int rc = RIP_ERR;
    if (!strcmp(action, "eject")) {
      rc = drives_eject(id, err, sizeof(err));
    } else if (!strcmp(action, "identify")) {
      rc = drives_reidentify(id, err, sizeof(err));
    } else if (!strcmp(action, "benchmark")) {
      cJSON *out = bench_drive(id, err, sizeof(err));
      if (out) {
        send_json(fd, 200, out);
        return;
      }
    } else if (!strcmp(action, "drivetest")) {
      rc = drivetest_start(id, err, sizeof(err));
    } else if (!strcmp(action, "probe")) {
      cJSON *out = bench_probe(id, err, sizeof(err));
      if (out) {
        send_json(fd, 200, out);
        return;
      }
    } else if (!strcmp(action, "lookup")) {
      cJSON *j = body_json(r);
      rc = drives_lookup(id, jstr(j, "name", ""), err, sizeof(err));
      cJSON_Delete(j);
    } else if (!strcmp(action, "load")) {
      cJSON *j = body_json(r);
      rc = drives_load_image(id, jstr(j, "path", ""), err, sizeof(err));
      cJSON_Delete(j);
    } else {
      snprintf(err, sizeof(err), "unknown action");
    }
    if (rc == RIP_OK) {
      send_ok(fd);
    } else {
      send_error(fd, 400, err);
    }
    return;
  }
  // The drive test: a session a disc at a time, then its zip.
  if (is_get && !strcmp(r->path, "/api/drivetest")) {
    send_json(fd, 200, drivetest_status());
    return;
  }
  if (is_post && !strcmp(r->path, "/api/drivetest/disc")) {
    cJSON *j = body_json(r);
    const cJSON *said = cJSON_GetObjectItemCaseSensitive(j, "said");
    const cJSON *note = cJSON_GetObjectItemCaseSensitive(j, "note");
    int rc = drivetest_label(jint(j, "n", 0), cJSON_GetStringValue(said), cJSON_GetStringValue(note), err, sizeof(err));
    cJSON_Delete(j);
    if (rc == RIP_OK) {
      send_ok(fd);
    } else {
      send_error(fd, 400, err);
    }
    return;
  }
  if (is_post && !strcmp(r->path, "/api/drivetest/finish")) {
    if (drivetest_finish(err, sizeof(err)) == RIP_OK) {
      send_ok(fd);
    } else {
      send_error(fd, 400, err);
    }
    return;
  }
  if (is_post && !strcmp(r->path, "/api/drivetest/discard")) {
    drivetest_discard();
    send_ok(fd);
    return;
  }
  if (is_get && !strcmp(r->path, "/api/drivetest/file")) {
    char name[200];
    size_t len = 0;
    char *data;
    qparam(r, "name", name, sizeof(name));
    data = drivetest_file(name, &len);
    if (data) {
      send_response(fd, 200, "application/zip", data, len);
      free(data);
    } else {
      send_error(fd, 404, "no such drive test");
    }
    return;
  }
  if (is_get && !strcmp(r->path, "/api/reports")) {
    size_t len = 0;
    char *data = reports_bundle(&len);
    if (data) {
      send_response(fd, 200, "application/zip", data, len);
      free(data);
    } else {
      send_error(fd, 500, "couldn't put the reports together");
    }
    return;
  }
  if (is_post && !strcmp(r->path, "/api/rip")) {
    cJSON *j = body_json(r);
    int job = 0;
    int rc = cJSON_IsObject(j) ? jobs_start(j, &job, err, sizeof(err)) : RIP_ERR;
    cJSON_Delete(j);
    if (rc == RIP_OK) {
      cJSON *out = cJSON_CreateObject();
      cJSON_AddNumberToObject(out, "job", job);
      send_json(fd, 200, out);
    } else {
      send_error(fd, 400, err[0] ? err : "invalid request");
    }
    return;
  }
  if (is_get && !strcmp(r->path, "/api/games")) {
    char all[4];
    qparam(r, "all", all, sizeof(all));
    send_json(fd, 200, games_json(all[0] == '1'));
    return;
  }
  if (is_get && !strcmp(r->path, "/api/jobs")) {
    send_json(fd, 200, jobs_json());
    return;
  }
  if (is_post && !strcmp(r->path, "/api/jobs/clear")) {
    jobs_clear_finished();
    send_ok(fd);
    return;
  }
  if (is_post && split_path(r->path, "/api/jobs/", id, sizeof(id), &action)) {
    int rc = !strcmp(action, "cancel")  ? jobs_cancel(atoi(id), err, sizeof(err))
             : !strcmp(action, "retry") ? jobs_retry(atoi(id), err, sizeof(err))
                                        : RIP_ERR;
    if (rc == RIP_OK) {
      send_ok(fd);
    } else {
      send_error(fd, 400, err[0] ? err : "unknown action");
    }
    return;
  }
  if (is_get && !strcmp(r->path, "/api/settings")) {
    send_json(fd, 200, settings_json(0));
    return;
  }
  if (is_post && !strcmp(r->path, "/api/settings")) {
    cJSON *j = body_json(r);
    int rc = cJSON_IsObject(j) ? settings_apply(j, err, sizeof(err)) : RIP_ERR;
    cJSON_Delete(j);
    if (rc == RIP_OK) {
      send_json(fd, 200, settings_json(0));
    } else {
      send_error(fd, 400, err[0] ? err : "invalid settings");
    }
    return;
  }
  if (is_post && !strcmp(r->path, "/api/destinations/test")) {
    handle_destination_test(r);
    return;
  }
  // Pairing with RomM: start with the server's address, then poll.
  if (is_post && !strcmp(r->path, "/api/romm/pair")) {
    cJSON *j = body_json(r);
    int rc =
        cJSON_IsObject(j) ? romm_pair_start(jstr(j, "url", ""), jbool(j, "tls_verify", 1), err, sizeof(err)) : RIP_ERR;
    cJSON_Delete(j);
    if (rc == RIP_OK) {
      send_json(fd, 200, romm_pair_status());
    } else {
      send_error(fd, 400, err[0] ? err : "send the server's address");
    }
    return;
  }
  if (is_get && !strcmp(r->path, "/api/romm/pair")) {
    send_json(fd, 200, romm_pair_status());
    return;
  }
  if (is_post && !strcmp(r->path, "/api/romm/pair/cancel")) {
    romm_pair_cancel();
    send_json(fd, 200, romm_pair_status());
    return;
  }
  if (is_get && !strcmp(r->path, "/api/romm/rommps")) {
    send_json(fd, 200, romm_rommps());
    return;
  }
  if (is_get && !strcmp(r->path, "/api/folders")) {
    char path[SVC_PATH];
    cJSON *j;
    char scope[16];
    char files[4];
    qparam(r, "path", path, sizeof(path));
    qparam(r, "scope", scope, sizeof(scope));
    qparam(r, "files", files, sizeof(files));
    j = folders_json(path, scope, files[0] == '1', err, sizeof(err));
    if (j) {
      send_json(fd, 200, j);
    } else {
      send_error(fd, 400, err);
    }
    return;
  }
  if (is_get && !strcmp(r->path, "/api/log")) {
    char *log = svc_log_recent();
    send_response(fd, 200, "text/plain; charset=utf-8", log ? log : "", log ? strlen(log) : 0);
    free(log);
    return;
  }
  send_error(fd, 404, "not found");
}

/* Server */

static void *conn_thread(void *arg) {
  int fd = (int)(long)arg;
  struct timeval tv = {30, 0};
  struct request r;

  setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
  setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
#ifdef SO_NOSIGPIPE
  {
    int one = 1;
    setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof(one));
  }
#endif
  memset(&r, 0, sizeof(r));
  r.fd = fd;
  if (read_request(fd, &r) == 0) {
    route(&r);
  }
  free(r.body);
  close(fd);
  return NULL;
}

static int open_listener(int port) {
  struct sockaddr_in a;
  int one = 1;
  int srv = socket(AF_INET, SOCK_STREAM, 0);

  if (srv < 0) {
    return -1;
  }
  setsockopt(srv, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
  memset(&a, 0, sizeof(a));
  a.sin_family = AF_INET;
  a.sin_port = htons((uint16_t)port);
  a.sin_addr.s_addr = htonl(INADDR_ANY);
  if (bind(srv, (struct sockaddr *)&a, sizeof(a)) != 0 || listen(srv, 16) != 0) {
    int e = errno;
    close(srv);
    errno = e;
    return -1;
  }
  return srv;
}

static int g_port;
static int g_srv = -1;
static int g_reopened;

int web_take_reopened(void) {
  return __atomic_exchange_n(&g_reopened, 0, __ATOMIC_SEQ_CST);
}

// Rest mode tears the console's sockets down and the listener never accepts
// again after waking: when accept keeps failing, a fresh listener (as RommPS does).
static void *accept_thread(void *arg) {
  int fails = 0;

  (void)arg;
  for (;;) {
    int fd;
    if (g_srv < 0) {
      g_srv = open_listener(g_port);
      if (g_srv < 0) {
        sleep(3);
        continue;
      }
      RIP_INFO("web UI listening again on port %d", g_port);
      __atomic_store_n(&g_reopened, 1, __ATOMIC_SEQ_CST);
    }
    fd = accept(g_srv, NULL, NULL);
    if (fd < 0) {
      if (errno == EINTR) {
        continue;
      }
      if (++fails < ACCEPT_FAILS_BEFORE_REOPEN) {
        usleep(250 * 1000);
        continue;
      }
      RIP_WARN("accept failed (%s), reopening the listener", strerror(errno));
      close(g_srv);
      g_srv = -1;
      fails = 0;
      continue;
    }
    fails = 0;
    if (svc_thread(conn_thread, (void *)(long)fd) != 0) {
      close(fd);
    }
  }
  return NULL;
}

int web_start(int port) {
  g_started = svc_now();
  g_port = port;
  g_srv = open_listener(port);
  if (g_srv < 0) {
    RIP_ERROR("cannot listen on port %d (%s), retrying", port, strerror(errno));
  }
  svc_thread(accept_thread, NULL);
  return g_srv < 0 ? -1 : 0;
}
