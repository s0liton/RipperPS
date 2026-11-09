// RomM destinations (5.4 or newer): a rip goes up through RomM's chunked
// upload into its platform's folder, then RomM is asked to scan that
// platform. RomM takes one file per game, so a folder (BIN/CUE, a game dump)
// goes up as a zip of its contents. Pairing is RomM's device flow, as RommPS
// does it: the console shows a code, the user approves it in RomM.

#include <fcntl.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "cJSON.h"
#include "deliver.h"
#include "http.h"
#include "platform.h"
#include "ripper/common.h"
#include "ripper/log.h"
#include "ripper/util.h"
#include "ripper/zip.h"
#include "romm.h"
#include "svc.h"

#define CHUNK (16u * 1024 * 1024)  // RomM takes up to 64 MiB a chunk
#define ROMM_SCOPES "me.read", "platforms.read", "platforms.write", "roms.read", "roms.write", "tasks.run"

/* Server */

// "romm.lan:8080" or "https://romm.lan/" to "https://romm.lan:8080".
static void base_url(const char *in, char *out, size_t n) {
  size_t l;

  while (*in == ' ') {
    in++;
  }
  if (strncmp(in, "http://", 7) && strncmp(in, "https://", 8)) {
    snprintf(out, n, "https://%s", in);
  } else {
    snprintf(out, n, "%s", in);
  }
  l = strlen(out);
  while (l && (out[l - 1] == '/' || out[l - 1] == ' ')) {
    out[--l] = '\0';
  }
}

struct romm {
  char base[512];
  char token[512];
  int tls_verify;
};

static void romm_from(const cJSON *dest, struct romm *r) {
  memset(r, 0, sizeof(*r));
  base_url(jstr(dest, "url", ""), r->base, sizeof(r->base));
  svc_str_copy(r->token, sizeof(r->token), jstr(dest, "token", ""));
  r->tls_verify = jbool(dest, "tls_verify", 1);
}

static int call(const struct romm *r, const char *method, const char *path, const cJSON *json,
                const char *const *headers, const void *body, size_t len, struct http_resp *resp) {
  char url[1024];
  struct http_req req;

  snprintf(url, sizeof(url), "%s%s", r->base, path);
  memset(&req, 0, sizeof(req));
  req.method = method;
  req.url = url;
  req.bearer = r->token;
  req.headers = headers;
  req.json = json;
  req.body = body;
  req.body_len = len;
  req.tls_verify = r->tls_verify;
  req.timeout_s = body ? 600 : 60;
  return http_do(&req, resp);
}

// RomM's version, "" when nothing that looks like RomM answered.
static int heartbeat(const struct romm *r, char *version, size_t vn, char *err, size_t en) {
  struct http_resp resp;
  cJSON *j;
  const cJSON *v;
  int major = 0;
  int minor = 0;

  call(r, "GET", "/api/heartbeat", NULL, NULL, NULL, 0, &resp);
  j = http_json(&resp);
  v = cJSON_GetObjectItemCaseSensitive(cJSON_GetObjectItemCaseSensitive(j, "SYSTEM"), "VERSION");
  if (resp.status != 200 || !cJSON_IsString(v)) {
    if (resp.status) {
      snprintf(err, en, "%s answered, but not as RomM does (HTTP %ld)", r->base, resp.status);
    } else {
      snprintf(err, en, "no answer from %s: %s", r->base, resp.error);
    }
    cJSON_Delete(j);
    http_resp_free(&resp);
    return RIP_ERR;
  }
  snprintf(version, vn, "%s", v->valuestring);
  cJSON_Delete(j);
  http_resp_free(&resp);
  sscanf(version, "%d.%d", &major, &minor);
  if (major && (major < 5 || (major == 5 && minor < 4))) {
    snprintf(err, en, "RomM %s found; uploads need RomM 5.4 or newer", version);
    return RIP_ERR;
  }
  return RIP_OK;
}

