#include "drives.h"

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "catalog.h"
#include "details.h"
#include "disc_report.h"
#include "jobs.h"
#include "platform.h"
#include "ripper/common.h"
#include "ripper/iso9660.h"
#include "ripper/log.h"
#include "ripper/mmc.h"
#include "ripper/rip.h"
#include "ripper/udf.h"
#include "ripper/util.h"
#include "drivetest.h"
#include "settings.h"
#include "svc.h"

#define MAX_SLOTS 8
#define POLL_MS 1500
#define RESCAN_POLLS 10           // look for drives coming and going every 15 s
#define INSTALL_TELL_EVERY_S 300  // how far an install is, while RipperPS isn't in front

enum state { ST_EMPTY, ST_LOADING, ST_IDENTIFYING, ST_READY, ST_UNSUPPORTED, ST_RIPPING, ST_DONE, ST_INSTALLING };

static const char *STATE_NAMES[] = {"empty",       "loading", "identifying", "ready",
                                    "unsupported", "ripping", "done",        "installing"};

struct slot {
  int used;
  struct plat_drive info;
  int open;
  struct rip_transport t;
  struct rip_drive drive;
  enum state state;
  char message[256];
  struct rip_disc disc;
  int identified;
  cJSON *offer;
  unsigned disc_seq;      // goes up with every disc, so a UI can tell a new one arrived
  int busy;               // a job has the drive
  enum state prev_state;  // what a benchmark found, to put back
  char prev_message[256];
  pthread_mutex_t io;  // one command at a time: the transports reuse one request block
  int job_id;
  int wanted_identify;
  uint64_t loading_since;  // when the drive last went from empty to not ready
  int loading_logged;
  // Names and a cover, looked up after identification (details.h).
  struct disc_details details;
  int details_state;             // 0 none, 1 looking, 2 done
  int polled;                    // asked at least once: a disc found at start isn't one that just went in
  int game_state;                // PS4/PS5: the installed game as offered (game_state())
  struct plat_dump_status game;  // ... as last asked, with how far its install is
  double install_rate;           // bytes a second the install copies, smoothed
  uint64_t install_moved_at;     // when its count last moved (svc_mono_ms)
  int64_t install_told;          // the last notification of how far it is (svc_now)
  char report_dir[SVC_PATH];     // the last disc report's folder ...
  unsigned report_seq;           // ... and the disc it's of
};

static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static struct slot g_slots[MAX_SLOTS];
static unsigned g_seq;

static struct slot *find(const char *id) {
  int i;

  for (i = 0; i < MAX_SLOTS; i++) {
    if (g_slots[i].used && !strcmp(g_slots[i].info.id, id)) {
      return &g_slots[i];
    }
  }
  return NULL;
}

static void set_state(struct slot *s, enum state st, const char *message) {
  s->state = st;
  svc_str_copy(s->message, sizeof(s->message), message ? message : "");
}

/* Identification */

// A name for the files: the title the disc gives, its product code, or its label.
static void suggest_name(const struct rip_disc *d, char *out, size_t n) {
  if (d->title[0] && d->title_id[0]) {
    snprintf(out, n, "%s (%s)", d->title, d->title_id);
  } else if (d->title[0]) {
    svc_str_copy(out, n, d->title);
  } else if (d->title_id[0]) {
    svc_str_copy(out, n, d->title_id);
  } else if (d->volume_id[0]) {
    svc_pretty_label(d->volume_id, out, n);
  } else {
    svc_str_copy(out, n, "Disc");
  }
}

