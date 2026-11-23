#include "drivetest.h"

#include <ctype.h>
#include <dirent.h>
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
#include "settings.h"
#include "svc.h"

#define MAX_DISCS 30
#define TICK_MS 1000
#define REPORT_WAIT_MS 5000  // a disc the drive won't read gets its report just after its state

struct entry {
  unsigned seq;
  char folder[96];  // disc-01-<name>, in the session's folder
  char kind[48];    // what ripper made of it
  char title[160];
  char profile[48];
  double size;
  char drive_state[16];  // ready, unsupported, installing, done
  char outcome[256];     // what ripper said about it
  char can[160];         // the formats it could make here
  char cannot[300];      // the first it couldn't, and why
  char findings[2048];   // the drive test's
  char signs[1024];      // the disc report's clues: "looks like: ...", the custom firmware reads
  int commands;
  int answered;
  char result[24];  // tested, stopped, report only, failed
  char said[64];    // what the person says it is
  char note[256];
};

static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static struct {
  int active;  // the session's thread runs
  char state[16];
  char drive[16];
  char drive_label[64];
  char drive_name[96];
  char source[16];
  char reads[128];
  char firmware[160];
  char prompt[400];
  char step[64];
  int done;
  int total;
  int testing;  // the disc being tested (from 1); -1 the drive alone; 0 nothing
  char dir[SVC_PATH];
  char stamp[32];    // in the names
  char started[32];  // as the log writes the time, to keep only this session's lines
  char file[160];
  char error[256];
  int drive_tested;
  char drive_findings[2048];
  int drive_commands;
  int drive_answered;
  struct entry discs[MAX_DISCS];
  int count;
  volatile int cancel;  // stops the test running now
  volatile int finish;
  volatile int discard;
} g;

/* Helpers */

static void diag_dir(char *out, size_t n) {
  svc_path_join(out, n, plat_data_dir(), "diag");
}

static void set_state(const char *state, const char *prompt) {
  pthread_mutex_lock(&g_lock);
  svc_str_copy(g.state, sizeof(g.state), state);
  if (prompt) {
    svc_str_copy(g.prompt, sizeof(g.prompt), prompt);
  }
  pthread_mutex_unlock(&g_lock);
}

static void progress(void *ud, const char *step, int done, int total) {
  (void)ud;
  pthread_mutex_lock(&g_lock);
  svc_str_copy(g.step, sizeof(g.step), step);
  g.done = done;
  g.total = total;
  pthread_mutex_unlock(&g_lock);
}

static void copy_tree(const char *from, const char *to) {
  DIR *d = opendir(from);
  struct dirent *e;

  if (!d) {
    return;
  }
  rip_mkdir_p(to);
  while ((e = readdir(d)) != NULL) {
    char a[SVC_PATH + 256];
    char b[SVC_PATH + 256];
    struct stat st;
    if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) {
      continue;
    }
    snprintf(a, sizeof(a), "%s/%s", from, e->d_name);
    snprintf(b, sizeof(b), "%s/%s", to, e->d_name);
    if (stat(a, &st) != 0) {
      continue;
    }
    if (S_ISDIR(st.st_mode)) {
      copy_tree(a, b);
    } else {
      svc_copy_file(a, b, NULL, NULL);
    }
  }
  closedir(d);
}

// The disc report the watcher wrote for this disc, into the disc's folder.
static int take_report(const char *id, unsigned seq, const char *disc_dir) {
  char from[SVC_PATH];
  char to[SVC_PATH + 16];

  if (drives_report_dir(id, seq, from, sizeof(from)) != RIP_OK) {
    return 0;
  }
  snprintf(to, sizeof(to), "%s/report", disc_dir);
  copy_tree(from, to);
  return 1;
}

