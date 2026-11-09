#include "jobs.h"

#include <pthread.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "catalog.h"
#include "deliver.h"
#include "drives.h"
#include "musicbrainz.h"
#include "platform.h"
#include "ripper/common.h"
#include "ripper/exfat.h"
#include "ripper/game.h"
#include "ripper/keydb.h"
#include "ripper/log.h"
#include "ripper/rip.h"
#include "ripper/ufs2.h"
#include "ripper/util.h"
#include "settings.h"
#include "stats.h"
#include "svc.h"

#define MAX_JOBS 50

enum job_state { JOB_RIPPING, JOB_DELIVERING, JOB_DONE, JOB_FAILED, JOB_CANCELLED };

static const char *STATE_NAMES[] = {"ripping", "delivering", "done", "failed", "cancelled"};

struct job {
  int id;
  enum job_state state;
  char phase[24];
  char drive[16];
  char drive_label[64];
  char title_id[16];  // an installed game to dump, instead of a drive's disc
  char system[16];
  char system_name[48];
  char format[16];
  char format_label[64];
  char dest_id[48];
  char dest_name[64];
  char name[256];
  char folder[512];
  char folder_tpl[256];  // from the request; "" takes the profile's
  char romm_platform[64];
  cJSON *options;
  cJSON *dest;  // a copy taken at the start, so edits in Settings don't change a running job
  unsigned disc_seq;
  int local;
  uint64_t done;
  uint64_t total;
  double rate;  // bytes per second
  uint64_t rate_at_ms;
  uint64_t rate_done;
  int64_t started;
  int64_t finished;
  char error[256];
  char note[256];  // done, but with something to know: sectors that couldn't be read
  char where[512];
  char dir[SVC_PATH];
  char files[4][256];
  int file_count;
  int ripped;          // the files exist: a failed delivery can be retried
  int staged;          // the rip went to staging, and a transfer stage takes it to the destination
  uint64_t rip_total;  // the rip stage's size and time, kept once the transfer starts
  int64_t rip_secs;
  uint64_t xfer_total;
  volatile int cancel;
};

static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static struct job *g_jobs[MAX_JOBS];
static int g_next_id = 1;
static int64_t g_awake_until;  // asked for through the API: the console stays up until then

static void history_path(char *out, size_t n) {
  svc_path_join(out, n, plat_data_dir(), "jobs.json");
}

static cJSON *job_json(const struct job *j) {
  cJSON *o = cJSON_CreateObject();
  cJSON *files = cJSON_AddArrayToObject(o, "files");
  int i;

  cJSON_AddNumberToObject(o, "id", j->id);
  cJSON_AddStringToObject(o, "state", STATE_NAMES[j->state]);
  cJSON_AddStringToObject(o, "phase", j->phase);
  cJSON_AddStringToObject(o, "drive", j->drive);
  cJSON_AddStringToObject(o, "drive_label", j->drive_label);
  cJSON_AddStringToObject(o, "system", j->system);
  cJSON_AddStringToObject(o, "system_name", j->system_name);
  cJSON_AddStringToObject(o, "format", j->format);
  cJSON_AddStringToObject(o, "format_label", j->format_label);
  cJSON_AddStringToObject(o, "destination", j->dest_id);
  cJSON_AddStringToObject(o, "destination_name", j->dest_name);
  cJSON_AddStringToObject(o, "name", j->name);
  cJSON_AddNumberToObject(o, "done", (double)j->done);
  cJSON_AddNumberToObject(o, "total", (double)j->total);
  cJSON_AddNumberToObject(o, "rate", j->rate);
  cJSON_AddNumberToObject(o, "eta", j->rate > 1 && j->total > j->done ? (double)(j->total - j->done) / j->rate : 0);
  cJSON_AddNumberToObject(o, "started", (double)j->started);
  cJSON_AddNumberToObject(o, "finished", (double)j->finished);
  cJSON_AddStringToObject(o, "error", j->error);
  cJSON_AddStringToObject(o, "where", j->where);
  cJSON_AddStringToObject(o, "note", j->note);
  cJSON_AddBoolToObject(o, "retryable", j->state == JOB_FAILED && j->ripped && j->staged);
  // The two stages, each with its own numbers: reading the disc, then taking
  // the rip to its destination (a rename, a copy or an upload).
  cJSON_AddStringToObject(o, "stage", j->state == JOB_RIPPING ? "rip" : j->state == JOB_DELIVERING ? "transfer" : "");
  {
    cJSON *rip = cJSON_AddObjectToObject(o, "rip");
    int ripping = j->state == JOB_RIPPING;
    cJSON_AddNumberToObject(rip, "done", (double)(ripping ? j->done : j->ripped ? j->rip_total : j->done));
    cJSON_AddNumberToObject(rip, "total", (double)(ripping ? j->total : j->ripped ? j->rip_total : j->total));
    cJSON_AddNumberToObject(rip, "seconds", (double)(ripping ? svc_now() - j->started : j->rip_secs));
    if (j->staged) {
      cJSON *x = cJSON_AddObjectToObject(o, "transfer");
      int moving = j->state == JOB_DELIVERING;
      cJSON_AddStringToObject(x, "kind", j->local ? "move" : "upload");
      cJSON_AddNumberToObject(x, "done", (double)(moving ? j->done : j->state == JOB_DONE ? j->xfer_total : 0));
      cJSON_AddNumberToObject(x, "total", (double)(moving ? j->total : j->xfer_total));
      cJSON_AddNumberToObject(x, "rate", moving ? j->rate : 0);
    }
  }
  // What's needed to send it again after a restart.
  cJSON_AddBoolToObject(o, "ripped", j->ripped);
  cJSON_AddBoolToObject(o, "staged", j->staged);
  cJSON_AddBoolToObject(o, "local", j->local);
  cJSON_AddStringToObject(o, "dir", j->dir);
  cJSON_AddStringToObject(o, "folder", j->folder);
  cJSON_AddStringToObject(o, "romm_platform", j->romm_platform);
  cJSON_AddNumberToObject(o, "rip_total", (double)j->rip_total);
  cJSON_AddNumberToObject(o, "rip_secs", (double)j->rip_secs);
  for (i = 0; i < j->file_count; i++) {
    char path[SVC_PATH];
    cJSON *f = cJSON_CreateObject();
    svc_path_join(path, sizeof(path), j->dir, j->files[i]);
    cJSON_AddStringToObject(f, "name", j->files[i]);
    cJSON_AddNumberToObject(f, "size", (double)svc_file_size(path));
    cJSON_AddItemToArray(files, f);
  }
  return o;
}

