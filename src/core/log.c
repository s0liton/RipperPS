#include "ripper/log.h"

#include <stdarg.h>
#include <stdio.h>

static void default_sink(enum rip_log_level level, const char *msg, void *user) {
  static const char *names[] = {"debug", "info", "warn", "error"};

  (void)user;
  fprintf(stderr, "[%s] %s\n", names[level], msg);
}

static rip_log_sink_fn log_sink = default_sink;
static void *log_user;
static enum rip_log_level log_min = RIP_LOG_INFO;

void rip_log_set_sink(rip_log_sink_fn sink, void *user) {
  log_sink = sink ? sink : default_sink;
  log_user = user;
}

void rip_log_set_level(enum rip_log_level min) {
  log_min = min;
}

void rip_log(enum rip_log_level level, const char *fmt, ...) {
  char msg[1024];
  va_list ap;

  if (level < log_min) {
    return;
  }
  va_start(ap, fmt);
  vsnprintf(msg, sizeof(msg), fmt, ap);
  va_end(ap);
  log_sink(level, msg, log_user);
}
