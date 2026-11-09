#ifndef RIPPER_SVC_H_
#define RIPPER_SVC_H_

// The payload's service: what turns the core library into an app. A drive
// watcher, a job queue, destinations to deliver to, and the JSON API and web
// UI that the PS5 app and a browser use. Shared helpers live here.

#include <stddef.h>
#include <stdint.h>

#include "cJSON.h"

#ifndef RIPPER_VERSION
#define RIPPER_VERSION "0.2.0"
#endif
#ifndef RIPPER_BUILD
#define RIPPER_BUILD "dev"
#endif

#define SVC_DEFAULT_PORT 8790
// Where drive test results are sent: GitHub's new-issue page with the drive
// report form (.github/ISSUE_TEMPLATE/drive-report.yml). The web UI adds the
// form's fields to it.
#define RIPPER_REPORT_FORM "https://github.com/s0liton/ripper/issues/new?template=drive-report.yml"

#define SVC_PATH 1024

// Starts a detached thread with a 1 MiB stack: console threads get a tiny one
// by default, too small for compression or a TLS handshake.
int svc_thread(void *(*fn)(void *), void *arg);

// Logging: the core's RIP_INFO and friends go to the platform (klog or
// stderr), the log file, and a buffer the API serves.
void svc_log_init(const char *path);
char *svc_log_recent(void);  // the last lines, newline-separated; free it

// Files and paths.
char *svc_read_file(const char *path, size_t *len);  // NUL-terminated; free it
int svc_write_file_atomic(const char *path, const void *data, size_t len);
void svc_path_join(char *out, size_t n, const char *a, const char *b);
int svc_dir_exists(const char *path);
int64_t svc_file_size(const char *path);  // -1 when missing
// Moves a file, copying across file systems. progress may be NULL.
typedef int (*svc_progress_fn)(void *ud, uint64_t done, uint64_t total);  // nonzero stops
int svc_move_file(const char *from, const char *to, svc_progress_fn progress, void *ud);
int svc_copy_file(const char *from, const char *to, svc_progress_fn progress, void *ud);
void svc_remove_tree(const char *path);
// Free space where path lives, in bytes; -1 if unknown.
int64_t svc_free_space(const char *path);

// Strings.
void svc_str_copy(char *dst, size_t n, const char *src);
// Fills {key} placeholders from vars (key, value pairs, NULL-terminated).
// Values are made safe as path segments; '/' in the template stays.
void svc_template(char *out, size_t n, const char *tpl, const char *const *vars);
// "THE_MATRIX_D1" -> "The Matrix": a volume label people can read.
void svc_pretty_label(const char *label, char *out, size_t n);
int svc_is_id(const char *s);  // [a-z0-9_-]+, for ids from requests
// Letters, digits and single dashes, for a file or folder name: "HL-DT-ST BD-RE" -> "HL-DT-ST-BD-RE".
void svc_slug(const char *in, char *out, size_t n);

// JSON.
const char *jstr(const cJSON *o, const char *k, const char *def);
int jint(const cJSON *o, const char *k, int def);
int jbool(const cJSON *o, const char *k, int def);
void jset_str(cJSON *o, const char *k, const char *v);
void jset_num(cJSON *o, const char *k, double v);
void jset_bool(cJSON *o, const char *k, int v);
cJSON *jload(const char *path);  // NULL if missing or unreadable
int jsave(const char *path, const cJSON *j);

int64_t svc_now(void);  // seconds since the epoch
uint64_t svc_mono_ms(void);
// " · 12 min left", " · 1 h 5 min left"; "" when it can't be told.
void svc_time_left(double seconds, char *out, size_t n);

#endif