// The lines of a disc report that say what the disc looks like and what
// custom firmware got out of it: the evidence that a drive can read a kind.
static void report_signs(const char *disc_dir, char *out, size_t n) {
  static const char *const MARKS[] = {"looks like: ",
                                      "XGD",
                                      "Xbox security sector",
                                      "C0 raw read",
                                      "  DVD scrambling",
                                      "  Nintendo scrambling",
                                      "  Blu-ray scrambling",
                                      "GD-ROM: ",
                                      NULL};
  char path[SVC_PATH + 160];
  char line[512];
  FILE *f;
  size_t at = 0;
  int i;

  out[0] = '\0';
  snprintf(path, sizeof(path), "%s/report/report.txt", disc_dir);
  f = fopen(path, "r");
  if (!f) {
    return;
  }
  while (fgets(line, sizeof(line), f) && at + 1 < n) {
    for (i = 0; MARKS[i]; i++) {
      if (!strncmp(line, MARKS[i], strlen(MARKS[i]))) {
        const char *l = line;
        while (*l == ' ') {
          l++;
        }
        snprintf(out + at, n - at, "%s", l);
        at = strlen(out);
        if (at && out[at - 1] != '\n' && at + 1 < n) {
          out[at++] = '\n';
          out[at] = '\0';
        }
        break;
      }
    }
  }
  fclose(f);
}

// The line of the findings that starts with prefix, without it.
static void finding_line(const char *findings, const char *prefix, char *out, size_t n) {
  const char *at = findings;
  size_t pl = strlen(prefix);

  out[0] = '\0';
  while (at && *at) {
    const char *end = strchr(at, '\n');
    size_t len = end ? (size_t)(end - at) : strlen(at);
    if (len >= pl && !strncmp(at, prefix, pl)) {
      len -= pl;
      if (len >= n) {
        len = n - 1;
      }
      memcpy(out, at + pl, len);
      out[len] = '\0';
      return;
    }
    at = end ? end + 1 : NULL;
  }
}

// One finding a line, as a JSON array.
static cJSON *lines_json(const char *text) {
  cJSON *arr = cJSON_CreateArray();
  const char *at = text;

  while (at && *at) {
    const char *end = strchr(at, '\n');
    size_t len = end ? (size_t)(end - at) : strlen(at);
    if (len) {
      char *line = malloc(len + 1);
      if (line) {
        memcpy(line, at, len);
        line[len] = '\0';
        cJSON_AddItemToArray(arr, cJSON_CreateString(line));
        free(line);
      }
    }
    at = end ? end + 1 : NULL;
  }
  return arr;
}

/* The log, as far as it's this session's, without addresses */

// "smb://user@nas/share" -> "smb://...", 192.168.1.20 -> x.x.x.x.
static void scrub(const char *in, char *out, size_t n) {
  size_t k = 0;

  while (*in && k + 1 < n) {
    // A scheme at the start of a word: "smb://".
    if (isalpha((unsigned char)*in) && (k == 0 || !isalnum((unsigned char)out[k - 1]))) {
      const char *p = in;
      while (isalpha((unsigned char)*p) && p - in < 8) {
        p++;
      }
      if (!strncmp(p, "://", 3)) {
        size_t sl = (size_t)(p - in) + 3;
        if (k + sl + 4 >= n) {
          break;
        }
        memcpy(out + k, in, sl);
        k += sl;
        memcpy(out + k, "...", 3);
        k += 3;
        in = p + 3;
        while (*in && *in != ' ' && *in != ',' && *in != ')' && *in != '\'' && *in != '"') {
          in++;
        }
        continue;
      }
    }
    if (isdigit((unsigned char)*in) && (k == 0 || !isalnum((unsigned char)out[k - 1]))) {
      // Four groups of digits with dots between: an IPv4 address.
      const char *p = in;
      int groups = 0;
      while (groups < 4) {
        const char *q = p;
        while (isdigit((unsigned char)*q) && q - p < 3) {
          q++;
        }
        if (q == p) {
          break;
        }
        groups++;
        p = q;
        if (groups < 4) {
          if (*p != '.') {
            break;
          }
          p++;
        }
      }
      if (groups == 4 && !isalnum((unsigned char)*p)) {
        if (k + 8 >= n) {
          break;
        }
        memcpy(out + k, "x.x.x.x", 7);
        k += 7;
        in = p;
        continue;
      }
      while (isalnum((unsigned char)*in) && k + 1 < n) {  // the rest of this number or word
        out[k++] = *in++;
      }
      continue;
    }
    out[k++] = *in++;
  }
  out[k] = '\0';
}