int deliver_romm_test(const cJSON *dest, char *msg, size_t n) {
  struct romm r;
  struct http_resp resp;
  char version[32] = "";
  cJSON *me;

  romm_from(dest, &r);
  if (!jstr(dest, "url", "")[0]) {
    snprintf(msg, n, "no server set");
    return RIP_ERR;
  }
  if (heartbeat(&r, version, sizeof(version), msg, n) != RIP_OK) {
    return RIP_ERR;
  }
  if (!r.token[0]) {
    snprintf(msg, n, "RomM %s found; pair the console to upload", version);
    return RIP_ERR;
  }
  call(&r, "GET", "/api/users/me", NULL, NULL, NULL, 0, &resp);
  me = http_json(&resp);
  if (resp.status != 200) {
    char d[160];
    http_detail(&resp, d, sizeof(d));
    snprintf(msg, n, "RomM %s found, but the token isn't accepted (%s): pair again", version, d);
    cJSON_Delete(me);
    http_resp_free(&resp);
    return RIP_ERR;
  }
  snprintf(msg, n, "RomM %s, signed in as %s", version, jstr(me, "username", "?"));
  cJSON_Delete(me);
  http_resp_free(&resp);
  return RIP_OK;
}

/* Upload */

// The platform's id for a slug, made in RomM when it isn't there yet.
static int platform_id(const struct romm *r, const char *slug, char *name, size_t nn, char *err, size_t en) {
  struct http_resp resp;
  cJSON *list;
  const cJSON *p;
  cJSON *body;
  int id = 0;

  call(r, "GET", "/api/platforms", NULL, NULL, NULL, 0, &resp);
  list = http_json(&resp);
  if (resp.status != 200 || !cJSON_IsArray(list)) {
    http_detail(&resp, err, en);
    cJSON_Delete(list);
    http_resp_free(&resp);
    return -1;
  }
  cJSON_ArrayForEach(p, list) {
    if (!strcmp(jstr(p, "fs_slug", ""), slug) || !strcmp(jstr(p, "slug", ""), slug)) {
      id = jint(p, "id", 0);
      snprintf(name, nn, "%s", jstr(p, "display_name", jstr(p, "name", slug)));
      break;
    }
  }
  cJSON_Delete(list);
  http_resp_free(&resp);
  if (id > 0) {
    return id;
  }
  body = cJSON_CreateObject();
  cJSON_AddStringToObject(body, "fs_slug", slug);
  call(r, "POST", "/api/platforms", body, NULL, NULL, 0, &resp);
  cJSON_Delete(body);
  list = http_json(&resp);
  if ((resp.status == 200 || resp.status == 201) && jint(list, "id", 0) > 0) {
    id = jint(list, "id", 0);
    snprintf(name, nn, "%s", jstr(list, "display_name", jstr(list, "name", slug)));
    RIP_INFO("romm: made the platform %s (%d)", slug, id);
  } else {
    char d[160];
    http_detail(&resp, d, sizeof(d));
    snprintf(err, en, "RomM has no %s platform and couldn't make one (%s)", slug, d);
    id = -1;
  }
  cJSON_Delete(list);
  http_resp_free(&resp);
  return id;
}

// The header can't carry names outside Latin-1 (the JSON body has the real one).
static void header_name(const char *name, char *out, size_t n) {
  size_t i;
  size_t o = 0;

  for (i = 0; name[i] && o + 1 < n; i++) {
    unsigned char ch = (unsigned char)name[i];
    out[o++] = ch < 0x80 && ch >= 0x20 ? (char)ch : '_';
  }
  out[o] = '\0';
}

