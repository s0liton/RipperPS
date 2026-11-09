#ifndef RIPPER_LOG_H_
#define RIPPER_LOG_H_

// Prefixed names avoid clashing with <syslog.h> LOG_* macros on FreeBSD.
enum rip_log_level {
  RIP_LOG_DEBUG,
  RIP_LOG_INFO,
  RIP_LOG_WARN,
  RIP_LOG_ERROR
};

// The platform layer installs a sink: klog on the PS5, stderr on the host.
typedef void (*rip_log_sink_fn)(enum rip_log_level level, const char *msg, void *user);

void rip_log_set_sink(rip_log_sink_fn sink, void *user);
void rip_log_set_level(enum rip_log_level min);
void rip_log(enum rip_log_level level, const char *fmt, ...) __attribute__((format(printf, 2, 3)));

#define RIP_DEBUG(...) rip_log(RIP_LOG_DEBUG, __VA_ARGS__)
#define RIP_INFO(...) rip_log(RIP_LOG_INFO, __VA_ARGS__)
#define RIP_WARN(...) rip_log(RIP_LOG_WARN, __VA_ARGS__)
#define RIP_ERROR(...) rip_log(RIP_LOG_ERROR, __VA_ARGS__)

#endif