static void write_log(const char *dir, const char *started) {
  char *log = svc_log_recent();
  char path[SVC_PATH + 32];
  FILE *f;
  const char *at = log;
  size_t sl = strlen(started);

  snprintf(path, sizeof(path), "%s/ripper-log.txt", dir);
  f = fopen(path, "w");
  if (!f) {
    free(log);
    return;
  }
  fprintf(f, "ripper's log from %s on, addresses taken out\n\n", started);
  while (at && *at) {
    const char *end = strchr(at, '\n');
    size_t len = end ? (size_t)(end - at) : strlen(at);
    char line[300];
    char clean[400];
    if (len >= sizeof(line)) {
      len = sizeof(line) - 1;
    }
    memcpy(line, at, len);
    line[len] = '\0';
    // Lines start with "YYYY-MM-DD HH:MM:SS", so they sort as text.
    if (len >= sl && strncmp(line, started, sl) >= 0) {
      scrub(line, clean, sizeof(clean));
      fprintf(f, "%s\n", clean);
    }
    at = end ? end + 1 : NULL;
  }
  fclose(f);
  free(log);
}

// "SONY     PS-SYSTEM   502R 1305" -> "SONY PS-SYSTEM 502R 1305": INQUIRY pads its fields with spaces.
static void set_drive_name(const char *name) {
  svc_squeeze(name, g.drive_name, sizeof(g.drive_name));
}

/* Testing */

static int claim(const char *id, struct drive_claim *c, const char *what) {
  char err[256];

  if (drives_claim(id, 0, 0, c, err, sizeof(err)) != RIP_OK) {
    RIP_WARN("drive test: can't take %s: %s", id, err);
    return 0;
  }
  drives_busy_message(id, what);
  return 1;
}

// The drive with no disc in it: what it says about itself.
static void test_drive_alone(const char *id) {
  struct drive_claim c;
  struct diag_summary sum;
  char dir[SVC_PATH + 16];
  int rc;

  memset(&c, 0, sizeof(c));
  pthread_mutex_lock(&g_lock);
  g.testing = -1;
  snprintf(dir, sizeof(dir), "%s/drive", g.dir);
  pthread_mutex_unlock(&g_lock);
  set_state("testing", "Looking at the drive itself. This takes a few seconds.");
  if (!claim(id, &c, "Drive test: the drive itself")) {
    return;
  }
  pthread_mutex_lock(c.io);
  rc = diag_run(c.t, dir, progress, NULL, &g.cancel, &sum);
  pthread_mutex_unlock(c.io);
  drives_release(id, 0, "");
  drives_claim_free(&c);
  pthread_mutex_lock(&g_lock);
  g.testing = 0;
  if (rc == RIP_OK || rc == RIP_ECANCEL) {
    g.drive_tested = 1;
    svc_str_copy(g.drive_findings, sizeof(g.drive_findings), sum.findings);
    g.drive_commands = sum.commands;
    g.drive_answered = sum.answered;
    finding_line(sum.findings, "firmware: ", g.firmware, sizeof(g.firmware));
    if (sum.drive[0]) {
      set_drive_name(sum.drive);
    }
  }
  pthread_mutex_unlock(&g_lock);
}

// What the watcher made of the disc, from /api/drives' view of the drive.
static void describe(struct entry *e, const cJSON *drive) {
  const cJSON *disc = cJSON_GetObjectItemCaseSensitive(drive, "disc");
  const cJSON *offer = cJSON_GetObjectItemCaseSensitive(drive, "offer");
  const cJSON *fmt;
  const char *message = jstr(drive, "message", "");
  const char *title;

  svc_str_copy(e->drive_state, sizeof(e->drive_state), jstr(drive, "state", ""));
  if (disc) {
    svc_str_copy(e->kind, sizeof(e->kind), jstr(disc, "kind", ""));
    svc_str_copy(e->profile, sizeof(e->profile), jstr(disc, "profile", ""));
    title = jstr(disc, "display", "");
    if (!title[0]) {
      title = jstr(disc, "title", "");
    }
    if (!title[0]) {
      title = jstr(disc, "title_id", "");
    }
    if (!title[0]) {
      title = jstr(disc, "volume_id", "");
    }
    svc_str_copy(e->title, sizeof(e->title), title);
    e->size = cJSON_GetNumberValue(cJSON_GetObjectItemCaseSensitive(disc, "size"));
  }
  if (offer && jstr(offer, "system_name", "")[0]) {
    svc_str_copy(e->kind, sizeof(e->kind), jstr(offer, "system_name", e->kind));
  }
  svc_str_copy(e->outcome, sizeof(e->outcome),
               message[0]                         ? message
               : !strcmp(e->drive_state, "ready") ? "ready to rip"
                                                  : e->drive_state);
  cJSON_ArrayForEach(fmt, cJSON_GetObjectItemCaseSensitive(offer, "formats")) {
    const char *label = jstr(fmt, "label", "");
    if (jbool(fmt, "available", 0)) {
      size_t at = strlen(e->can);
      snprintf(e->can + at, sizeof(e->can) - at, "%s%s", at ? ", " : "", label);
    } else if (!e->cannot[0]) {
      const char *reason = jstr(fmt, "reason", "");
      snprintf(e->cannot, sizeof(e->cannot), "%s: %s", label,
               reason[0] ? reason : jstr(fmt, "availability", "not here"));
    }
  }
}

