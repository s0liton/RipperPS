#include "service.h"

#include <signal.h>
#include <stdio.h>
#include <unistd.h>

#include "drives.h"
#include "jobs.h"
#include "platform.h"
#include "ripper/log.h"
#include "settings.h"
#include "svc.h"
#include "web.h"

// The loop ticks every few seconds; a longer gap means the console slept.
#define TICK_SEC 3
#define RESUME_GAP_SEC 60

// The build, findable in the ELF by RipperPS, which carries a copy of ripper
// and replaces a different build that's running (when nothing rips).
__attribute__((used)) const char ripper_build_mark[] = "ripper-build:" RIPPER_BUILD;

int service_main(void) {
  char path[SVC_PATH];
  char ip[64] = "";
  char body[128];
  int64_t last;
  int port;
  int init = plat_init();

  if (init > 0) {
    return 0;  // a newer copy is running
  }
  signal(SIGPIPE, SIG_IGN);
  svc_path_join(path, sizeof(path), plat_data_dir(), "ripper.log");
  svc_log_init(path);
  RIP_INFO("ripper %s (build %s) starting on %s, data in %s", RIPPER_VERSION, RIPPER_BUILD, plat_name(),
           plat_data_dir());

  settings_load();
  rip_log_set_level(RIP_LOG_INFO);
  jobs_init();
  drives_start();
  port = settings_web_port();
  if (web_start(port) != 0) {
    RIP_ERROR("the web UI isn't available yet");
  }
  plat_local_ip(ip, sizeof(ip));
  RIP_INFO("web UI on http://%s:%d", ip[0] ? ip : "127.0.0.1", port);
  snprintf(body, sizeof(body), "http://%s:%d", ip[0] ? ip : "127.0.0.1", port);
  plat_notify("Ripper is running", body);

  last = svc_now();
  for (;;) {
    int64_t now;
    sleep(TICK_SEC);
    now = svc_now();
    if (now - last > RESUME_GAP_SEC || web_take_reopened()) {
      RIP_INFO("resumed after %llds", (long long)(now - last));
    }
    last = now;
  }
  return 0;
}
