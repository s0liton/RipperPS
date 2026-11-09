#include "http.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ripper/common.h"
#include "ripper/log.h"
#include "svc.h"

#ifdef RIPPER_HAVE_CURL

#include <curl/curl.h>
#include <pthread.h>

#ifdef RIPPER_EMBED_CA
#include "cacert_pem.h"  // generated from the SDK's Mozilla CA bundle
#endif

static pthread_once_t g_once = PTHREAD_ONCE_INIT;

static void global_init(void) {
  curl_global_init(CURL_GLOBAL_ALL);
}

struct sink {
  char *buf;
  size_t len;
  size_t cap;
};

static size_t write_cb(char *data, size_t size, size_t n, void *ud) {
  struct sink *s = ud;
  size_t add = size * n;

  if (s->len + add + 1 > s->cap) {
    size_t cap = (s->len + add + 1) * 2;
    char *b = realloc(s->buf, cap);
    if (!b) {
      return 0;
    }
    s->buf = b;
    s->cap = cap;
  }
  memcpy(s->buf + s->len, data, add);
  s->len += add;
  s->buf[s->len] = '\0';
  return add;
}

int http_do(const struct http_req *req, struct http_resp *resp) {
  CURL *c;
  struct curl_slist *hdrs = NULL;
  struct sink s = {NULL, 0, 0};
  char *json = NULL;
  char auth[600];
  CURLcode rc;
  int i;

  memset(resp, 0, sizeof(*resp));
  pthread_once(&g_once, global_init);
  c = curl_easy_init();
  if (!c) {
    snprintf(resp->error, sizeof(resp->error), "out of memory");
    return RIP_ERR;
  }
  curl_easy_setopt(c, CURLOPT_URL, req->url);
  curl_easy_setopt(c, CURLOPT_CUSTOMREQUEST, req->method);
  curl_easy_setopt(c, CURLOPT_NOSIGNAL, 1L);
  curl_easy_setopt(c, CURLOPT_CONNECTTIMEOUT, 10L);
  curl_easy_setopt(c, CURLOPT_TIMEOUT, req->timeout_s ? req->timeout_s : 60L);
  curl_easy_setopt(c, CURLOPT_FOLLOWLOCATION, 1L);
  curl_easy_setopt(c, CURLOPT_USERAGENT, "ripper/" RIPPER_VERSION);
  curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, write_cb);
  curl_easy_setopt(c, CURLOPT_WRITEDATA, &s);
  if (!req->tls_verify) {
    curl_easy_setopt(c, CURLOPT_SSL_VERIFYPEER, 0L);
    curl_easy_setopt(c, CURLOPT_SSL_VERIFYHOST, 0L);
  } else {
#ifdef RIPPER_EMBED_CA
    struct curl_blob blob = {(void *)CACERT_PEM, sizeof(CACERT_PEM) - 1, CURL_BLOB_NOCOPY};
    curl_easy_setopt(c, CURLOPT_CAINFO_BLOB, &blob);
#endif
  }
  if (req->bearer && req->bearer[0]) {
    snprintf(auth, sizeof(auth), "Authorization: Bearer %s", req->bearer);
    hdrs = curl_slist_append(hdrs, auth);
  }
  hdrs = curl_slist_append(hdrs, "Accept: application/json");
  for (i = 0; req->headers && req->headers[i]; i++) {
    hdrs = curl_slist_append(hdrs, req->headers[i]);
  }
  if (req->json) {
    json = cJSON_PrintUnformatted(req->json);
    hdrs = curl_slist_append(hdrs, "Content-Type: application/json");
    curl_easy_setopt(c, CURLOPT_POSTFIELDS, json ? json : "{}");
    curl_easy_setopt(c, CURLOPT_POSTFIELDSIZE_LARGE, (curl_off_t)(json ? strlen(json) : 2));
  } else if (req->body) {
    hdrs = curl_slist_append(hdrs, "Content-Type: application/octet-stream");
    curl_easy_setopt(c, CURLOPT_POSTFIELDS, req->body);
    curl_easy_setopt(c, CURLOPT_POSTFIELDSIZE_LARGE, (curl_off_t)req->body_len);
  } else if (strcmp(req->method, "GET") != 0) {
    curl_easy_setopt(c, CURLOPT_POSTFIELDS, "");
    curl_easy_setopt(c, CURLOPT_POSTFIELDSIZE_LARGE, (curl_off_t)0);
  }
  curl_easy_setopt(c, CURLOPT_HTTPHEADER, hdrs);
  rc = curl_easy_perform(c);
  if (rc == CURLE_OK) {
    curl_easy_getinfo(c, CURLINFO_RESPONSE_CODE, &resp->status);
  } else {
    snprintf(resp->error, sizeof(resp->error), "%s", curl_easy_strerror(rc));
  }
  resp->body = s.buf ? s.buf : calloc(1, 1);
  resp->len = s.len;
  curl_slist_free_all(hdrs);
  curl_easy_cleanup(c);
  free(json);
  return rc == CURLE_OK ? RIP_OK : RIP_ERR;
}

#else

int http_do(const struct http_req *req, struct http_resp *resp) {
  (void)req;
  memset(resp, 0, sizeof(*resp));
  resp->body = calloc(1, 1);
  snprintf(resp->error, sizeof(resp->error), "HTTP isn't in this build (no libcurl)");
  return RIP_ERR;
}

#endif

void http_resp_free(struct http_resp *resp) {
  free(resp->body);
  resp->body = NULL;
}

cJSON *http_json(const struct http_resp *resp) {
  return resp->body ? cJSON_Parse(resp->body) : NULL;
}

void http_detail(const struct http_resp *resp, char *out, size_t n) {
  cJSON *j = http_json(resp);
  const cJSON *d = cJSON_GetObjectItemCaseSensitive(j, "detail");

  out[0] = '\0';
  if (cJSON_IsString(d)) {
    snprintf(out, n, "%s", d->valuestring);
  } else if (cJSON_IsArray(d) && cJSON_GetArraySize(d) > 0) {
    // FastAPI's validation errors: the first one's message.
    snprintf(out, n, "%s", jstr(cJSON_GetArrayItem(d, 0), "msg", ""));
  } else if (resp->status) {
    snprintf(out, n, "HTTP %ld", resp->status);
  } else {
    snprintf(out, n, "%s", resp->error);
  }
  cJSON_Delete(j);
}