// The jobs, for the queue page after a restart. Caller holds the lock.
static void save_history(void) {
  cJSON *arr = cJSON_CreateArray();
  char path[SVC_PATH];
  int i;

  for (i = 0; i < MAX_JOBS; i++) {
    // Running ones too: one cut short by a crash comes back failed, not gone.
    if (g_jobs[i]) {
      cJSON_AddItemToArray(arr, job_json(g_jobs[i]));
    }
  }
  history_path(path, sizeof(path));
  jsave(path, arr);
  cJSON_Delete(arr);
}

// While anything rips or uploads, the console is kept out of rest mode.
static void *awake_thread(void *arg) {
  (void)arg;
  for (;;) {
    int busy = 0;
    int i;
    sleep(30);
    pthread_mutex_lock(&g_lock);
    for (i = 0; i < MAX_JOBS; i++) {
      busy |= g_jobs[i] && (g_jobs[i]->state == JOB_RIPPING || g_jobs[i]->state == JOB_DELIVERING);
    }
    busy |= svc_now() < g_awake_until;
    pthread_mutex_unlock(&g_lock);
    if (busy) {
      plat_keep_awake();
    }
  }
  return NULL;
}

// One job for a notification: "Built-in Blu-ray: 45% · 2.8 MB/s · 12 min left".
// Caller holds the lock.
static int progress_line(const struct job *j, char *out, size_t n) {
  int sending = j->state == JOB_DELIVERING;
  char pct[24];
  char speed[32] = "";
  char left[48];

  if (j->total) {
    snprintf(pct, sizeof(pct), "%s%d%%", sending ? "sending " : "", (int)(100.0 * (double)j->done / (double)j->total));
  } else {
    snprintf(pct, sizeof(pct), "%s", sending ? "sending" : "starting");
  }
  if (j->rate > 1) {
    snprintf(speed, sizeof(speed), " · %.1f MB/s", j->rate / 1e6);
  }
  svc_time_left(j->rate > 1 && j->total > j->done ? (double)(j->total - j->done) / j->rate : 0, left, sizeof(left));
  return snprintf(out, n, "%s: %s%s%s", j->drive_label[0] ? j->drive_label : "Game dump", pct, speed, left);
}

// What's running, for a notification: a line per job. Jobs under 15 s old are
// left out until their speed settles. The number of jobs told.
static int progress_text(char *title, size_t tn, char *body, size_t bn) {
  const struct job *first = NULL;
  int n = 0;
  size_t used = 0;
  int i;

  body[0] = '\0';
  pthread_mutex_lock(&g_lock);
  for (i = 0; i < MAX_JOBS; i++) {
    const struct job *j = g_jobs[i];
    int w;
    if (!j || (j->state != JOB_RIPPING && j->state != JOB_DELIVERING) || svc_now() - j->started < 15) {
      continue;
    }
    if (n && used + 1 < bn) {
      body[used++] = '\n';
      body[used] = '\0';
    }
    w = used < bn ? progress_line(j, body + used, bn - used) : 0;
    used = w > 0 && used + (size_t)w < bn ? used + (size_t)w : bn;
    first = first ? first : j;
    n++;
  }
  if (n == 1) {
    snprintf(title, tn, "%s %s", first->state == JOB_DELIVERING ? "Sending" : "Ripping", first->name);
  } else if (n > 1) {
    snprintf(title, tn, "Ripping %d discs", n);
  }
  pthread_mutex_unlock(&g_lock);
  return n;
}

// While RipperPS isn't in front (or isn't running), what's ripping is told in
// a notification: once it's been away a few seconds (not for a quick look at
// the home screen), then every few minutes.
#define PROGRESS_AWAY_S 10
#define PROGRESS_EVERY_S 300

