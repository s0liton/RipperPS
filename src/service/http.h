#ifndef RIPPER_HTTP_H_
#define RIPPER_HTTP_H_

// HTTP(S) through libcurl, for RomM. On the console the SDK's Mozilla CA
// bundle is built in (it has no CA store of its own), as RommPS does it.

#include <stddef.h>
#include <stdint.h>

#include "cJSON.h"

struct http_req {
  const char *method;  // "GET", "POST", "PUT"
  const char *url;
  const char *bearer;          // a token, or NULL
  const char *const *headers;  // "Name: value" lines, NULL-terminated, may be NULL
  const cJSON *json;           // a JSON body, or
  const void *body;            // raw bytes (with body_len)
  size_t body_len;
  int tls_verify;
  long timeout_s;  // 0: 60 s
};

struct http_resp {
  long status;  // 0 when no answer came
  char *body;   // NUL-terminated, free() it
  size_t len;
  char error[160];  // curl's reason when status is 0
};

// RIP_OK when the server answered (any status), RIP_ERR when it didn't.
int http_do(const struct http_req *req, struct http_resp *resp);
void http_resp_free(struct http_resp *resp);

// The body as JSON (NULL if it isn't), and RomM's "detail" message from it.
cJSON *http_json(const struct http_resp *resp);
void http_detail(const struct http_resp *resp, char *out, size_t n);

#endif