// What the UI offers for this disc on this drive.
static cJSON *make_offer(const struct slot *s, const char **why_not) {
  const struct cat_system *sys = catalog_system_for(s->disc.kind);
  struct rip_choice choices[8];
  cJSON *offer;
  cJSON *formats;
  cJSON *profile;
  cJSON *suggest;
  const char *first_ok = NULL;
  const char *profile_format;
  int profile_ok = 0;
  char name[256];
  size_t n;
  size_t i;

  *why_not = NULL;
  if (!sys) {
    *why_not = "RipperPS doesn't know this kind of disc";
    return NULL;
  }
  offer = cJSON_CreateObject();
  cJSON_AddStringToObject(offer, "system", sys->id);
  cJSON_AddStringToObject(offer, "system_name", sys->name);
  cJSON_AddStringToObject(offer, "group", sys->group);
  formats = cJSON_AddArrayToObject(offer, "formats");
  profile = settings_profile(sys->id);
  profile_format = jstr(profile, "format", "");
  n = rip_plan(&s->drive, &s->disc, choices, RIP_ARRAY_LEN(choices));
  for (i = 0; i < n; i++) {
    const char *fid = catalog_format_id(choices[i].recipe->id);
    cJSON *f = cJSON_CreateObject();
    cJSON_AddStringToObject(f, "id", fid);
    cJSON_AddStringToObject(f, "label", choices[i].recipe->output);
    cJSON_AddStringToObject(f, "availability", rip_availability_name(choices[i].avail));
    cJSON_AddBoolToObject(f, "available", choices[i].avail == RIP_AVAIL_OK);
    if (choices[i].recipe->id == RIP_RECIPE_SYSTEM_DUMP && choices[i].avail == RIP_AVAIL_OK) {
      // A game dump reads the installed game through the console, not the disc.
      struct plat_dump_status st;
      plat_game_status(s->disc.title_id, &st);
      if (!st.supported) {
        choices[i].avail = RIP_AVAIL_NEEDS_DRIVE;
        choices[i].reason = "not on this system";
      } else if (!st.installed && !st.installing) {
        choices[i].avail = RIP_AVAIL_BLOCKED;
        choices[i].reason = "install the game first";
      } else if (!st.complete) {
        choices[i].avail = RIP_AVAIL_BLOCKED;
        choices[i].reason = "still installing";
      } else if (st.partial) {
        choices[i].avail = RIP_AVAIL_BLOCKED;
        choices[i].reason =
            "parts of it were never installed here (other languages), and reading them would hang "
            "the console";
      } else if (!st.mounted) {
        cJSON_AddStringToObject(f, "note",
                                st.running ? "Closes the game that's running." : "Starts the game for a moment.");
      }
      cJSON_ReplaceItemInObjectCaseSensitive(f, "availability",
                                             cJSON_CreateString(rip_availability_name(choices[i].avail)));
      cJSON_ReplaceItemInObjectCaseSensitive(f, "available", cJSON_CreateBool(choices[i].avail == RIP_AVAIL_OK));
    }
    cJSON_AddStringToObject(f, "reason", choices[i].reason);
    cJSON_AddItemToArray(formats, f);
    if (choices[i].avail == RIP_AVAIL_OK) {
      if (!first_ok) {
        first_ok = fid;
      }
      if (!strcmp(fid, profile_format)) {
        profile_ok = 1;
      }
    }
  }
  if (!first_ok) {
    // Say why, from the first format's reason: another drive, not done yet, protection.
    *why_not = n ? choices[0].reason : "nothing can be made of this disc yet";
  }
  suggest_name(&s->disc, name, sizeof(name));
  suggest = cJSON_Duplicate(profile, 1);
  if (!profile_ok && first_ok) {
    // The profile's format doesn't work here: the first that does, with its own options.
    jset_str(suggest, "format", first_ok);
  }
  jset_str(suggest, "name", name);
  cJSON_AddItemToObject(offer, "suggestion", suggest);
  cJSON_Delete(profile);
  return offer;
}

// Caller holds g_lock. PS4/PS5: what the console says of the disc's installed
// game (into s->game), as one number. The dump's offer depends on it, and it
// changes with the disc in.
static int game_state(struct slot *s) {
  struct plat_dump_status st;
  uint64_t now = svc_mono_ms();

  if (s->disc.kind != RIP_DISC_PS4_BD && s->disc.kind != RIP_DISC_PS5_BD) {
    return 0;
  }
  plat_game_status(s->disc.title_id, &st);
  // The install's speed, between moves of its count: the system writes it every few seconds.
  if (!st.installing || !s->game.installing) {
    s->install_rate = 0;
    s->install_moved_at = now;
  } else if (st.copied > s->game.copied && now > s->install_moved_at) {
    double r = (double)(st.copied - s->game.copied) * 1000.0 / (double)(now - s->install_moved_at);
    s->install_rate = s->install_rate > 0 ? 0.7 * s->install_rate + 0.3 * r : r;
    s->install_moved_at = now;
  }
  s->game = st;
  return st.installed | st.complete << 1 | st.mounted << 2 | st.running << 3 | st.installing << 4;
}

// Caller holds g_lock. The state an offer leaves the drive in.
static void offer_state(struct slot *s, const cJSON *offer, const char *why_not) {
  if (offer && !why_not) {
    set_state(s, ST_READY, "");
  } else if (s->game.installing) {
    set_state(s, ST_INSTALLING,
              "The console is still installing this game. Leave the disc in: it can be ripped once that's done.");
  } else {
    set_state(s, ST_UNSUPPORTED, why_not);
  }
}

// Caller holds g_lock. The offer again for the same disc, keeping the name looked up for it.
static void redo_offer(struct slot *s, int game) {
  const char *why_not = NULL;
  cJSON *offer = make_offer(s, &why_not);
  const char *name = jstr(cJSON_GetObjectItemCaseSensitive(s->offer, "suggestion"), "name", "");

  if (offer && name[0]) {
    jset_str(cJSON_GetObjectItemCaseSensitive(offer, "suggestion"), "name", name);
  }
  cJSON_Delete(s->offer);
  s->offer = offer;
  s->game_state = game;
  offer_state(s, offer, why_not);
  RIP_INFO("%s: %s", s->info.label, why_not ? why_not : "ready to rip");
}

// What the game is called: as looked up, as the disc says, or its ID.
static const char *game_title(const struct slot *s) {
  return s->details.title[0] ? s->details.title : s->disc.title[0] ? s->disc.title : s->disc.title_id;
}

// "64% · 42.0 MB/s · 4 min left", as a rip's progress is told.
static void install_progress(const struct slot *s, char *out, size_t n) {
  const struct plat_dump_status *st = &s->game;
  char left[48];

  svc_time_left(s->install_rate > 1 ? (double)(st->to_copy - st->copied) / s->install_rate : 0, left, sizeof(left));
  if (s->install_rate > 1) {
    snprintf(out, n, "%d%% · %.1f MB/s%s", (int)(100.0 * (double)st->copied / (double)st->to_copy),
             s->install_rate / 1e6, left);
  } else {
    snprintf(out, n, "%d%%", (int)(100.0 * (double)st->copied / (double)st->to_copy));
  }
}