static const char *entry_name(const struct entry *e) {
  if (e->title[0]) {
    return e->title;
  }
  return e->kind[0] ? e->kind : "the disc";
}

static void test_disc(const char *id, unsigned seq) {
  struct entry e;
  struct drive_claim c;
  struct diag_summary sum;
  cJSON *drive = drives_one_json(id);
  char disc_dir[SVC_PATH + 128];
  char test_dir[SVC_PATH + 160];
  char slug[48];
  char prompt[400];
  int n;
  int have_report = 0;
  int rc = RIP_ERR;

  memset(&e, 0, sizeof(e));
  memset(&sum, 0, sizeof(sum));
  e.seq = seq;
  if (drive) {
    describe(&e, drive);
  }
  cJSON_Delete(drive);

  pthread_mutex_lock(&g_lock);
  if (g.count >= MAX_DISCS) {
    pthread_mutex_unlock(&g_lock);
    set_state("waiting", "That's as many discs as one test holds. Press Finish, then start another for more.");
    return;
  }
  n = g.count + 1;
  svc_slug(e.title[0] ? e.title : e.kind[0] ? e.kind : "unreadable", slug, sizeof(slug));
  snprintf(e.folder, sizeof(e.folder), "disc-%02d-%s", n, slug[0] ? slug : "disc");
  snprintf(disc_dir, sizeof(disc_dir), "%s/%s", g.dir, e.folder);
  svc_str_copy(e.result, sizeof(e.result), "testing");
  g.discs[g.count++] = e;
  g.testing = n;
  g.done = 0;
  g.total = 0;
  g.step[0] = '\0';
  pthread_mutex_unlock(&g_lock);
  rip_mkdir_p(disc_dir);

  // A game the console is installing from this disc: reading it now could
  // upset the install, so only the report the watcher wrote goes in.
  if (!strcmp(e.drive_state, "installing")) {
    take_report(id, seq, disc_dir);
    report_signs(disc_dir, e.signs, sizeof(e.signs));
    svc_str_copy(e.result, sizeof(e.result), "report only");
  } else {
    snprintf(prompt, sizeof(prompt), "Testing disc %d, %.200s. Leave it in: this takes a minute or two.", n,
             entry_name(&e));
    set_state("testing", prompt);
    memset(&c, 0, sizeof(c));
    if (claim(id, &c, "Drive test: reading the disc")) {
      pthread_mutex_lock(c.io);  // the watcher's report is written by now, unless the disc never got ready
      have_report = take_report(id, seq, disc_dir);
      snprintf(test_dir, sizeof(test_dir), "%s/test", disc_dir);
      rc = diag_run(c.t, test_dir, progress, NULL, &g.cancel, &sum);
      pthread_mutex_unlock(c.io);
      drives_release(id, 0, "");
      drives_claim_free(&c);
    }
    if (!have_report) {
      uint64_t until = svc_mono_ms() + REPORT_WAIT_MS;
      while (!take_report(id, seq, disc_dir) && svc_mono_ms() < until) {
        usleep(250 * 1000);
      }
    }
    svc_str_copy(e.findings, sizeof(e.findings), sum.findings);
    e.commands = sum.commands;
    e.answered = sum.answered;
    report_signs(disc_dir, e.signs, sizeof(e.signs));
    svc_str_copy(e.result, sizeof(e.result), rc == RIP_OK ? "tested" : rc == RIP_ECANCEL ? "stopped" : "failed");
  }

  pthread_mutex_lock(&g_lock);
  // What the person typed in meanwhile stays.
  svc_str_copy(e.said, sizeof(e.said), g.discs[n - 1].said);
  svc_str_copy(e.note, sizeof(e.note), g.discs[n - 1].note);
  g.discs[n - 1] = e;
  g.testing = 0;
  if (!g.drive_tested && sum.drive[0]) {
    // The drive's own findings came with the disc's.
    g.drive_tested = 1;
    set_drive_name(sum.drive);
    finding_line(sum.findings, "firmware: ", g.firmware, sizeof(g.firmware));
  }
  pthread_mutex_unlock(&g_lock);
  RIP_INFO("drive test: disc %d (%s, %s): %s", n, e.kind, e.outcome, e.result);

  if (g.finish || g.discard) {
    return;
  }
  if (settings_notify()) {
    char head[200];
    snprintf(head, sizeof(head), "Disc %d tested: put in the next one", n);
    plat_notify(head, "Or press Finish in the drive test.");
  }
  if (settings_auto_eject() && strcmp(e.drive_state, "installing") != 0) {
    char err[64];
    drives_eject(id, err, sizeof(err));
  }
}