static void *progress_thread(void *arg) {
  int64_t away_since = 0;  // 0 while RipperPS is in front
  int64_t last = 0;        // the last notification, 0 since RipperPS was last in front

  (void)arg;
  for (;;) {
    char title[300];
    char body[256];
    int64_t now;
    sleep(5);
    now = svc_now();
    if (plat_app_in_front()) {
      away_since = 0;
      last = 0;
      continue;
    }
    if (!away_since) {
      away_since = now;
    }
    if (!settings_notify() || now - away_since < PROGRESS_AWAY_S || (last && now - last < PROGRESS_EVERY_S)) {
      continue;
    }
    if (progress_text(title, sizeof(title), body, sizeof(body)) > 0) {
      plat_notify(title, body);
      last = now;
    }
  }
  return NULL;
}

void jobs_init(void) {
  char path[SVC_PATH];
  cJSON *arr;
  cJSON *o;
  int i = 0;

  history_path(path, sizeof(path));
  arr = jload(path);
  pthread_mutex_lock(&g_lock);
  cJSON_ArrayForEach(o, arr) {
    struct job *j = calloc(1, sizeof(*j));
    static const struct {
      const char *key;
      size_t off;
      size_t len;
    } strs[] = {
#define F(k, f) {k, offsetof(struct job, f), sizeof(((struct job *)0)->f)}
        F("phase", phase),
        F("drive", drive),
        F("drive_label", drive_label),
        F("system", system),
        F("system_name", system_name),
        F("format", format),
        F("format_label", format_label),
        F("destination", dest_id),
        F("destination_name", dest_name),
        F("name", name),
        F("error", error),
        F("where", where),
        F("note", note),
        F("dir", dir),
        F("folder", folder),
        F("romm_platform", romm_platform),
#undef F
    };
    size_t k;
    const char *st = jstr(o, "state", "done");
    if (!j || i >= MAX_JOBS) {
      free(j);
      break;
    }
    j->id = jint(o, "id", 0);
    for (k = 0; k < RIP_ARRAY_LEN(strs); k++) {
      svc_str_copy((char *)j + strs[k].off, strs[k].len, jstr(o, strs[k].key, ""));
    }
    j->state = !strcmp(st, "failed") ? JOB_FAILED : !strcmp(st, "cancelled") ? JOB_CANCELLED : JOB_DONE;
    j->ripped = jbool(o, "ripped", 0);
    j->staged = jbool(o, "staged", 0);
    j->local = jbool(o, "local", 0);
    j->rip_total = (uint64_t)cJSON_GetNumberValue(cJSON_GetObjectItemCaseSensitive(o, "rip_total"));
    j->rip_secs = (int64_t)cJSON_GetNumberValue(cJSON_GetObjectItemCaseSensitive(o, "rip_secs"));
    if (!strcmp(st, "ripping") || !strcmp(st, "delivering")) {
      // ripper stopped (a crash, the console turned off) with it running.
      j->state = JOB_FAILED;
      svc_str_copy(j->error, sizeof(j->error),
                   !strcmp(st, "ripping") ? "RipperPS stopped during the rip" : "RipperPS stopped while sending it");
      j->ripped = j->ripped && !strcmp(st, "delivering");
      j->finished = svc_now();
    }
    if (j->state == JOB_FAILED && j->ripped && j->staged) {
      // Sent again to the destination it had, as Settings has it now.
      j->dest = settings_destination(j->dest_id);
      j->ripped = j->dest != NULL && svc_dir_exists(j->dir);
      for (k = 0; j->ripped && k < 4 && k < (size_t)cJSON_GetArraySize(cJSON_GetObjectItemCaseSensitive(o, "files"));
           k++) {
        const cJSON *f = cJSON_GetArrayItem(cJSON_GetObjectItemCaseSensitive(o, "files"), (int)k);
        svc_str_copy(j->files[k], sizeof(j->files[0]), jstr(f, "name", ""));
        j->file_count = (int)k + 1;
      }
    }
    j->done = (uint64_t)jint(o, "done", 0);
    j->total = (uint64_t)jint(o, "total", 0);
    j->started = (int64_t)cJSON_GetNumberValue(cJSON_GetObjectItemCaseSensitive(o, "started"));
    if (!j->finished) {
      j->finished = (int64_t)cJSON_GetNumberValue(cJSON_GetObjectItemCaseSensitive(o, "finished"));
    }
    if (j->id >= g_next_id) {
      g_next_id = j->id + 1;
    }
    g_jobs[i++] = j;
  }
  save_history();
  pthread_mutex_unlock(&g_lock);
  cJSON_Delete(arr);
  svc_thread(awake_thread, NULL);
  svc_thread(progress_thread, NULL);
}

/* Progress */

static void set_phase(struct job *j, const char *phase, uint64_t done, uint64_t total) {
  uint64_t now = svc_mono_ms();

  pthread_mutex_lock(&g_lock);
  if (strcmp(j->phase, phase) != 0) {
    svc_str_copy(j->phase, sizeof(j->phase), phase);
    j->rate = 0;
    j->rate_at_ms = now;
    j->rate_done = done;
  } else if (now - j->rate_at_ms >= 1000) {
    double inst = (double)(done - j->rate_done) * 1000.0 / (double)(now - j->rate_at_ms);
    j->rate = j->rate > 0 ? j->rate * 0.7 + inst * 0.3 : inst;
    j->rate_at_ms = now;
    j->rate_done = done;
  }
  j->done = done;
  j->total = total;
  pthread_mutex_unlock(&g_lock);
}

static void rip_progress(const char *phase, uint64_t done, uint64_t total, void *user) {
  set_phase(user, phase, done, total);
}