// Caller holds g_lock. What to tell of the disc's game since the last look
// (was): playable while the rest still copies, how far it is every few
// minutes while RipperPS isn't in front, done. 1 when there's something.
static int install_news(struct slot *s, const struct plat_dump_status *was, char *head, size_t hn, char *body,
                        size_t bn) {
  const struct plat_dump_status *st = &s->game;
  char how[96];
  int64_t now = svc_now();

  if (!settings_notify()) {
    return 0;
  }
  if (was->installing && !st->installing) {
    snprintf(head, hn, "%s is installed", game_title(s));
    if (s->state == ST_READY) {
      snprintf(body, bn, "Ready to rip.");
    } else {
      snprintf(body, bn, "It can't be ripped: %s.", s->message);
    }
    return 1;
  }
  if (!st->installing) {
    return 0;
  }
  install_progress(s, how, sizeof(how));
  // The title is what shows large: what to do goes there.
  if (!was->installing) {
    snprintf(head, hn, "Installing %s: leave the disc in", game_title(s));
    snprintf(body, bn, "%s. It can be ripped once it's installed.", how);
    s->install_told = now;
    return 1;
  }
  // The system says it's ready to play here, and stops showing the copy.
  if (st->installed && !was->installed) {
    snprintf(head, hn, "%s is still installing: leave the disc in", game_title(s));
    snprintf(body, bn, "It can be played already. %s.", how);
    s->install_told = now;
    return 1;
  }
  if (now - s->install_told >= INSTALL_TELL_EVERY_S && !plat_app_in_front()) {
    snprintf(head, hn, "%s is still installing: leave the disc in", game_title(s));
    snprintf(body, bn, "%s.", how);
    s->install_told = now;
    return 1;
  }
  return 0;
}

static cJSON *disc_json(const struct rip_disc *d) {
  cJSON *j = cJSON_CreateObject();

  cJSON_AddStringToObject(j, "kind", rip_disc_kind_name(d->kind));
  cJSON_AddStringToObject(j, "profile", mmc_profile_name(d->profile));
  cJSON_AddStringToObject(j, "title", d->title);
  cJSON_AddStringToObject(j, "title_id", d->title_id);
  cJSON_AddStringToObject(j, "volume_id", d->volume_id);
  cJSON_AddNumberToObject(j, "size", (double)(d->sector_count * d->sector_size));
  cJSON_AddBoolToObject(j, "css", d->css);
  if (d->has_toc) {
    unsigned i;
    unsigned audio = 0;
    for (i = 0; i < d->toc.count; i++) {
      audio += cd_track_is_audio(&d->toc.tracks[i]);
    }
    cJSON_AddNumberToObject(j, "tracks", d->toc.count);
    cJSON_AddNumberToObject(j, "audio_tracks", audio);
    cJSON_AddNumberToObject(j, "seconds", d->toc.leadout / 75);
    cJSON_AddStringToObject(j, "musicbrainz_id", d->musicbrainz_id);
  }
  return j;
}

/* Details: names and covers */

static void auto_start(struct slot *s, unsigned seq);

// A file off the disc for details.c, unless a rip has the drive.
static int read_disc_file(void *ud, const char *path, uint8_t **out, size_t *len) {
  struct slot *s = ud;
  struct rip_reader r;
  int rc = -1;

  pthread_mutex_lock(&s->io);
  if (!s->busy && plat_drive_reader(s->info.id, &s->t, &r) == RIP_OK) {
    struct iso_fs iso;
    struct iso_entry ie;
    struct udf_fs udf;
    struct udf_entry ue;
    if (udf_mount(&udf, &r) == RIP_OK && udf_lookup(&udf, path, &ue) == RIP_OK &&
        udf_read_file(&udf, &ue, 8u << 20, out, len) == RIP_OK) {
      rc = 0;
    } else if (iso_mount(&iso, &r) == RIP_OK && iso_lookup(&iso, path, &ie) == RIP_OK &&
               iso_read_file(&iso, &ie, 8u << 20, out, len) == RIP_OK) {
      rc = 0;
    }
    rip_reader_close(&r);
  }
  pthread_mutex_unlock(&s->io);
  return rc;
}

struct details_job {
  struct slot *s;
  unsigned seq;
  struct rip_disc disc;
  char system[16];
  char hint[256];  // looked up again by a name the user gave: not announced again
};

static void *details_thread(void *arg) {
  struct details_job *dj = arg;
  struct slot *s = dj->s;
  struct disc_details d;

  details_lookup(dj->system, &dj->disc, plat_drive_mount(s->info.id), read_disc_file, s, dj->hint, &d);
  pthread_mutex_lock(&g_lock);
  if (s->disc_seq == dj->seq) {
    details_free(&s->details);
    s->details = d;
    s->details_state = 2;
    // The name looked up is the one to rip under (Redump's, the album's).
    if (d.name[0] && s->offer) {
      cJSON *sug = cJSON_GetObjectItemCaseSensitive(s->offer, "suggestion");
      char name[256];
      svc_str_copy(name, sizeof(name), d.name);
      rip_sanitize_name(name, sizeof(name));
      if (sug) {
        jset_str(sug, "name", name);
      }
    }
  } else {
    details_free(&d);
  }
  {
    // "Disc ready: Gran Turismo 4", with what it is under it.
    int announce = s->disc_seq == dj->seq && s->state == ST_READY && settings_notify() && !dj->hint[0];
    char head[320];
    char body[128];
    const struct cat_system *sys = catalog_system_for(dj->disc.kind);
    snprintf(head, sizeof(head), "Disc ready: %s",
             s->details.title[0] ? s->details.title : rip_disc_kind_name(dj->disc.kind));
    snprintf(body, sizeof(body), "%s", sys ? sys->name : rip_disc_kind_name(dj->disc.kind));
    pthread_mutex_unlock(&g_lock);
    if (announce) {
      plat_notify(head, body);
    }
  }
  if (settings_auto_start() && !dj->hint[0]) {
    auto_start(s, dj->seq);
  }
  free(dj);
  return NULL;
}