static int upload(const struct romm *r, int platform, const char *path, const char *name, const struct deliver_req *req,
                  uint64_t base_done, uint64_t all, char *err, size_t en) {
  struct http_resp resp;
  struct stat st;
  cJSON *body;
  cJSON *j;
  char h_platform[64];
  char h_name[600];
  char h_size[64];
  char h_count[64];
  char ascii[512];
  char upload_id[128] = "";
  char p[300];
  uint8_t *buf = NULL;
  uint64_t size;
  uint64_t chunks;
  uint64_t per;
  uint64_t i;
  int fd = -1;
  int rc = RIP_OK;

  if (stat(path, &st) != 0) {
    snprintf(err, en, "%s is missing", path);
    return RIP_ERR;
  }
  size = (uint64_t)st.st_size;
  chunks = (size + CHUNK - 1) / CHUNK;
  // RomM expects the size split evenly, the last chunk taking what's left.
  per = chunks ? (size + chunks - 1) / chunks : 0;
  header_name(name, ascii, sizeof(ascii));
  snprintf(h_platform, sizeof(h_platform), "x-upload-platform: %d", platform);
  snprintf(h_name, sizeof(h_name), "x-upload-filename: %s", ascii);
  snprintf(h_size, sizeof(h_size), "x-upload-total-size: %llu", (unsigned long long)size);
  snprintf(h_count, sizeof(h_count), "x-upload-total-chunks: %llu", (unsigned long long)chunks);
  {
    const char *headers[] = {h_platform, h_name, h_size, h_count, NULL};
    body = cJSON_CreateObject();
    cJSON_AddStringToObject(body, "filename", name);
    call(r, "POST", "/api/roms/upload/start", body, headers, NULL, 0, &resp);
    cJSON_Delete(body);
  }
  j = http_json(&resp);
  if ((resp.status != 200 && resp.status != 201) || !jstr(j, "upload_id", "")[0]) {
    char d[200];
    http_detail(&resp, d, sizeof(d));
    snprintf(err, en, "RomM refused the upload of %s: %s", name, d);
    cJSON_Delete(j);
    http_resp_free(&resp);
    return RIP_ERR;
  }
  svc_str_copy(upload_id, sizeof(upload_id), jstr(j, "upload_id", ""));
  cJSON_Delete(j);
  http_resp_free(&resp);
  RIP_INFO("romm: uploading %s, %.1f MB in %llu chunk(s)", name, (double)size / 1e6, (unsigned long long)chunks);

  buf = malloc(CHUNK);
  fd = open(path, O_RDONLY);
  if (!buf || fd < 0) {
    snprintf(err, en, "can't read %s", path);
    rc = RIP_ERR;
  }
  for (i = 0; rc == RIP_OK && i < chunks; i++) {
    size_t want = (size_t)(size - i * per < per ? size - i * per : per);
    char h_index[64];
    const char *headers[] = {h_index, NULL};
    long got = rip_read_full(fd, buf, want);
    if (got != (long)want) {
      snprintf(err, en, "can't read %s", path);
      rc = RIP_ERR;
      break;
    }
    snprintf(h_index, sizeof(h_index), "x-chunk-index: %llu", (unsigned long long)i);
    snprintf(p, sizeof(p), "/api/roms/upload/%s", upload_id);
    call(r, "PUT", p, NULL, headers, buf, want, &resp);
    if (resp.status != 200 && resp.status != 201) {
      char d[200];
      http_detail(&resp, d, sizeof(d));
      snprintf(err, en, "chunk %llu of %s didn't go up: %s", (unsigned long long)i + 1, name, d);
      rc = RIP_ERR;
    }
    http_resp_free(&resp);
    if (rc == RIP_OK && req->progress && req->progress(req->ud, base_done + i * per + want, all)) {
      rc = RIP_ECANCEL;
    }
  }
  if (fd >= 0) {
    close(fd);
  }
  free(buf);
  if (rc == RIP_OK) {
    snprintf(p, sizeof(p), "/api/roms/upload/%s/complete", upload_id);
    call(r, "POST", p, NULL, NULL, NULL, 0, &resp);
    if (resp.status != 200 && resp.status != 201) {
      char d[200];
      http_detail(&resp, d, sizeof(d));
      snprintf(err, en, "RomM didn't take %s at the end: %s", name, d);
      rc = RIP_ERR;
    }
    http_resp_free(&resp);
  }
  if (rc != RIP_OK) {
    snprintf(p, sizeof(p), "/api/roms/upload/%s/cancel", upload_id);
    call(r, "POST", p, NULL, NULL, NULL, 0, &resp);
    http_resp_free(&resp);
  }
  return rc;
}

// Zipping shows as the first pass over the bytes; the upload is the second.
static int zip_progress(void *ud, uint64_t done, uint64_t total) {
  const struct deliver_req *req = ud;
  return req->progress ? req->progress(req->ud, done, total) : 0;
}