static int deliver_progress(void *ud, uint64_t done, uint64_t total) {
  struct job *j = ud;

  set_phase(j, j->local ? "moving" : "uploading", done, total);
  return j->cancel;
}

static void finish(struct job *j, enum job_state st, const char *error) {
  char title[320];

  pthread_mutex_lock(&g_lock);
  j->state = st;
  j->finished = svc_now();
  svc_str_copy(j->error, sizeof(j->error), error ? error : "");
  save_history();
  pthread_mutex_unlock(&g_lock);
  if (st == JOB_DONE) {
    stats_record(j->system, j->rip_total ? j->rip_total : j->total, j->rip_secs ? j->rip_secs : svc_now() - j->started,
                 j->xfer_total, 1);
    RIP_INFO("job %d done: %s -> %s", j->id, j->name, j->where);
  } else {
    if (st == JOB_FAILED) {
      stats_record(j->system, 0, 0, 0, 0);
    }
    RIP_WARN("job %d %s: %s", j->id, STATE_NAMES[st], error ? error : "");
  }
  if (settings_notify() && st != JOB_CANCELLED) {
    snprintf(title, sizeof(title), "%s: %s", st == JOB_DONE ? "Ripped" : "Rip failed", j->name);
    plat_notify(title, st == JOB_DONE ? j->dest_name : j->error);
  }
}

/* The work */

static void choose_staging(struct job *j, uint64_t size);

static void deliver_files(struct job *j) {
  const char *names[4];
  struct deliver_req r;
  char where[512] = "";
  char err[256] = "";
  int i;
  int rc;

  pthread_mutex_lock(&g_lock);
  if (j->state == JOB_RIPPING) {
    j->rip_total = j->total;
    j->rip_secs = svc_now() - j->started;
  }
  j->state = JOB_DELIVERING;
  j->phase[0] = '\0';
  j->done = 0;
  j->total = 0;
  save_history();
  pthread_mutex_unlock(&g_lock);
  set_phase(j, j->local ? "moving" : "uploading", 0, 0);
  for (i = 0; i < j->file_count; i++) {
    names[i] = j->files[i];
  }
  memset(&r, 0, sizeof(r));
  r.dest = j->dest;
  r.dir = j->dir;
  r.files = names;
  r.count = j->file_count;
  r.folder = j->folder;
  r.name = j->name;
  r.romm_platform = j->romm_platform;
  r.progress = deliver_progress;
  r.ud = j;
  rc = deliver_run(&r, where, sizeof(where), err, sizeof(err));
  if (j->cancel) {
    finish(j, JOB_CANCELLED, "cancelled");
    return;
  }
  if (rc != RIP_OK) {
    finish(j, JOB_FAILED, err[0] ? err : "delivery failed");
    return;
  }
  pthread_mutex_lock(&g_lock);
  svc_str_copy(j->where, sizeof(j->where), where);
  j->xfer_total = j->total;
  pthread_mutex_unlock(&g_lock);
  svc_remove_tree(j->dir);  // the staging copy (empty, after a local move)
  finish(j, JOB_DONE, NULL);
}

static void *deliver_thread(void *arg) {
  deliver_files(arg);
  return NULL;
}

static int dump_progress(void *ud, const char *phase, uint64_t done, uint64_t total) {
  struct job *j = ud;

  set_phase(j, phase, done, total);
  return j->cancel;
}

static int pack_progress(void *ud, uint64_t done, uint64_t total) {
  struct job *j = ud;

  set_phase(j, "packing", done, total);
  return j->cancel;
}

static int finish_progress(void *ud, unsigned done, unsigned total) {
  struct job *j = ud;

  set_phase(j, "signing", done, total);
  return j->cancel;
}

// PS4/PS5: the console decrypts the installed copy; the drive isn't read, so
// it's let go at once. The game comes out as a folder named like the rip.
static void game_dump(struct job *j, const char *title_id) {
  char out[SVC_PATH];
  char err[256] = "";
  int rc;

  if (!title_id[0]) {
    finish(j, JOB_FAILED, "the disc's title ID couldn't be read");
    return;
  }
  if (rip_mkdir_p(j->dir) != 0) {
    finish(j, JOB_FAILED, "can't create the output folder");
    return;
  }
  svc_path_join(out, sizeof(out), j->dir, j->name);
  rc = plat_game_dump(title_id, out, dump_progress, j, &j->cancel, err, sizeof(err));
  if (rc == RIP_OK) {
    // Ready for where it runs: fake-signed and backported if asked.
    struct game_finish fin;
    catalog_game_finish(!strncmp(title_id, "PPSA", 4) ? "ps5" : "ps4", j->options, &fin);
    rc = game_finish_folder(out, &fin, finish_progress, j, NULL);
    if (rc == RIP_ERR) {
      snprintf(err, sizeof(err), "the dump is done, but its executables couldn't be fake-signed");
    }
  }
  if (rc == RIP_OK && strcmp(catalog_game_container(j->options), "folder") != 0) {
    // The folder becomes an image, the game at its root, and goes.
    const char *kind = catalog_game_container(j->options);
    int ufs = !strcmp(kind, "ffpkg");
    char image[SVC_PATH + 8];
    char label[16];
    snprintf(image, sizeof(image), "%s.%s", out, kind);
    svc_str_copy(label, sizeof(label), title_id);
    rc = ufs ? ufs2_build(out, image, label, pack_progress, j) : exfat_build(out, image, label, pack_progress, j);
    if (rc == RIP_OK) {
      svc_remove_tree(out);
      pthread_mutex_lock(&g_lock);
      j->file_count = 1;
      snprintf(j->files[0], sizeof(j->files[0]), "%.240s.%s", j->name, kind);
      pthread_mutex_unlock(&g_lock);
      svc_str_copy(out, sizeof(out), image);
    } else {
      unlink(image);
      snprintf(err, sizeof(err), "the dump is done, but the %s image couldn't be made", ufs ? "UFS2" : "exFAT");
    }
  }
  if (rc != RIP_OK) {
    if (j->staged || rc == RIP_ECANCEL) {
      svc_remove_tree(out);
    }
    finish(j, rc == RIP_ECANCEL ? JOB_CANCELLED : JOB_FAILED, rc == RIP_ECANCEL ? "cancelled" : err);
    return;
  }
  pthread_mutex_lock(&g_lock);
  if (!j->file_count) {
    j->file_count = 1;
    svc_str_copy(j->files[0], sizeof(j->files[0]), j->name);
  }
  j->ripped = 1;
  pthread_mutex_unlock(&g_lock);
  if (!j->staged) {
    pthread_mutex_lock(&g_lock);
    svc_str_copy(j->where, sizeof(j->where), out);
    j->rip_total = j->total;
    j->rip_secs = svc_now() - j->started;
    pthread_mutex_unlock(&g_lock);
    finish(j, JOB_DONE, NULL);
  } else {
    deliver_files(j);
  }
}