// Caller holds g_lock.
static void start_details(struct slot *s, const char *hint) {
  const struct cat_system *sys = catalog_system_for(s->disc.kind);
  struct details_job *dj = calloc(1, sizeof(*dj));

  details_free(&s->details);
  s->details_state = 0;
  if (!dj) {
    return;
  }
  dj->s = s;
  dj->seq = s->disc_seq;
  dj->disc = s->disc;
  svc_str_copy(dj->system, sizeof(dj->system), sys ? sys->id : "");
  svc_str_copy(dj->hint, sizeof(dj->hint), hint ? hint : "");
  s->details_state = 1;
  if (svc_thread(details_thread, dj) != 0) {
    s->details_state = 0;
    free(dj);
  }
}

int drives_lookup(const char *id, const char *name, char *err, size_t en) {
  struct slot *s;
  int rc = RIP_ERR;

  pthread_mutex_lock(&g_lock);
  s = find(id);
  if (!s || !s->identified || s->state == ST_EMPTY) {
    snprintf(err, en, s ? "no disc in it" : "no such drive");
  } else {
    start_details(s, name);
    rc = RIP_OK;
  }
  pthread_mutex_unlock(&g_lock);
  return rc;
}

int drives_cover(const char *id, uint8_t **out, size_t *len, char *type, size_t tn) {
  struct slot *s;
  int rc = RIP_ENOENT;

  pthread_mutex_lock(&g_lock);
  s = find(id);
  if (s && s->details.cover && (*out = malloc(s->details.cover_len)) != NULL) {
    memcpy(*out, s->details.cover, s->details.cover_len);
    *len = s->details.cover_len;
    svc_str_copy(type, tn, s->details.cover_type);
    rc = RIP_OK;
  }
  pthread_mutex_unlock(&g_lock);
  return rc;
}

// Leaves a report of the disc for testers (disc_report.h). The caller holds s->io.
static void report_disc(struct slot *s, struct rip_reader *reader, const struct rip_disc *disc, uint16_t profile,
                        const char *outcome, const cJSON *offer) {
  struct disc_report r = {s->info.id, s->info.label, &s->drive, &s->t, reader,
                          disc,       profile,       outcome,   offer, plat_drive_mount(s->info.id)};
  char dir[SVC_PATH];
  unsigned seq;

  pthread_mutex_lock(&g_lock);
  seq = s->disc_seq;
  pthread_mutex_unlock(&g_lock);
  disc_report_write(&r, dir, sizeof(dir));
  pthread_mutex_lock(&g_lock);
  svc_str_copy(s->report_dir, sizeof(s->report_dir), dir);
  s->report_seq = seq;
  pthread_mutex_unlock(&g_lock);
}

// Reads what the disc is. Runs on the watcher's thread with the lock released.
static void identify(struct slot *s, uint16_t profile) {
  struct rip_reader reader;
  struct rip_disc disc;
  int have_reader;
  const char *why_not = NULL;
  cJSON *offer;
  cJSON *reported;
  char title[300];
  char how[96];
  int installing;

  // The drive stays ours until the report is written, so a rip can't start in between.
  pthread_mutex_lock(&s->io);
  have_reader = plat_drive_reader(s->info.id, &s->t, &reader) == RIP_OK;
  rip_disc_identify(&disc, profile, have_reader ? &reader : NULL, &s->t, plat_drive_mount(s->info.id));
  rip_disc_refine(&disc, &s->drive);

  pthread_mutex_lock(&g_lock);
  s->disc = disc;
  s->identified = 1;
  s->disc_seq = ++g_seq;
  start_details(s, NULL);
  cJSON_Delete(s->offer);
  memset(&s->game, 0, sizeof(s->game));
  s->offer = offer = make_offer(s, &why_not);
  s->game_state = game_state(s);
  offer_state(s, offer, why_not);
  installing = s->state == ST_INSTALLING;
  if (installing) {
    snprintf(title, sizeof(title), "%s", game_title(s));
    install_progress(s, how, sizeof(how));
    s->install_told = svc_now();
  } else if (offer && !why_not) {
    const cJSON *sug = cJSON_GetObjectItemCaseSensitive(offer, "suggestion");
    snprintf(title, sizeof(title), "%s", jstr(sug, "name", ""));
  } else {
    snprintf(title, sizeof(title), "%s", rip_disc_kind_name(disc.kind));
  }
  reported = offer ? cJSON_Duplicate(offer, 1) : NULL;
  pthread_mutex_unlock(&g_lock);

  RIP_INFO("%s: %s, %s", s->info.label, title, why_not ? why_not : "ready to rip");
  report_disc(s, have_reader ? &reader : NULL, &disc, profile, why_not ? why_not : "ready to rip", reported);
  if (have_reader) {
    rip_reader_close(&reader);
  }
  pthread_mutex_unlock(&s->io);
  cJSON_Delete(reported);
  // A disc that can be ripped is announced once its name is looked up (details_thread).
  if (installing && settings_notify()) {
    char head[320];
    char body[160];
    // Playable already: the system says so, and the disc must stay in all the same.
    if (s->game.installed) {
      snprintf(head, sizeof(head), "%.250s is still installing: leave the disc in", title);
      snprintf(body, sizeof(body), "It can be played already. %s.", how);
    } else {
      snprintf(head, sizeof(head), "Installing %.250s: leave the disc in", title);
      snprintf(body, sizeof(body), "%s. It can be ripped once it's installed.", how);
    }
    plat_notify(head, body);
  } else if (why_not && settings_notify()) {
    char head[320];
    snprintf(head, sizeof(head), "Can't rip this disc: %.280s", title);
    plat_notify(head, why_not);
  }
  // "Rip as soon as a disc is ready" starts it once the name is looked up too (details_thread).
}