/* Packing up */

static void write_summary(const char *dir) {
  cJSON *j = cJSON_CreateObject();
  cJSON *d;
  cJSON *discs;
  char path[SVC_PATH + 32];
  FILE *f;
  int i;

  // Called with g_lock held.
  cJSON_AddStringToObject(j, "ripper", RIPPER_VERSION);
  cJSON_AddStringToObject(j, "build", RIPPER_BUILD);
  cJSON_AddStringToObject(j, "platform", plat_name());
  cJSON_AddStringToObject(j, "started", g.started);
  d = cJSON_AddObjectToObject(j, "drive");
  cJSON_AddStringToObject(d, "slot", g.drive);
  cJSON_AddStringToObject(d, "label", g.drive_label);
  cJSON_AddStringToObject(d, "source", g.source);
  cJSON_AddStringToObject(d, "name", g.drive_name);
  cJSON_AddStringToObject(d, "reads", g.reads);
  cJSON_AddStringToObject(d, "firmware", g.firmware);
  cJSON_AddItemToObject(d, "findings", lines_json(g.drive_findings));
  discs = cJSON_AddArrayToObject(j, "discs");
  for (i = 0; i < g.count; i++) {
    const struct entry *e = &g.discs[i];
    cJSON *o = cJSON_CreateObject();
    cJSON_AddNumberToObject(o, "n", i + 1);
    cJSON_AddStringToObject(o, "said", e->said);
    cJSON_AddStringToObject(o, "note", e->note);
    cJSON_AddStringToObject(o, "detected", e->kind);
    cJSON_AddStringToObject(o, "title", e->title);
    cJSON_AddStringToObject(o, "profile", e->profile);
    cJSON_AddNumberToObject(o, "size", e->size);
    cJSON_AddStringToObject(o, "drive_state", e->drive_state);
    cJSON_AddStringToObject(o, "outcome", e->outcome);
    cJSON_AddStringToObject(o, "can_make", e->can);
    cJSON_AddStringToObject(o, "cannot_make", e->cannot);
    cJSON_AddStringToObject(o, "result", e->result);
    cJSON_AddNumberToObject(o, "commands", e->commands);
    cJSON_AddNumberToObject(o, "answered", e->answered);
    cJSON_AddStringToObject(o, "folder", e->folder);
    cJSON_AddItemToObject(o, "signs", lines_json(e->signs));
    cJSON_AddItemToObject(o, "findings", lines_json(e->findings));
    cJSON_AddItemToArray(discs, o);
  }
  snprintf(path, sizeof(path), "%s/session.json", dir);
  jsave(path, j);
  cJSON_Delete(j);

  snprintf(path, sizeof(path), "%s/summary.txt", dir);
  f = fopen(path, "w");
  if (!f) {
    return;
  }
  fprintf(f, "ripper %s (build %s) drive test, %s\n\n", RIPPER_VERSION, RIPPER_BUILD, g.started);
  fprintf(f, "Drive: %s (%s, %s)\n", g.drive_name[0] ? g.drive_name : "?", g.drive_label, g.source);
  fprintf(f, "Reads: %s\nFirmware: %s\n", g.reads, g.firmware[0] ? g.firmware : "?");
  if (g.drive_findings[0]) {
    fprintf(f, "\nThe drive alone:\n%s", g.drive_findings);
  }
  for (i = 0; i < g.count; i++) {
    const struct entry *e = &g.discs[i];
    fprintf(f, "\n== Disc %d: %s (%s)\n", i + 1, e->said[0] ? e->said : "(not said)", e->result);
    if (e->note[0]) {
      fprintf(f, "note: %s\n", e->note);
    }
    fprintf(f, "ripper saw: %s '%s', %s, %.0f bytes\n", e->kind[0] ? e->kind : "nothing", e->title, e->profile,
            e->size);
    fprintf(f, "ripper said: %s\n", e->outcome);
    fprintf(f, "can make here: %s\n", e->can[0] ? e->can : "nothing");
    if (e->cannot[0]) {
      fprintf(f, "not here: %s\n", e->cannot);
    }
    if (e->signs[0]) {
      fprintf(f, "the disc report:\n%s", e->signs);
    }
    if (e->findings[0]) {
      fprintf(f, "the drive test:\n%s", e->findings);
    }
    fprintf(f, "files: %s/\n", e->folder);
  }
  fclose(f);
}