// An installed game, no disc involved: the profile of its console and straight to the dump.
static void *installed_thread(void *arg) {
  struct job *j = arg;
  const struct cat_system *sys = catalog_system(!strncmp(j->title_id, "PPSA", 4) ? "ps5" : "ps4");
  cJSON *profile = settings_profile(sys->id);
  const char *tpl = j->folder_tpl[0] ? j->folder_tpl : jstr(profile, "folder", "");
  const char *vars[] = {"system", sys->id, "system_name", sys->name, "name", j->name, "title", j->name,
                        "artist", "",      "album",       "",        NULL};

  pthread_mutex_lock(&g_lock);
  svc_str_copy(j->system, sizeof(j->system), sys->id);
  svc_str_copy(j->system_name, sizeof(j->system_name), sys->name);
  svc_str_copy(j->format_label, sizeof(j->format_label), "Decrypted game folder");
  svc_str_copy(j->drive_label, sizeof(j->drive_label), "Installed game");
  svc_template(j->folder, sizeof(j->folder), tpl, vars);
  pthread_mutex_unlock(&g_lock);
  cJSON_Delete(profile);
  choose_staging(j, 0);
  game_dump(j, j->title_id);
  return NULL;
}

static const char *base_name(const char *path) {
  const char *s = strrchr(path, '/');

  return s ? s + 1 : path;
}

// Where the rip is written. Staging, so that taking it to the destination is a
// stage of its own; for a local destination on another filesystem only when
// staging has room for it (a 40 GB dump is written where it goes otherwise).
static void choose_staging(struct job *j, uint64_t size) {
  char staging[SVC_PATH];
  char sub[32];
  char dest[SVC_PATH];
  struct stat a;
  struct stat b;
  int stage = 1;

  settings_staging_dir(staging, sizeof(staging));
  if (j->local) {
    // Compared on the destination's root, so a rip that fails leaves no empty
    // folder behind; the folder itself is made when the rip goes there.
    svc_path_join(dest, sizeof(dest), jstr(j->dest, "path", ""), j->folder);
    rip_mkdir_p(jstr(j->dest, "path", ""));
    rip_mkdir_p(staging);
    if (stat(staging, &a) != 0 || stat(jstr(j->dest, "path", ""), &b) != 0) {
      stage = 0;
    } else if (a.st_dev != b.st_dev) {
      int64_t room = svc_free_space(staging);
      stage = !size || (room >= 0 && (uint64_t)room > size + size / 10 + (256u << 20));
    }
  }
  pthread_mutex_lock(&g_lock);
  j->staged = stage;
  if (stage) {
    snprintf(sub, sizeof(sub), "job-%d", j->id);
    svc_path_join(j->dir, sizeof(j->dir), staging, sub);
  } else {
    svc_str_copy(j->dir, sizeof(j->dir), dest);
  }
  save_history();
  pthread_mutex_unlock(&g_lock);
}

