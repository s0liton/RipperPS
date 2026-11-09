#include "diagnose.h"

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "drives.h"
#include "platform.h"
#include "ripper/common.h"
#include "ripper/diag.h"
#include "ripper/log.h"
#include "ripper/util.h"
#include "ripper/zip.h"
#include "svc.h"

static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static struct {
  int running;
  char state[16];  // idle, running, done, failed
  char drive[16];
  char step[64];
  int done;
  int total;
  char file[128];  // the zip, in the diag folder
  char error[256];
  struct diag_summary summary;
  volatile int cancel;
} g;

static void diag_dir(char *out, size_t n) {
  svc_path_join(out, n, plat_data_dir(), "diag");
}

static void progress(void *ud, const char *step, int done, int total) {
  (void)ud;
  pthread_mutex_lock(&g_lock);
  svc_str_copy(g.step, sizeof(g.step), step);
  g.done = done;
  g.total = total;
  pthread_mutex_unlock(&g_lock);
}

static void finish(const char *state, const char *error) {
  pthread_mutex_lock(&g_lock);
  svc_str_copy(g.state, sizeof(g.state), state);
  svc_str_copy(g.error, sizeof(g.error), error ? error : "");
  g.running = 0;
  pthread_mutex_unlock(&g_lock);
}

static void *diag_thread(void *arg) {
  struct drive_claim *claim = arg;
  struct diag_summary sum;
  char dir[SVC_PATH];
  char work[SVC_PATH];
  char zip[SVC_PATH + 8];
  char name[128];
  char path[SVC_PATH + 32];
  char stamp[32];
  char *log;
  time_t now = time(NULL);
  struct tm tm;
  int rc;

  localtime_r(&now, &tm);
  strftime(stamp, sizeof(stamp), "%Y%m%d-%H%M%S", &tm);
  snprintf(name, sizeof(name), "ripper-diag-%s-%s", claim->id, stamp);
  diag_dir(dir, sizeof(dir));
  svc_path_join(work, sizeof(work), dir, name);
  RIP_INFO("diagnostic of %s (%s) into %s", claim->id, claim->label, work);

  pthread_mutex_lock(claim->io);
  rc = diag_run(claim->t, work, progress, NULL, &g.cancel, &sum);
  pthread_mutex_unlock(claim->io);
  drives_release(claim->id, 0, "");

  if (rc == RIP_OK || rc == RIP_ECANCEL) {
    // The log of this session goes with it, and what the console is.
    log = svc_log_recent();
    snprintf(path, sizeof(path), "%s/ripper-log.txt", work);
    if (log) {
      rip_write_file(path, log, strlen(log));
    }
    free(log);
    snprintf(path, sizeof(path), "%s/about.txt", work);
    {
      char about[512];
      int n = snprintf(about, sizeof(about), "ripper %s (%s)\ndrive slot: %s (%s)\ntaken: %s\n", RIPPER_VERSION,
                       RIPPER_BUILD, claim->id, claim->label, stamp);
      rip_write_file(path, about, (size_t)n);
    }
    snprintf(zip, sizeof(zip), "%s.zip", work);
    if (zip_build_dir(work, zip, NULL, NULL) == RIP_OK) {
      svc_remove_tree(work);
      pthread_mutex_lock(&g_lock);
      snprintf(g.file, sizeof(g.file), "%.100s.zip", name);
      g.summary = sum;
      pthread_mutex_unlock(&g_lock);
      RIP_INFO("diagnostic done: %s, %d of %d commands taken", zip, sum.answered, sum.commands);
      finish(rc == RIP_OK ? "done" : "cancelled", NULL);
    } else {
      finish("failed", "couldn't zip the results");
    }
  } else {
    finish("failed", "couldn't write the results");
  }
  free(claim);
  return NULL;
}

int diagnose_start(const char *drive_id, char *err, size_t en) {
  struct drive_claim *claim = calloc(1, sizeof(*claim));
  char dir[SVC_PATH];

  if (!claim) {
    snprintf(err, en, "out of memory");
    return RIP_ENOMEM;
  }
  pthread_mutex_lock(&g_lock);
  if (g.running) {
    pthread_mutex_unlock(&g_lock);
    free(claim);
    snprintf(err, en, "a diagnostic is running already");
    return RIP_ERR;
  }
  pthread_mutex_unlock(&g_lock);
  if (drives_claim(drive_id, 0, 0, claim, err, en) != RIP_OK) {
    free(claim);
    return RIP_ERR;
  }
  diag_dir(dir, sizeof(dir));
  rip_mkdir_p(dir);
  pthread_mutex_lock(&g_lock);
  memset(&g, 0, sizeof(g));
  g.running = 1;
  svc_str_copy(g.state, sizeof(g.state), "running");
  svc_str_copy(g.drive, sizeof(g.drive), drive_id);
  svc_str_copy(g.step, sizeof(g.step), "starting");
  pthread_mutex_unlock(&g_lock);
  if (svc_thread(diag_thread, claim) != 0) {
    drives_release(drive_id, 0, "");
    free(claim);
    finish("failed", "couldn't start");
    snprintf(err, en, "couldn't start the diagnostic");
    return RIP_ERR;
  }
  return RIP_OK;
}

void diagnose_cancel(void) {
  g.cancel = 1;
}

cJSON *diagnose_status(void) {
  cJSON *o = cJSON_CreateObject();

  pthread_mutex_lock(&g_lock);
  cJSON_AddStringToObject(o, "state", g.state[0] ? g.state : "idle");
  cJSON_AddStringToObject(o, "drive", g.drive);
  cJSON_AddStringToObject(o, "step", g.step);
  cJSON_AddNumberToObject(o, "done", g.done);
  cJSON_AddNumberToObject(o, "total", g.total);
  cJSON_AddStringToObject(o, "file", g.file);
  cJSON_AddStringToObject(o, "error", g.error);
  cJSON_AddStringToObject(o, "drive_name", g.summary.drive);
  cJSON_AddStringToObject(o, "disc", g.summary.disc);
  cJSON_AddStringToObject(o, "findings", g.summary.findings);
  cJSON_AddNumberToObject(o, "commands", g.summary.commands);
  cJSON_AddNumberToObject(o, "answered", g.summary.answered);
  pthread_mutex_unlock(&g_lock);
  return o;
}

char *diagnose_file(const char *name, size_t *len) {
  char dir[SVC_PATH];
  char path[SVC_PATH + 160];
  size_t i;

  // A file of ours, nothing else: no paths.
  if (!name || strncmp(name, "ripper-diag-", 12) != 0 || strlen(name) > 120) {
    return NULL;
  }
  for (i = 0; name[i]; i++) {
    if (name[i] == '/' || name[i] == '\\' || (name[i] == '.' && name[i + 1] == '.')) {
      return NULL;
    }
  }
  diag_dir(dir, sizeof(dir));
  snprintf(path, sizeof(path), "%s/%s", dir, name);
  return svc_read_file(path, len);
}