// The disc's profile as a rip request, started as it is.
static void auto_start(struct slot *s, unsigned seq) {
  char err[256];
  cJSON *req;

  if (drivetest_holds(s->info.id)) {
    return;  // a drive test takes every disc as it comes
  }
  pthread_mutex_lock(&g_lock);
  req = s->disc_seq == seq && s->state == ST_READY && s->offer
            ? cJSON_Duplicate(cJSON_GetObjectItemCaseSensitive(s->offer, "suggestion"), 1)
            : NULL;
  pthread_mutex_unlock(&g_lock);
  if (!req) {
    return;
  }
  jset_str(req, "drive", s->info.id);
  jset_num(req, "disc_seq", seq);
  if (jobs_start(req, NULL, err, sizeof(err)) != RIP_OK) {
    RIP_WARN("auto start: %s", err);
  }
  cJSON_Delete(req);
}

/* Watching */

static void close_slot(struct slot *s) {
  if (s->open) {
    rip_transport_close(&s->t);
    pthread_mutex_destroy(&s->io);
  }
  cJSON_Delete(s->offer);
  details_free(&s->details);
  memset(s, 0, sizeof(*s));
}

// Opens drives that appeared, forgets ones that went (not while ripping).
static void rescan(void) {
  struct plat_drive found[MAX_SLOTS];
  int n = plat_drive_list(found, MAX_SLOTS);
  int i;
  int k;

  pthread_mutex_lock(&g_lock);
  for (i = 0; i < MAX_SLOTS; i++) {
    struct slot *s = &g_slots[i];
    int still = 0;
    if (!s->used || s->busy) {
      continue;
    }
    for (k = 0; k < n; k++) {
      still |= !strcmp(found[k].id, s->info.id);
    }
    if (!still) {
      RIP_INFO("%s was disconnected", s->info.label);
      close_slot(s);
    }
  }
  for (k = 0; k < n; k++) {
    struct slot *s;
    char caps[128];
    if (find(found[k].id)) {
      continue;
    }
    for (i = 0; i < MAX_SLOTS && g_slots[i].used; i++) {
    }
    if (i == MAX_SLOTS) {
      break;
    }
    s = &g_slots[i];
    memset(s, 0, sizeof(*s));
    s->info = found[k];
    if (plat_drive_open(s->info.id, &s->t) != RIP_OK) {
      continue;
    }
    if (rip_drive_probe(&s->drive, &s->t, s->info.source) != RIP_OK) {
      rip_transport_close(&s->t);
      continue;
    }
    s->used = 1;
    s->open = 1;
    pthread_mutex_init(&s->io, NULL);
    set_state(s, ST_EMPTY, "");
    rip_caps_str(s->drive.caps, caps, sizeof(caps));
    RIP_INFO("drive %s (%s): %s %s %s, reads %s", s->info.id, s->info.label, s->drive.inquiry.vendor,
             s->drive.inquiry.product, s->drive.inquiry.revision, caps);
  }
  pthread_mutex_unlock(&g_lock);
}

// A disc the drive won't read gets a report too, once the lock is let go.
static void report_unreadable(struct slot *s) {
  char outcome[256];

  pthread_mutex_lock(&g_lock);
  svc_str_copy(outcome, sizeof(outcome), s->message);
  pthread_mutex_unlock(&g_lock);
  pthread_mutex_lock(&s->io);
  report_disc(s, NULL, NULL, s->drive.last_profile, outcome, NULL);
  pthread_mutex_unlock(&s->io);
}