static void *job_thread(void *arg) {
  struct job *j = arg;
  struct drive_claim claim;
  struct rip_reader reader;
  struct rip_keydb keys;
  struct rip_job_opts opts;
  struct rip_job_result *res = calloc(1, sizeof(*res));
  struct rip_audio_meta *meta = NULL;
  const struct rip_recipe *recipe = NULL;
  struct rip_choice choices[8];
  enum rip_recipe_id rid;
  char keys_path[SVC_PATH];
  char err[256] = "";
  size_t n;
  size_t i;
  int rc;

  memset(&keys, 0, sizeof(keys));
  if (!res || drives_claim(j->drive, j->disc_seq, j->id, &claim, err, sizeof(err)) != RIP_OK) {
    finish(j, JOB_FAILED, err[0] ? err : "out of memory");
    free(res);
    return NULL;
  }
  // The recipe the request named, as this drive offers it for this disc.
  catalog_recipe_id(j->format, &rid);
  n = rip_plan(claim.drive, &claim.disc, choices, RIP_ARRAY_LEN(choices));
  for (i = 0; i < n; i++) {
    if (choices[i].recipe->id == rid && choices[i].avail == RIP_AVAIL_OK) {
      recipe = choices[i].recipe;
    }
  }
  if (!recipe) {
    drives_release(j->drive, 0, "");
    finish(j, JOB_FAILED, "that format isn't available for this disc");
    free(res);
    drives_claim_free(&claim);
    return NULL;
  }
  // An audio CD: what it is, from MusicBrainz, for its folder, names and tags.
  if (rip_recipe_is_audio(recipe->id) && claim.disc.musicbrainz_id[0]) {
    const cJSON *lk = cJSON_GetObjectItemCaseSensitive(j->options, "lookup");
    int lookup = !lk || cJSON_IsTrue(lk) || (cJSON_IsNumber(lk) && lk->valueint) ||
                 (cJSON_IsString(lk) && lk->valuestring[0] == '1');
    if (lookup && claim.audio) {  // found when the disc went in
      meta = claim.audio;
      claim.audio = NULL;
    } else if (lookup) {
      set_phase(j, "looking up", 0, 0);
      meta = calloc(1, sizeof(*meta));
      if (meta && mb_lookup(claim.disc.musicbrainz_id, meta, err, sizeof(err)) != RIP_OK) {
        RIP_WARN("%s; the tracks keep plain names", err);
        free(meta);
        meta = NULL;
      }
      err[0] = '\0';
    }
    // The album's name, unless the rip was given one of its own.
    if (meta && meta->album[0] && (!strcmp(j->name, "Disc") || !j->name[0])) {
      pthread_mutex_lock(&g_lock);
      svc_str_copy(j->name, sizeof(j->name), meta->album);
      rip_sanitize_name(j->name, sizeof(j->name));
      pthread_mutex_unlock(&g_lock);
    }
  }
  {
    // What the disc is decides the profile the folder and RomM platform come from.
    const struct cat_system *sys = catalog_system_for(claim.disc.kind);
    cJSON *profile = sys ? settings_profile(sys->id) : NULL;
    const char *tpl = j->folder_tpl[0] ? j->folder_tpl : jstr(profile, "folder", "");
    pthread_mutex_lock(&g_lock);
    svc_str_copy(j->format_label, sizeof(j->format_label), recipe->output);
    svc_str_copy(j->drive_label, sizeof(j->drive_label), claim.label);
    if (sys) {
      svc_str_copy(j->system, sizeof(j->system), sys->id);
      svc_str_copy(j->system_name, sizeof(j->system_name), sys->name);
    }
    if (!j->romm_platform[0]) {
      svc_str_copy(j->romm_platform, sizeof(j->romm_platform),
                   jstr(profile, "romm_platform", sys ? sys->romm_slug : ""));
    }
    {
      char artist[256];
      const char *vars[] = {"system", j->system, "system_name", j->system_name, "name",  j->name, "title",
                            j->name,  "artist",  artist,        "album",        j->name, NULL};
      svc_str_copy(artist, sizeof(artist), meta && meta->artist[0] ? meta->artist : "Unknown Artist");
      rip_sanitize_name(artist, sizeof(artist));
      svc_template(j->folder, sizeof(j->folder), tpl, vars);
    }
    pthread_mutex_unlock(&g_lock);
    cJSON_Delete(profile);
  }
  choose_staging(j, (uint64_t)claim.disc.sector_count * claim.disc.sector_size);

  if (rip_mkdir_p(j->dir) != 0) {
    drives_release(j->drive, 0, "");
    finish(j, JOB_FAILED, "can't create the output folder");
    free(meta);
    free(res);
    drives_claim_free(&claim);
    return NULL;
  }
  if (recipe->id == RIP_RECIPE_SYSTEM_DUMP) {
    // PS4/PS5: the console decrypts the installed copy; the disc isn't read.
    char title_id[32];
    int done;
    svc_str_copy(title_id, sizeof(title_id), claim.disc.title_id);
    // Not ejected yet: a disc game won't start without its disc, and the dump starts it.
    drives_release(j->drive, 0, "Dumping the installed game");
    game_dump(j, title_id);
    pthread_mutex_lock(&g_lock);
    done = j->state == JOB_DONE;
    pthread_mutex_unlock(&g_lock);
    if (done) {
      drives_dumped(j->drive, claim.disc_seq);
    }
    free(res);
    drives_claim_free(&claim);
    return NULL;
  }
  settings_ps3_keys(keys_path, sizeof(keys_path));
  if (recipe->id == RIP_RECIPE_PS3_ISO && rip_keydb_load(&keys, keys_path) != RIP_OK) {
    RIP_WARN("no PS3 key file at %s; the image will stay encrypted", keys_path);
  }
  memset(&opts, 0, sizeof(opts));
  opts.out_dir = j->dir;
  opts.name = j->name;
  opts.ps3_keys = keys.count ? &keys : NULL;
  opts.read_retries = settings_read_retries();
  opts.progress = rip_progress;
  opts.user = j;
  opts.cancel = &j->cancel;
  {
    static char css_cache[SVC_PATH];
    svc_path_join(css_cache, sizeof(css_cache), plat_data_dir(), "dvdcss");
    opts.css_cache_dir = css_cache;
  }
  catalog_job_options(j->format, j->options, &opts);
  opts.audio = meta;
  opts.cover = claim.cover;
  opts.cover_len = claim.cover_len;

  pthread_mutex_lock(claim.io);
  rc = plat_drive_reader(claim.id, claim.t, &reader);
  if (rc == RIP_OK) {
    rc = rip_run(recipe, claim.drive, &reader, &claim.disc, &opts, res);
    rip_reader_close(&reader);
  }
  pthread_mutex_unlock(claim.io);
  rip_keydb_free(&keys);
  free(meta);
  meta = NULL;
  drives_claim_free(&claim);
  drives_release(j->drive, rc == RIP_OK, rc == RIP_OK ? "Ripped" : "");

  if (rc != RIP_OK) {
    if (j->staged) {
      svc_remove_tree(j->dir);
    }
    finish(j, rc == RIP_ECANCEL ? JOB_CANCELLED : JOB_FAILED,
           rc == RIP_ECANCEL ? "cancelled" : "the rip failed; the log has the details");
    free(res);
    return NULL;
  }

  pthread_mutex_lock(&g_lock);
  j->file_count = 0;
  if (res->image_path[0]) {
    svc_str_copy(j->files[j->file_count++], sizeof(j->files[0]), base_name(res->image_path));
  }
  if (res->encrypted_path[0]) {
    svc_str_copy(j->files[j->file_count++], sizeof(j->files[0]), base_name(res->encrypted_path));
  }
  // The manifest, when it's beside the rip (an album keeps its own inside).
  if (res->manifest_path[0] && !strncmp(res->manifest_path, j->dir, strlen(j->dir)) &&
      !strchr(res->manifest_path + strlen(j->dir) + 1, '/')) {
    svc_str_copy(j->files[j->file_count++], sizeof(j->files[0]), base_name(res->manifest_path));
  }
  svc_str_copy(j->name, sizeof(j->name), res->name);  // a Redump name, when the key file had one
  if (res->unreadable) {
    snprintf(j->note, sizeof(j->note), "%llu sector%s couldn't be read and %s zeros in the image (a scratched disc?)",
             (unsigned long long)res->unreadable, res->unreadable == 1 ? "" : "s", res->unreadable == 1 ? "is" : "are");
  }
  j->ripped = 1;
  pthread_mutex_unlock(&g_lock);
  free(res);

  if (!j->staged) {
    pthread_mutex_lock(&g_lock);
    svc_str_copy(j->where, sizeof(j->where), j->dir);
    j->rip_total = j->total;
    j->rip_secs = svc_now() - j->started;
    pthread_mutex_unlock(&g_lock);
    finish(j, JOB_DONE, NULL);
  } else {
    deliver_files(j);
  }
  return NULL;
}