// The zip from the session's folder; called again when a label changes after.
static int pack(void) {
  char dir[SVC_PATH];
  char zip[SVC_PATH + 200];
  char base[SVC_PATH];
  char name[160];
  char model[64];
  char started[32];
  int rc;

  pthread_mutex_lock(&g_lock);
  svc_str_copy(dir, sizeof(dir), g.dir);
  svc_str_copy(started, sizeof(started), g.started);
  svc_slug(g.drive_name[0] ? g.drive_name : g.drive, model, sizeof(model));
  snprintf(name, sizeof(name), "ripper-drivetest-%.60s-%s.zip", model, g.stamp);
  write_summary(dir);
  pthread_mutex_unlock(&g_lock);
  write_log(dir, started);
  diag_dir(base, sizeof(base));
  snprintf(zip, sizeof(zip), "%s/%s", base, name);
  rc = zip_build_dir(dir, zip, NULL, NULL);
  pthread_mutex_lock(&g_lock);
  if (rc == RIP_OK) {
    svc_str_copy(g.file, sizeof(g.file), name);
  }
  pthread_mutex_unlock(&g_lock);
  return rc;
}

/* The session */

static void *session_thread(void *arg) {
  char id[16];
  unsigned last = 0;
  int first = 1;

  (void)arg;
  pthread_mutex_lock(&g_lock);
  svc_str_copy(id, sizeof(id), g.drive);
  pthread_mutex_unlock(&g_lock);

  while (!g.finish && !g.discard) {
    char st[16];
    unsigned seq = 0;
    int busy = 0;
    char prompt[400];
    if (drives_peek(id, st, sizeof(st), &seq, &busy) != RIP_OK) {
      set_state("waiting", "The drive isn't there any more. Plug it back in, or press Finish.");
    } else if (busy) {
      set_state("waiting", "Something else is using the drive. Waiting for it to finish.");
    } else if (!strcmp(st, "empty")) {
      if (first) {
        first = 0;
        test_drive_alone(id);
        continue;
      }
      pthread_mutex_lock(&g_lock);
      svc_str_copy(prompt, sizeof(prompt),
                   g.count ? "Put in the next disc, or press Finish if that's all."
                           : "Put in a disc. Any kind: games, music, movies, even ones you think the drive can't "
                             "read. How a drive refuses a disc tells us something too.");
      pthread_mutex_unlock(&g_lock);
      set_state("waiting", prompt);
    } else if (!strcmp(st, "loading") || !strcmp(st, "identifying")) {
      set_state("waiting", "Reading the disc. One the drive can't read takes up to half a minute to give up on.");
    } else if (seq != last) {
      first = 0;
      last = seq;
      test_disc(id, seq);
      continue;
    } else {
      pthread_mutex_lock(&g_lock);
      snprintf(prompt, sizeof(prompt), "Disc %d is done. Take it out and put in the next one, or press Finish.",
               g.count);
      pthread_mutex_unlock(&g_lock);
      set_state("waiting", prompt);
    }
    usleep(TICK_MS * 1000);
  }

  if (g.discard) {
    char dir[SVC_PATH];
    pthread_mutex_lock(&g_lock);
    svc_str_copy(dir, sizeof(dir), g.dir);
    pthread_mutex_unlock(&g_lock);
    svc_remove_tree(dir);
    pthread_mutex_lock(&g_lock);
    memset(&g, 0, sizeof(g));
    pthread_mutex_unlock(&g_lock);
    RIP_INFO("drive test thrown away");
    return NULL;
  }
  set_state("finishing", "Packing the results.");
  if (pack() == RIP_OK) {
    set_state("done",
              "Thank you! Download the zip and attach it to the drive report form. Say what you can about "
              "the drive there: its model, its firmware, and the enclosure or adapter it's in.");
    RIP_INFO("drive test done: %s", g.file);
  } else {
    pthread_mutex_lock(&g_lock);
    svc_str_copy(g.error, sizeof(g.error), "couldn't zip the results");
    pthread_mutex_unlock(&g_lock);
    set_state("failed", "The results couldn't be packed. They're still on the console in /data/ripper/diag.");
  }
  pthread_mutex_lock(&g_lock);
  g.active = 0;
  pthread_mutex_unlock(&g_lock);
  return NULL;
}