static void poll_slot(struct slot *s) {
  uint16_t profile = 0;
  enum rip_media_state media;
  int want;
  int unreadable = 0;
  int told = 0;
  unsigned ready_seq = 0;
  struct rip_disc unnamed;
  unsigned unnamed_seq = 0;
  char head[320];
  char body[160];

  pthread_mutex_lock(&g_lock);
  if (!s->used || s->busy) {
    pthread_mutex_unlock(&g_lock);
    return;
  }
  want = s->wanted_identify;
  s->wanted_identify = 0;
  pthread_mutex_unlock(&g_lock);

  if (pthread_mutex_trylock(&s->io) != 0) {
    return;  // a job is reading it
  }
  media = rip_drive_media(&s->drive, &profile);
  pthread_mutex_unlock(&s->io);

  pthread_mutex_lock(&g_lock);
  if (s->busy) {  // a job took it meanwhile
    pthread_mutex_unlock(&g_lock);
    return;
  }
  // A disc going into the console's own drive: the system shows it on its
  // home screen, over RipperPS (plat_disc_inserted gives the focus back).
  if (s->polled && s->state == ST_EMPTY && media != RIP_MEDIA_EMPTY && media != RIP_MEDIA_ERROR &&
      s->info.source == RIP_DRIVE_INTERNAL && !s->info.is_virtual && settings_keep_front()) {
    plat_disc_inserted();
  }
  s->polled = 1;
  switch (media) {
  case RIP_MEDIA_EMPTY:
    if (s->state != ST_EMPTY) {
      RIP_INFO("%s: empty", s->info.label);
    }
    s->identified = 0;
    cJSON_Delete(s->offer);
    s->offer = NULL;
    set_state(s, ST_EMPTY, "");
    break;
  case RIP_MEDIA_NOT_READY:
    if (!s->identified) {
      uint64_t now = svc_mono_ms();
      if (s->state != ST_LOADING) {
        s->loading_since = now;
        s->loading_logged = 0;
        set_state(s, ST_LOADING, "");
      }
      // A disc that never gets ready: say what the drive says, then give up waiting.
      if (now - s->loading_since > 15000 && !s->loading_logged) {
        s->loading_logged = 1;
        RIP_INFO("%s: still not ready after 15 s: profile %s (0x%04x), sense %02x/%02x/%02x", s->info.label,
                 mmc_profile_name(s->drive.last_profile), s->drive.last_profile, s->drive.last_sense.key,
                 s->drive.last_sense.asc, s->drive.last_sense.ascq);
      }
      if (now - s->loading_since > 30000) {
        char msg[200];
        if (!(s->drive.caps & RIP_CAP_READ_CD) && s->drive.last_profile == MMC_PROFILE_NONE) {
          // A disc it can't see at all: on a drive without a CD laser, almost surely a CD.
          snprintf(msg, sizeof(msg),
                   "The drive doesn't recognize this disc. %s can't read CDs: music CDs, PS1 and other CD games "
                   "need a USB drive.",
                   s->info.source == RIP_DRIVE_INTERNAL ? "The PS5's drive" : "This drive");
        } else {
          snprintf(msg, sizeof(msg), "The drive never got ready with this disc (sense %02x/%02x/%02x).",
                   s->drive.last_sense.key, s->drive.last_sense.asc, s->drive.last_sense.ascq);
        }
        s->identified = 1;
        s->disc_seq = ++g_seq;
        set_state(s, ST_UNSUPPORTED, msg);
        RIP_WARN("%s: %s", s->info.label, msg);
        unreadable = 1;
      }
    }
    break;
  case RIP_MEDIA_INCOMPATIBLE:
    if (s->state != ST_UNSUPPORTED) {
      s->disc_seq = ++g_seq;
      set_state(s, ST_UNSUPPORTED,
                s->info.source == RIP_DRIVE_INTERNAL && !(s->drive.caps & RIP_CAP_READ_CD)
                    ? "The PS5's drive won't read this disc. It takes PS4 and PS5 games, Blu-ray and DVD; "
                      "CDs need a USB drive."
                    : "Can't read this disc.");
      RIP_INFO("%s: %s", s->info.label, s->message);
      unreadable = 1;
    }
    break;
  case RIP_MEDIA_ERROR:
    break;  // gone; the rescan finds out
  case RIP_MEDIA_PRESENT:
    if (!s->identified || want) {
      set_state(s, ST_IDENTIFYING, "");
      s->identified = 1;  // only once per disc, even if it fails
      pthread_mutex_unlock(&g_lock);
      identify(s, profile);
      return;
    }
    // A game installing, or done installing, after its disc went in.
    if ((s->state == ST_READY || s->state == ST_UNSUPPORTED || s->state == ST_INSTALLING) && s->offer) {
      struct plat_dump_status was = s->game;
      enum state before = s->state;
      int game = game_state(s);
      if (game != s->game_state) {
        redo_offer(s, game);
      }
      told = install_news(s, &was, head, sizeof(head), body, sizeof(body));
      ready_seq = before != ST_READY && s->state == ST_READY ? s->disc_seq : 0;
    }
    // Its name: the system mounts a game disc a while after the drive has it.
    if ((s->disc.kind == RIP_DISC_PS4_BD || s->disc.kind == RIP_DISC_PS5_BD) && !s->disc.title[0]) {
      unnamed = s->disc;
      unnamed_seq = s->disc_seq;
    }
    break;
  }
  pthread_mutex_unlock(&g_lock);
  if (unreadable) {
    report_unreadable(s);
  }
  if (unnamed_seq && rip_disc_title_from_mount(&unnamed, plat_drive_mount(s->info.id))) {
    pthread_mutex_lock(&g_lock);
    if (s->disc_seq == unnamed_seq && !s->disc.title[0]) {
      char name[256];
      svc_str_copy(s->disc.title, sizeof(s->disc.title), unnamed.title);
      RIP_INFO("%s: the system mounted the disc: '%s'", s->info.label, s->disc.title);
      suggest_name(&s->disc, name, sizeof(name));
      if (s->offer) {
        jset_str(cJSON_GetObjectItemCaseSensitive(s->offer, "suggestion"), "name", name);
      }
      start_details(s, NULL);
    }
    pthread_mutex_unlock(&g_lock);
  }
  if (told) {
    plat_notify(head, body);
  }
  // "Rip as soon as a disc is ready": a game is, once installed.
  if (ready_seq && settings_auto_start()) {
    auto_start(s, ready_seq);
  }
}

static void *watch_thread(void *arg) {
  unsigned polls = 0;
  int i;

  (void)arg;
  for (;;) {
    if (polls++ % RESCAN_POLLS == 0) {
      rescan();
    }
    for (i = 0; i < MAX_SLOTS; i++) {
      poll_slot(&g_slots[i]);
    }
    usleep(POLL_MS * 1000);
  }
  return NULL;
}