/* API */

// Room for one more: the oldest finished job goes. Caller holds the lock.
static int free_slot(void) {
  int i;
  int oldest = -1;

  for (i = 0; i < MAX_JOBS; i++) {
    if (!g_jobs[i]) {
      return i;
    }
    if (g_jobs[i]->state >= JOB_DONE && (oldest < 0 || g_jobs[i]->id < g_jobs[oldest]->id)) {
      oldest = i;
    }
  }
  if (oldest >= 0) {
    cJSON_Delete(g_jobs[oldest]->options);
    cJSON_Delete(g_jobs[oldest]->dest);
    free(g_jobs[oldest]);
    g_jobs[oldest] = NULL;
  }
  return oldest;
}

int jobs_start(const cJSON *req, int *job_id, char *err, size_t en) {
  const char *drive = jstr(req, "drive", "");
  const char *format = jstr(req, "format", "");
  const char *dest_id = jstr(req, "destination", "");
  const cJSON *opts = cJSON_GetObjectItemCaseSensitive(req, "options");
  enum rip_recipe_id rid;
  cJSON *dest;
  struct job *j;
  char name[256];
  int slot;

  err[0] = '\0';
  if (catalog_recipe_id(format, &rid) != 0) {
    snprintf(err, en, "unknown format '%s'", format);
    return RIP_ERR;
  }
  dest = settings_destination(dest_id);
  if (!dest) {
    snprintf(err, en, "no destination '%s'; add it in Settings", dest_id);
    return RIP_ERR;
  }
  svc_str_copy(name, sizeof(name), jstr(req, "name", ""));
  rip_sanitize_name(name, sizeof(name));
  if (!name[0] || !strcmp(name, "_")) {
    snprintf(err, en, "the rip needs a name");
    cJSON_Delete(dest);
    return RIP_ERR;
  }
  j = calloc(1, sizeof(*j));
  if (!j) {
    cJSON_Delete(dest);
    return RIP_ENOMEM;
  }
  svc_str_copy(j->drive, sizeof(j->drive), drive);
  svc_str_copy(j->title_id, sizeof(j->title_id), jstr(req, "title_id", ""));
  if (!drive[0] && (!j->title_id[0] || strcmp(format, "dump") != 0)) {
    snprintf(err, en, "say which drive's disc to rip, or which installed game to dump");
    cJSON_Delete(dest);
    free(j);
    return RIP_ERR;
  }
  svc_str_copy(j->format, sizeof(j->format), format);
  svc_str_copy(j->dest_id, sizeof(j->dest_id), dest_id);
  svc_str_copy(j->dest_name, sizeof(j->dest_name), jstr(dest, "name", dest_id));
  svc_str_copy(j->name, sizeof(j->name), name);
  svc_str_copy(j->romm_platform, sizeof(j->romm_platform), jstr(req, "romm_platform", ""));
  svc_str_copy(j->folder_tpl, sizeof(j->folder_tpl), jstr(req, "folder", ""));
  j->options = cJSON_IsObject(opts) ? cJSON_Duplicate(opts, 1) : cJSON_CreateObject();
  j->dest = dest;
  j->disc_seq = (unsigned)jint(req, "disc_seq", 0);
  j->local = deliver_is_local(dest);
  j->state = JOB_RIPPING;
  j->started = svc_now();

  pthread_mutex_lock(&g_lock);
  slot = free_slot();
  if (slot < 0) {
    pthread_mutex_unlock(&g_lock);
    snprintf(err, en, "too many jobs running");
    cJSON_Delete(j->options);
    cJSON_Delete(j->dest);
    free(j);
    return RIP_ERR;
  }
  j->id = g_next_id++;
  g_jobs[slot] = j;
  pthread_mutex_unlock(&g_lock);

  RIP_INFO("job %d: %s from %s as %s to %s", j->id, name, drive[0] ? drive : j->title_id, format, j->dest_name);
  if (svc_thread(drive[0] ? job_thread : installed_thread, j) != 0) {
    finish(j, JOB_FAILED, "could not start a thread");
    snprintf(err, en, "could not start the rip");
    return RIP_ERR;
  }
  if (job_id) {
    *job_id = j->id;
  }
  return RIP_OK;
}