int deliver_romm(const struct deliver_req *req, char *where, size_t wn, char *err, size_t en) {
  struct romm r;
  struct stat st;
  char version[32];
  char pname[128] = "";
  char path[1100];
  char upname[600];
  char zip_path[1200] = "";
  int pid;
  int rc;
  int i;
  int pick = -1;

  romm_from(req->dest, &r);
  if (!req->romm_platform || !req->romm_platform[0]) {
    snprintf(err, en, "this kind of disc has no RomM platform; send it to another destination");
    return RIP_ERR;
  }
  if (!r.token[0]) {
    snprintf(err, en, "the console isn't paired with RomM yet (Settings, the RomM destination, Pair)");
    return RIP_ERR;
  }
  if (heartbeat(&r, version, sizeof(version), err, en) != RIP_OK) {
    return RIP_ERR;
  }
  // The game: the rip's first item that isn't its manifest.
  for (i = 0; i < req->count && pick < 0; i++) {
    size_t l = strlen(req->files[i]);
    if (!(l > 4 && !strcmp(req->files[i] + l - 4, ".txt"))) {
      pick = i;
    }
  }
  if (pick < 0) {
    snprintf(err, en, "nothing to upload");
    return RIP_ERR;
  }
  snprintf(path, sizeof(path), "%s/%s", req->dir, req->files[pick]);
  if (stat(path, &st) != 0) {
    snprintf(err, en, "%s is missing", path);
    return RIP_ERR;
  }
  if (S_ISDIR(st.st_mode)) {
    // One file per game in RomM: the folder's contents as a zip.
    snprintf(zip_path, sizeof(zip_path), "%s.zip", path);
    snprintf(upname, sizeof(upname), "%s.zip", req->files[pick]);
    if (req->progress) {
      req->progress(req->ud, 0, 1);
    }
    rc = zip_build_dir(path, zip_path, zip_progress, (void *)req);
    if (rc != RIP_OK) {
      snprintf(err, en, rc == RIP_ECANCEL ? "cancelled" : "couldn't zip %s for RomM", req->files[pick]);
      return rc;
    }
    svc_str_copy(path, sizeof(path), zip_path);
  } else {
    svc_str_copy(upname, sizeof(upname), req->files[pick]);
  }
  pid = platform_id(&r, req->romm_platform, pname, sizeof(pname), err, en);
  if (pid <= 0) {
    if (zip_path[0]) {
      unlink(zip_path);
    }
    return RIP_ERR;
  }
  if (stat(path, &st) != 0) {
    snprintf(err, en, "%s is missing", path);
    return RIP_ERR;
  }
  rc = upload(&r, pid, path, upname, req, 0, (uint64_t)st.st_size, err, en);
  if (zip_path[0]) {
    unlink(zip_path);
  }
  if (rc != RIP_OK) {
    return rc;
  }
  if (jbool(req->dest, "scan", 1)) {
    // Quick scan of the platform; a scan already running (409) finds it too.
    struct http_resp resp;
    cJSON *body = cJSON_CreateObject();
    cJSON *plats = cJSON_AddArrayToObject(body, "platforms");
    cJSON_AddItemToArray(plats, cJSON_CreateNumber(pid));
    call(&r, "POST", "/api/tasks/scan", body, NULL, NULL, 0, &resp);
    cJSON_Delete(body);
    if (resp.status != 200 && resp.status != 201 && resp.status != 202 && resp.status != 409) {
      char d[160];
      http_detail(&resp, d, sizeof(d));
      RIP_WARN("romm: the scan didn't start (%s); RomM's next scan will find it", d);
    }
    http_resp_free(&resp);
  }
  snprintf(where, wn, "RomM: %s / %s", pname[0] ? pname : req->romm_platform, upname);
  return RIP_OK;
}

/* Pairing */

static pthread_mutex_t g_pair_lock = PTHREAD_MUTEX_INITIALIZER;
static char g_pair_status[16] = "idle";  // idle, pending, ok, error
static char g_pair_message[256];
static char g_pair_code[32];
static char g_pair_link[700];
static char g_pair_token[512];
static int g_pair_gen;