int drivetest_start(const char *drive_id, char *err, size_t en) {
  cJSON *drive;
  char base[SVC_PATH - 64];  // room for the session's folder in g.dir
  char old[SVC_PATH];
  char st[16];
  unsigned seq;
  int busy;
  time_t now = time(NULL);
  struct tm tm;

  pthread_mutex_lock(&g_lock);
  if (g.active) {
    pthread_mutex_unlock(&g_lock);
    snprintf(err, en, "a drive test is running already: finish it or throw it away first");
    return RIP_ERR;
  }
  svc_str_copy(old, sizeof(old), g.dir);
  pthread_mutex_unlock(&g_lock);
  if (drives_peek(drive_id, st, sizeof(st), &seq, &busy) != RIP_OK) {
    snprintf(err, en, "no such drive");
    return RIP_ERR;
  }
  if (busy) {
    snprintf(err, en, "the drive is busy: let the rip finish first");
    return RIP_ERR;
  }
  drive = drives_one_json(drive_id);
  // The last session's folder (its zip stays).
  if (old[0]) {
    svc_remove_tree(old);
  }

  pthread_mutex_lock(&g_lock);
  memset(&g, 0, sizeof(g));
  g.active = 1;
  svc_str_copy(g.state, sizeof(g.state), "waiting");
  svc_str_copy(g.prompt, sizeof(g.prompt), "Starting.");
  svc_str_copy(g.drive, sizeof(g.drive), drive_id);
  svc_str_copy(g.drive_label, sizeof(g.drive_label), jstr(drive, "label", drive_id));
  svc_str_copy(g.source, sizeof(g.source), jstr(drive, "source", ""));
  svc_str_copy(g.reads, sizeof(g.reads), jstr(drive, "reads", ""));
  {
    char name[128];
    snprintf(name, sizeof(name), "%s %s %s", jstr(drive, "vendor", ""), jstr(drive, "product", ""),
             jstr(drive, "revision", ""));
    set_drive_name(name);
  }
  localtime_r(&now, &tm);
  strftime(g.stamp, sizeof(g.stamp), "%Y%m%d-%H%M%S", &tm);
  strftime(g.started, sizeof(g.started), "%Y-%m-%d %H:%M:%S", &tm);
  diag_dir(base, sizeof(base));
  snprintf(g.dir, sizeof(g.dir), "%s/drivetest-%s", base, g.stamp);
  pthread_mutex_unlock(&g_lock);
  cJSON_Delete(drive);

  if (rip_mkdir_p(g.dir) != RIP_OK) {
    pthread_mutex_lock(&g_lock);
    g.active = 0;
    pthread_mutex_unlock(&g_lock);
    snprintf(err, en, "can't write to the diag folder");
    return RIP_ERR;
  }
  RIP_INFO("drive test of %s (%s) started", g.drive_label, g.drive_name);
  if (svc_thread(session_thread, NULL) != 0) {
    pthread_mutex_lock(&g_lock);
    g.active = 0;
    pthread_mutex_unlock(&g_lock);
    snprintf(err, en, "couldn't start the drive test");
    return RIP_ERR;
  }
  return RIP_OK;
}

int drivetest_label(int n, const char *said, const char *note, char *err, size_t en) {
  int repack;

  pthread_mutex_lock(&g_lock);
  if (n < 1 || n > g.count) {
    pthread_mutex_unlock(&g_lock);
    snprintf(err, en, "no such disc in this test");
    return RIP_ERR;
  }
  if (said) {
    svc_str_copy(g.discs[n - 1].said, sizeof(g.discs[n - 1].said), said);
  }
  if (note) {
    svc_str_copy(g.discs[n - 1].note, sizeof(g.discs[n - 1].note), note);
  }
  repack = !g.active && !strcmp(g.state, "done");
  pthread_mutex_unlock(&g_lock);
  if (repack && pack() != RIP_OK) {
    snprintf(err, en, "couldn't pack the results again");
    return RIP_ERR;
  }
  return RIP_OK;
}