cJSON *jobs_json(void) {
  cJSON *arr = cJSON_CreateArray();
  struct job *sorted[MAX_JOBS];
  int n = 0;
  int i;
  int k;

  pthread_mutex_lock(&g_lock);
  for (i = 0; i < MAX_JOBS; i++) {
    if (g_jobs[i]) {
      sorted[n++] = g_jobs[i];
    }
  }
  // Newest first.
  for (i = 1; i < n; i++) {
    struct job *x = sorted[i];
    for (k = i; k > 0 && sorted[k - 1]->id < x->id; k--) {
      sorted[k] = sorted[k - 1];
    }
    sorted[k] = x;
  }
  for (i = 0; i < n; i++) {
    cJSON_AddItemToArray(arr, job_json(sorted[i]));
  }
  pthread_mutex_unlock(&g_lock);
  return arr;
}

static struct job *find(int id) {
  int i;

  for (i = 0; i < MAX_JOBS; i++) {
    if (g_jobs[i] && g_jobs[i]->id == id) {
      return g_jobs[i];
    }
  }
  return NULL;
}

int jobs_cancel(int id, char *err, size_t en) {
  struct job *j;
  int rc = RIP_ERR;

  pthread_mutex_lock(&g_lock);
  j = find(id);
  if (!j) {
    snprintf(err, en, "no such job");
  } else if (j->state >= JOB_DONE) {
    snprintf(err, en, "the job already finished");
  } else {
    j->cancel = 1;
    rc = RIP_OK;
  }
  pthread_mutex_unlock(&g_lock);
  return rc;
}

int jobs_retry(int id, char *err, size_t en) {
  struct job *j;
  int ok;

  pthread_mutex_lock(&g_lock);
  j = find(id);
  ok = j && j->state == JOB_FAILED && j->ripped && j->staged && j->dest;
  if (ok) {
    j->cancel = 0;
    j->state = JOB_DELIVERING;
    j->error[0] = '\0';
  }
  pthread_mutex_unlock(&g_lock);
  if (!ok) {
    snprintf(err, en, "only a failed delivery can be tried again");
    return RIP_ERR;
  }
  return svc_thread(deliver_thread, j) == 0 ? RIP_OK : RIP_ERR;
}

void jobs_clear_finished(void) {
  int i;

  pthread_mutex_lock(&g_lock);
  for (i = 0; i < MAX_JOBS; i++) {
    struct job *j = g_jobs[i];
    // A failed delivery keeps its staged files until it's retried or cleared.
    if (j && j->state >= JOB_DONE) {
      if (j->state == JOB_FAILED && j->ripped && j->staged && j->dir[0]) {
        svc_remove_tree(j->dir);
      }
      cJSON_Delete(j->options);
      cJSON_Delete(j->dest);
      free(j);
      g_jobs[i] = NULL;
    }
  }
  save_history();
  pthread_mutex_unlock(&g_lock);
}

int jobs_active(void) {
  int i;
  int n = 0;

  pthread_mutex_lock(&g_lock);
  for (i = 0; i < MAX_JOBS; i++) {
    n += g_jobs[i] && g_jobs[i]->state < JOB_DONE;
  }
  pthread_mutex_unlock(&g_lock);
  return n;
}

void jobs_keep_awake(int seconds) {
  pthread_mutex_lock(&g_lock);
  g_awake_until = seconds > 0 ? svc_now() + seconds : 0;
  pthread_mutex_unlock(&g_lock);
  if (seconds > 0) {
    plat_keep_awake();
  }
}

int64_t jobs_awake_until(void) {
  return g_awake_until;
}