void drives_start(void) {
  rescan();
  svc_thread(watch_thread, NULL);
}

/* API */

cJSON *drives_json(void) {
  cJSON *arr = cJSON_CreateArray();
  int i;

  pthread_mutex_lock(&g_lock);
  for (i = 0; i < MAX_SLOTS; i++) {
    struct slot *s = &g_slots[i];
    cJSON *j;
    char caps[128];
    if (!s->used) {
      continue;
    }
    j = cJSON_CreateObject();
    cJSON_AddStringToObject(j, "id", s->info.id);
    cJSON_AddStringToObject(j, "label", s->info.label);
    cJSON_AddStringToObject(j, "source", s->info.is_virtual ? "virtual" : rip_drive_source_name(s->info.source));
    cJSON_AddStringToObject(j, "vendor", s->drive.inquiry.vendor);
    cJSON_AddStringToObject(j, "product", s->drive.inquiry.product);
    cJSON_AddStringToObject(j, "revision", s->drive.inquiry.revision);
    rip_caps_str(s->drive.caps, caps, sizeof(caps));
    cJSON_AddStringToObject(j, "reads", caps);
    cJSON_AddStringToObject(j, "state", STATE_NAMES[s->state]);
    cJSON_AddStringToObject(j, "message", s->message);
    cJSON_AddNumberToObject(j, "disc_seq", s->disc_seq);
    if (s->identified && s->disc.profile != 0 && s->state != ST_EMPTY && s->state != ST_LOADING &&
        s->state != ST_IDENTIFYING) {
      cJSON *d = disc_json(&s->disc);
      // What to call it on screen and a cover, once looked up.
      cJSON_AddStringToObject(d, "details", s->details_state == 1 ? "looking" : s->details_state == 2 ? "done" : "");
      cJSON_AddStringToObject(d, "display", s->details.title);
      cJSON_AddStringToObject(d, "subtitle", s->details.subtitle);
      cJSON_AddStringToObject(d, "artist", s->details.artist);
      if (s->details.cover) {
        char url[96];
        snprintf(url, sizeof(url), "/api/drives/%s/cover?seq=%u", s->info.id, s->disc_seq);
        cJSON_AddStringToObject(d, "cover", url);
      }
      cJSON_AddItemToObject(j, "disc", d);
    }
    if (s->offer) {
      cJSON_AddItemToObject(j, "offer", cJSON_Duplicate(s->offer, 1));
    }
    // How far the console is with installing the disc's game, as a job's progress is given.
    if (s->state == ST_INSTALLING) {
      const struct plat_dump_status *st = &s->game;
      cJSON *in = cJSON_AddObjectToObject(j, "install");
      cJSON_AddNumberToObject(in, "done", (double)st->copied);
      cJSON_AddNumberToObject(in, "total", (double)st->to_copy);
      cJSON_AddNumberToObject(in, "rate", s->install_rate);
      cJSON_AddNumberToObject(in, "eta",
                              s->install_rate > 1 ? (double)(st->to_copy - st->copied) / s->install_rate : 0);
      cJSON_AddBoolToObject(in, "playable", st->installed);
    }
    if (s->busy || s->state == ST_DONE) {
      cJSON_AddNumberToObject(j, "job", s->job_id);
    }
    cJSON_AddItemToArray(arr, j);
  }
  pthread_mutex_unlock(&g_lock);
  return arr;
}

int drives_eject(const char *id, char *err, size_t en) {
  struct slot *s;
  uint8_t cdb[6] = {0x1B, 0, 0, 0, 0x02, 0};  // START STOP UNIT: eject
  int rc;

  pthread_mutex_lock(&g_lock);
  s = find(id);
  if (!s || s->busy || s->state == ST_INSTALLING) {
    snprintf(err, en,
             !s        ? "no such drive"
             : s->busy ? "the drive is busy ripping"
                       : "the console is still installing the game from this disc: eject it once that's done");
    pthread_mutex_unlock(&g_lock);
    return RIP_ERR;
  }
  pthread_mutex_unlock(&g_lock);
  // Unlock the tray first; some drives refuse to eject a disc the system locked.
  pthread_mutex_lock(&s->io);
  {
    uint8_t allow[6] = {0x1E, 0, 0, 0, 0, 0};
    rip_scsi(&s->t, allow, sizeof(allow), RIP_DIR_NONE, NULL, 0, 5000, NULL);
  }
  rc = rip_scsi(&s->t, cdb, sizeof(cdb), RIP_DIR_NONE, NULL, 0, 30000, NULL);
  pthread_mutex_unlock(&s->io);
  if (rc != RIP_OK) {
    snprintf(err, en, "the drive refused to eject");
  }
  return rc;
}

int drives_reidentify(const char *id, char *err, size_t en) {
  struct slot *s;

  pthread_mutex_lock(&g_lock);
  s = find(id);
  if (s && !s->busy) {
    s->wanted_identify = 1;
  }
  pthread_mutex_unlock(&g_lock);
  if (!s) {
    snprintf(err, en, "no such drive");
    return RIP_ERR;
  }
  return RIP_OK;
}