int drivetest_finish(char *err, size_t en) {
  pthread_mutex_lock(&g_lock);
  if (!g.active) {
    pthread_mutex_unlock(&g_lock);
    snprintf(err, en, "no drive test is running");
    return RIP_ERR;
  }
  g.finish = 1;
  g.cancel = 1;
  pthread_mutex_unlock(&g_lock);
  return RIP_OK;
}

void drivetest_discard(void) {
  pthread_mutex_lock(&g_lock);
  if (g.active) {
    g.discard = 1;
    g.cancel = 1;
    pthread_mutex_unlock(&g_lock);
    return;
  }
  // A finished one: its folder goes, its zip stays with the other reports.
  if (g.dir[0]) {
    char dir[SVC_PATH];
    svc_str_copy(dir, sizeof(dir), g.dir);
    memset(&g, 0, sizeof(g));
    pthread_mutex_unlock(&g_lock);
    svc_remove_tree(dir);
    return;
  }
  memset(&g, 0, sizeof(g));
  pthread_mutex_unlock(&g_lock);
}

int drivetest_holds(const char *drive_id) {
  int holds;

  pthread_mutex_lock(&g_lock);
  holds = g.active && !strcmp(g.drive, drive_id);
  pthread_mutex_unlock(&g_lock);
  return holds;
}

cJSON *drivetest_status(void) {
  cJSON *o = cJSON_CreateObject();
  cJSON *discs;
  int i;

  pthread_mutex_lock(&g_lock);
  cJSON_AddStringToObject(o, "state", g.state[0] ? g.state : "idle");
  cJSON_AddStringToObject(o, "drive", g.drive);
  cJSON_AddStringToObject(o, "drive_label", g.drive_label);
  cJSON_AddStringToObject(o, "drive_name", g.drive_name);
  cJSON_AddStringToObject(o, "reads", g.reads);
  cJSON_AddStringToObject(o, "firmware", g.firmware);
  cJSON_AddStringToObject(o, "prompt", g.prompt);
  cJSON_AddStringToObject(o, "step", g.step);
  cJSON_AddNumberToObject(o, "done", g.done);
  cJSON_AddNumberToObject(o, "total", g.total);
  cJSON_AddNumberToObject(o, "testing", g.testing);
  cJSON_AddStringToObject(o, "file", g.file);
  cJSON_AddStringToObject(o, "error", g.error);
  cJSON_AddStringToObject(o, "form", RIPPER_REPORT_FORM);
  cJSON_AddStringToObject(o, "version", RIPPER_VERSION);
  cJSON_AddStringToObject(o, "drive_findings", g.drive_findings);
  discs = cJSON_AddArrayToObject(o, "discs");
  for (i = 0; i < g.count; i++) {
    const struct entry *e = &g.discs[i];
    cJSON *d = cJSON_CreateObject();
    cJSON_AddNumberToObject(d, "n", i + 1);
    cJSON_AddStringToObject(d, "kind", e->kind);
    cJSON_AddStringToObject(d, "title", e->title);
    cJSON_AddStringToObject(d, "profile", e->profile);
    cJSON_AddStringToObject(d, "outcome", e->outcome);
    cJSON_AddStringToObject(d, "can", e->can);
    cJSON_AddStringToObject(d, "cannot", e->cannot);
    cJSON_AddStringToObject(d, "result", e->result);
    cJSON_AddStringToObject(d, "said", e->said);
    cJSON_AddStringToObject(d, "note", e->note);
    cJSON_AddStringToObject(d, "signs", e->signs);
    cJSON_AddStringToObject(d, "findings", e->findings);
    cJSON_AddItemToArray(discs, d);
  }
  pthread_mutex_unlock(&g_lock);
  return o;
}

char *drivetest_file(const char *name, size_t *len) {
  char dir[SVC_PATH];
  char path[SVC_PATH + 200];
  size_t i;

  // A file of ours, nothing else: no paths.
  if (!name || strncmp(name, "ripper-drivetest-", 17) != 0 || strlen(name) > 160) {
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