struct pair_job {
  struct romm r;
  char device_code[160];
  int interval;
  int expires;
  int gen;
};

static void pair_set(const char *status, const char *msg) {
  pthread_mutex_lock(&g_pair_lock);
  svc_str_copy(g_pair_status, sizeof(g_pair_status), status);
  svc_str_copy(g_pair_message, sizeof(g_pair_message), msg);
  pthread_mutex_unlock(&g_pair_lock);
}

static void *pair_poll(void *arg) {
  struct pair_job *p = arg;
  int left = p->expires;

  while (left > 0 && p->gen == g_pair_gen) {
    struct http_resp resp;
    cJSON *body;
    cJSON *j;
    sleep((unsigned)p->interval);
    left -= p->interval;
    if (p->gen != g_pair_gen) {
      break;
    }
    body = cJSON_CreateObject();
    cJSON_AddStringToObject(body, "device_code", p->device_code);
    call(&p->r, "POST", "/api/auth/device/token", body, NULL, NULL, 0, &resp);
    cJSON_Delete(body);
    j = http_json(&resp);
    if (resp.status == 200 && jstr(j, "access_token", "")[0]) {
      pthread_mutex_lock(&g_pair_lock);
      if (p->gen == g_pair_gen) {
        svc_str_copy(g_pair_token, sizeof(g_pair_token), jstr(j, "access_token", ""));
        svc_str_copy(g_pair_status, sizeof(g_pair_status), "ok");
        svc_str_copy(g_pair_message, sizeof(g_pair_message), "Paired. Save the destination to keep it.");
      }
      pthread_mutex_unlock(&g_pair_lock);
      RIP_INFO("romm: paired with %s", p->r.base);
      cJSON_Delete(j);
      http_resp_free(&resp);
      free(p);
      return NULL;
    }
    if (resp.status >= 400 && resp.status < 500) {
      char d[160];
      http_detail(&resp, d, sizeof(d));
      if (strstr(d, "slow_down") || resp.status == 429) {
        p->interval += 5;
      } else if (strstr(d, "access_denied")) {
        pair_set("error", "Pairing was turned down in RomM");
        left = -1;
      } else if (strstr(d, "expired")) {
        left = 0;
      }
    }
    cJSON_Delete(j);
    http_resp_free(&resp);
  }
  if (left == 0 && p->gen == g_pair_gen) {
    pair_set("error", "The code expired; start again");
  }
  free(p);
  return NULL;
}

// The console's id for RomM, made once.
static void device_identifier(char *out, size_t n) {
  char path[600];
  FILE *f;

  snprintf(path, sizeof(path), "%s/romm_device_id", plat_data_dir());
  f = fopen(path, "r");
  if (f) {
    if (fgets(out, (int)n, f)) {
      out[strcspn(out, "\r\n")] = '\0';
    }
    fclose(f);
    if (out[0]) {
      return;
    }
  }
  snprintf(out, n, "ripper-%08x%08x", (unsigned)rip_now_ms() ^ (unsigned)getpid(), (unsigned)rand());
  rip_write_file(path, out, strlen(out));
}