int drives_load_image(const char *id, const char *path, char *err, size_t en) {
  struct slot *s;
  int rc;

  pthread_mutex_lock(&g_lock);
  s = find(id);
  if (!s || s->busy || !s->info.is_virtual) {
    snprintf(err, en,
             !s        ? "no such drive"
             : s->busy ? "the drive is busy ripping"
                       : "only a virtual drive takes images");
    pthread_mutex_unlock(&g_lock);
    return RIP_ERR;
  }
  s->identified = 0;
  pthread_mutex_unlock(&g_lock);
  pthread_mutex_lock(&s->io);
  rc = plat_drive_load(id, &s->t, path);
  pthread_mutex_unlock(&s->io);
  if (rc != RIP_OK) {
    snprintf(err, en, "could not load %s", path);
  }
  return rc;
}

int drives_claim(const char *id, unsigned disc_seq, int job_id, struct drive_claim *c, char *err, size_t en) {
  struct slot *s;

  pthread_mutex_lock(&g_lock);
  s = find(id);
  if (!s) {
    snprintf(err, en, "no such drive");
  } else if (s->busy) {
    snprintf(err, en, "%s is already ripping", s->info.label);
  } else if (s->state != ST_READY && s->state != ST_DONE && job_id != 0) {
    snprintf(err, en, "%s has no disc ready", s->info.label);
  } else if (disc_seq && disc_seq != s->disc_seq) {
    snprintf(err, en, "the disc in %s changed; check it again", s->info.label);
  } else {
    s->busy = 1;
    s->job_id = job_id;
    s->prev_state = s->state;
    svc_str_copy(s->prev_message, sizeof(s->prev_message), s->message);
    set_state(s, ST_RIPPING, job_id ? "" : "Testing read speed");
    svc_str_copy(c->id, sizeof(c->id), s->info.id);
    svc_str_copy(c->label, sizeof(c->label), s->info.label);
    c->t = &s->t;
    c->io = &s->io;
    c->drive = &s->drive;
    c->disc = s->disc;
    c->disc_seq = s->disc_seq;
    c->cover = NULL;
    c->cover_len = 0;
    c->audio = NULL;
    if (job_id && s->details.cover && (c->cover = malloc(s->details.cover_len)) != NULL) {
      memcpy(c->cover, s->details.cover, s->details.cover_len);
      c->cover_len = s->details.cover_len;
    }
    if (job_id && s->details.audio && (c->audio = malloc(sizeof(*c->audio))) != NULL) {
      *c->audio = *s->details.audio;
    }
    pthread_mutex_unlock(&g_lock);
    return RIP_OK;
  }
  pthread_mutex_unlock(&g_lock);
  return RIP_ERR;
}

void drives_claim_free(struct drive_claim *c) {
  free(c->cover);
  free(c->audio);
  c->cover = NULL;
  c->audio = NULL;
}

void drives_release(const char *id, int ok, const char *message) {
  struct slot *s;
  int eject;
  char err[64];

  pthread_mutex_lock(&g_lock);
  s = find(id);
  if (!s) {
    pthread_mutex_unlock(&g_lock);
    return;
  }
  s->busy = 0;
  // A benchmark (no job) leaves the drive as it found it.
  if (s->job_id == 0) {
    set_state(s, s->prev_state, s->prev_message);
  } else {
    set_state(s, ok ? ST_DONE : ST_READY, message);
  }
  eject = ok && settings_auto_eject() && !s->info.is_virtual;
  pthread_mutex_unlock(&g_lock);
  if (eject) {
    drives_eject(id, err, sizeof(err));
  }
}

void drives_dumped(const char *id, unsigned disc_seq) {
  struct slot *s;
  int eject = 0;
  char err[64];

  pthread_mutex_lock(&g_lock);
  s = find(id);
  if (s && !s->busy && s->disc_seq == disc_seq && s->state == ST_READY) {
    set_state(s, ST_DONE, "Ripped");
    eject = settings_auto_eject() && !s->info.is_virtual;
  }
  pthread_mutex_unlock(&g_lock);
  if (eject) {
    drives_eject(id, err, sizeof(err));
  }
}

int drives_peek(const char *id, char *state, size_t sn, unsigned *disc_seq, int *busy) {
  struct slot *s;

  pthread_mutex_lock(&g_lock);
  s = find(id);
  if (s) {
    svc_str_copy(state, sn, STATE_NAMES[s->state]);
    *disc_seq = s->disc_seq;
    *busy = s->busy;
  }
  pthread_mutex_unlock(&g_lock);
  return s ? RIP_OK : RIP_ENOENT;
}

cJSON *drives_one_json(const char *id) {
  cJSON *all = drives_json();
  cJSON *d;
  cJSON *out = NULL;

  cJSON_ArrayForEach(d, all) {
    if (!strcmp(jstr(d, "id", ""), id)) {
      out = cJSON_Duplicate(d, 1);
      break;
    }
  }
  cJSON_Delete(all);
  return out;
}

int drives_report_dir(const char *id, unsigned disc_seq, char *out, size_t n) {
  struct slot *s;
  int rc = RIP_ENOENT;

  pthread_mutex_lock(&g_lock);
  s = find(id);
  if (s && s->report_seq == disc_seq && s->report_dir[0]) {
    svc_str_copy(out, n, s->report_dir);
    rc = RIP_OK;
  }
  pthread_mutex_unlock(&g_lock);
  return rc;
}

void drives_busy_message(const char *id, const char *message) {
  struct slot *s;

  pthread_mutex_lock(&g_lock);
  s = find(id);
  if (s && s->busy) {
    svc_str_copy(s->message, sizeof(s->message), message);
  }
  pthread_mutex_unlock(&g_lock);
}