int romm_pair_start(const char *url, int tls_verify, char *err, size_t en) {
  static const char *scopes[] = {ROMM_SCOPES};
  struct pair_job *p;
  struct http_resp resp;
  cJSON *body;
  cJSON *j;
  char version[32];
  char id[64];

  p = calloc(1, sizeof(*p));
  if (!p) {
    snprintf(err, en, "out of memory");
    return RIP_ENOMEM;
  }
  base_url(url, p->r.base, sizeof(p->r.base));
  p->r.tls_verify = tls_verify;
  if (heartbeat(&p->r, version, sizeof(version), err, en) != RIP_OK) {
    free(p);
    return RIP_ERR;
  }
  device_identifier(id, sizeof(id));
  body = cJSON_CreateObject();
  cJSON_AddStringToObject(body, "client_device_identifier", id);
  cJSON_AddStringToObject(body, "name", "RipperPS on PS5");
  cJSON_AddStringToObject(body, "client", "ripper");
  cJSON_AddStringToObject(body, "platform", "PS5");
  cJSON_AddStringToObject(body, "client_version", RIPPER_VERSION);
  cJSON_AddItemToObject(body, "requested_scopes", cJSON_CreateStringArray(scopes, RIP_ARRAY_LEN(scopes)));
  call(&p->r, "POST", "/api/auth/device/init", body, NULL, NULL, 0, &resp);
  cJSON_Delete(body);
  j = http_json(&resp);
  if ((resp.status != 200 && resp.status != 201) || !jstr(j, "device_code", "")[0]) {
    char d[160];
    http_detail(&resp, d, sizeof(d));
    snprintf(err, en, "RomM %s didn't start pairing (%s)", version, d);
    cJSON_Delete(j);
    http_resp_free(&resp);
    free(p);
    return RIP_ERR;
  }
  svc_str_copy(p->device_code, sizeof(p->device_code), jstr(j, "device_code", ""));
  p->interval = jint(j, "interval", 5) > 0 ? jint(j, "interval", 5) : 5;
  p->expires = jint(j, "expires_in", 600);
  pthread_mutex_lock(&g_pair_lock);
  p->gen = ++g_pair_gen;
  svc_str_copy(g_pair_status, sizeof(g_pair_status), "pending");
  svc_str_copy(g_pair_code, sizeof(g_pair_code), jstr(j, "user_code", ""));
  snprintf(g_pair_link, sizeof(g_pair_link), "%s%s", p->r.base, jstr(j, "verification_path_complete", ""));
  snprintf(g_pair_message, sizeof(g_pair_message), "Approve code %s in RomM", g_pair_code);
  g_pair_token[0] = '\0';
  pthread_mutex_unlock(&g_pair_lock);
  cJSON_Delete(j);
  http_resp_free(&resp);
  if (svc_thread(pair_poll, p) != 0) {
    pair_set("error", "couldn't start waiting for RomM");
    free(p);
    return RIP_ERR;
  }
  return RIP_OK;
}

cJSON *romm_pair_status(void) {
  cJSON *o = cJSON_CreateObject();

  pthread_mutex_lock(&g_pair_lock);
  cJSON_AddStringToObject(o, "status", g_pair_status);
  cJSON_AddStringToObject(o, "message", g_pair_message);
  if (!strcmp(g_pair_status, "pending")) {
    cJSON_AddStringToObject(o, "user_code", g_pair_code);
    cJSON_AddStringToObject(o, "verification_url", g_pair_link);
  }
  if (!strcmp(g_pair_status, "ok")) {
    // Handed over once, for the destination form to save.
    cJSON_AddStringToObject(o, "token", g_pair_token);
    g_pair_token[0] = '\0';
    svc_str_copy(g_pair_status, sizeof(g_pair_status), "idle");
  }
  pthread_mutex_unlock(&g_pair_lock);
  return o;
}

void romm_pair_cancel(void) {
  pthread_mutex_lock(&g_pair_lock);
  g_pair_gen++;
  svc_str_copy(g_pair_status, sizeof(g_pair_status), "idle");
  g_pair_message[0] = '\0';
  pthread_mutex_unlock(&g_pair_lock);
}

/* Pairing RomM */

cJSON *romm_rommps(void) {
  char path[SVC_PATH];
  cJSON *cfg;
  cJSON *out = cJSON_CreateObject();

  svc_path_join(path, sizeof(path), plat_rommps_dir(), "config.json");
  cfg = jload(path);

  if (!cfg || !jstr(cfg, "server_url", "")[0]) {
    cJSON_AddBoolToObject(out, "found", 0);
    cJSON_Delete(cfg);
    return out;
  }

  cJSON_AddBoolToObject(out, "found", 1);
  cJSON_AddStringToObject(out, "url", jstr(cfg, "server_url", ""));
  cJSON_AddBoolToObject(out, "tls_verify", jbool(cfg, "tls_verify", 1));
  cJSON_AddStringToObject(out, "username", jstr(cfg, "username", ""));
  cJSON_Delete(cfg);
  return out;
}