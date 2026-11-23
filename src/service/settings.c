#include "settings.h"

#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>

#include "catalog.h"
#include "platform.h"
#include "ripper/aacs.h"
#include "ripper/common.h"
#include "ripper/keydb.h"
#include "ripper/log.h"
#include "svc.h"

static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static cJSON *g_cfg;

static cJSON *clean_profile(const struct cat_system *s, const cJSON *in);

static void config_path(char *out, size_t n) {
  svc_path_join(out, n, plat_data_dir(), "config.json");
}

static void put_default_str(cJSON *j, const char *k, const char *v) {
  if (!cJSON_IsString(cJSON_GetObjectItemCaseSensitive(j, k))) {
    jset_str(j, k, v);
  }
}

static void put_default_num(cJSON *j, const char *k, double v) {
  if (!cJSON_IsNumber(cJSON_GetObjectItemCaseSensitive(j, k))) {
    jset_num(j, k, v);
  }
}

static void put_default_bool(cJSON *j, const char *k, int v) {
  if (!cJSON_IsBool(cJSON_GetObjectItemCaseSensitive(j, k))) {
    jset_bool(j, k, v);
  }
}

// Key files, per kind of disc that needs one. Each has a default place under
// the data folder, used unless the user picks another file.
struct key_type {
  const char *id;
  const char *label;
  const char *file;                 // the default's name, in <data>/keys
  long (*count)(const char *path);  // how many discs it has keys for, -1 when it can't be read
};

static long count_ps3(const char *path) {
  struct rip_keydb db;
  long n;

  memset(&db, 0, sizeof(db));
  n = rip_keydb_load(&db, path) == RIP_OK ? (long)db.count : -1;
  rip_keydb_free(&db);
  return n;
}

// A full KEYDB.cfg is tens of MB: counted once for each version of the file.
static long count_keydb(const char *path) {
  static char last[SVC_PATH];
  static long n = -1;
  static time_t mtime;
  static off_t size;
  static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
  struct stat st;
  long got;

  if (stat(path, &st) != 0) {
    return -1;
  }
  pthread_mutex_lock(&lock);
  if (strcmp(last, path) != 0 || st.st_mtime != mtime || st.st_size != size) {
    n = aacs_keydb_count(path);
    svc_str_copy(last, sizeof(last), path);
    mtime = st.st_mtime;
    size = st.st_size;
  }
  got = n;
  pthread_mutex_unlock(&lock);
  return got;
}

// A "NAME = hex" key file (the PS4 package keys): how many keys it names.
static long count_named(const char *path) {
  FILE *f = fopen(path, "r");
  char line[4096];
  long n = 0;

  if (!f) {
    return -1;
  }
  while (fgets(line, sizeof(line), f)) {
    const char *p = line;
    while (*p == ' ' || *p == '\t') {
      p++;
    }
    if (*p && *p != '#' && strchr(p, '=')) {
      n++;
    }
  }
  fclose(f);
  return n;
}

static const struct key_type KEY_TYPES[] = {
    {"ps3", "PS3 Disc Keys", "ps3.keys", count_ps3},
    {"bluray", "Blu-ray Keys", "KEYDB.cfg", count_keydb},
    {"ps4", "PS4 Package Keys", "ps4-pkg.keys", count_named},
};

static const struct key_type *find_key_type(const char *id) {
  size_t i;

  for (i = 0; id && i < RIP_ARRAY_LEN(KEY_TYPES); i++) {
    if (!strcmp(KEY_TYPES[i].id, id)) {
      return &KEY_TYPES[i];
    }
  }
  return NULL;
}

static void key_default(const struct key_type *t, char *out, size_t n) {
  char dir[SVC_PATH];

  svc_path_join(dir, sizeof(dir), plat_data_dir(), "keys");
  svc_path_join(out, n, dir, t->file);
}

static void fill_defaults(cJSON *j) {
  char path[SVC_PATH];
  cJSON *dests;
  cJSON *d;

  put_default_num(j, "web_port", SVC_DEFAULT_PORT);
  put_default_bool(j, "notify", 1);
  put_default_bool(j, "auto_start", 0);
  put_default_bool(j, "auto_eject", 1);
  cJSON_DeleteItemFromObjectCaseSensitive(j, "keep_front");  // always on now
  put_default_num(j, "read_retries", 3);
  {
    // Each key file's path, the default unless one was picked. "ps3_keys"
    // was the PS3 one's before there were others.
    cJSON *keys = cJSON_GetObjectItemCaseSensitive(j, "keys");
    const char *old = jstr(j, "ps3_keys", "");
    size_t i;
    if (!cJSON_IsObject(keys)) {
      cJSON_DeleteItemFromObjectCaseSensitive(j, "keys");
      keys = cJSON_AddObjectToObject(j, "keys");
    }
    if (old[0] && !jstr(keys, "ps3", "")[0]) {
      jset_str(keys, "ps3", old);
    }
    cJSON_DeleteItemFromObjectCaseSensitive(j, "ps3_keys");
    for (i = 0; i < RIP_ARRAY_LEN(KEY_TYPES); i++) {
      if (!jstr(keys, KEY_TYPES[i].id, "")[0]) {
        key_default(&KEY_TYPES[i], path, sizeof(path));
        jset_str(keys, KEY_TYPES[i].id, path);
      }
    }
  }
  svc_path_join(path, sizeof(path), plat_data_dir(), "staging");
  put_default_str(j, "staging_dir", path);
  put_default_str(j, "ui_theme", "acrylic");
  dests = cJSON_GetObjectItemCaseSensitive(j, "destinations");
  if (!cJSON_IsArray(dests)) {
    d = cJSON_CreateObject();
    cJSON_DeleteItemFromObjectCaseSensitive(j, "destinations");
    dests = cJSON_AddArrayToObject(j, "destinations");
    svc_path_join(path, sizeof(path), plat_data_dir(), "rips");
    cJSON_AddStringToObject(d, "id", "console");
    cJSON_AddStringToObject(d, "type", "local");
    cJSON_AddStringToObject(d, "name", "Local Location");
    cJSON_AddStringToObject(d, "path", path);
    cJSON_AddItemToArray(dests, d);
  }
  // The first destination's old name.
  cJSON_ArrayForEach(d, dests) {
    if (!strcmp(jstr(d, "id", ""), "console") && !strcmp(jstr(d, "name", ""), "This console")) {
      jset_str(d, "name", "Local Location");
    }
  }
  if (!cJSON_IsObject(cJSON_GetObjectItemCaseSensitive(j, "profiles"))) {
    cJSON_DeleteItemFromObjectCaseSensitive(j, "profiles");
    cJSON_AddObjectToObject(j, "profiles");
  }
}

void settings_load(void) {
  char path[SVC_PATH];
  cJSON *j;

  config_path(path, sizeof(path));
  j = jload(path);
  if (!cJSON_IsObject(j)) {
    if (j) {
      RIP_WARN("%s is not valid JSON, so starting from defaults", path);
    }
    cJSON_Delete(j);
    j = cJSON_CreateObject();
  }
  fill_defaults(j);
  {
    // Profiles saved before only what differs was kept: the rest goes.
    cJSON *profiles = cJSON_GetObjectItemCaseSensitive(j, "profiles");
    cJSON *p = profiles ? profiles->child : NULL;
    while (p) {
      cJSON *next = p->next;
      const struct cat_system *sys = catalog_system(p->string);
      if (sys && cJSON_IsObject(p)) {
        cJSON_ReplaceItemInObjectCaseSensitive(profiles, p->string, clean_profile(sys, p));
      }
      p = next;
    }
  }
  pthread_mutex_lock(&g_lock);
  cJSON_Delete(g_cfg);
  g_cfg = j;
  pthread_mutex_unlock(&g_lock);
  settings_save();
}

int settings_save(void) {
  char path[SVC_PATH];
  int rc;

  config_path(path, sizeof(path));
  pthread_mutex_lock(&g_lock);
  rc = jsave(path, g_cfg);
  pthread_mutex_unlock(&g_lock);
  if (rc != 0) {
    RIP_ERROR("could not save %s", path);
  }
  return rc == 0 ? RIP_OK : RIP_ERR;
}

static cJSON *find_dest(const cJSON *dests, const char *id) {
  cJSON *d;

  cJSON_ArrayForEach(d, dests) {
    if (!strcmp(jstr(d, "id", ""), id)) {
      return d;
    }
  }
  return NULL;
}

// Every system's profile, defaults filled in. Caller holds the lock.
static cJSON *merged_profile(const struct cat_system *s) {
  cJSON *p = catalog_default_profile(s);
  const cJSON *mine = cJSON_GetObjectItemCaseSensitive(cJSON_GetObjectItemCaseSensitive(g_cfg, "profiles"), s->id);
  static const char *keys[] = {"format", "destination", "folder", "romm_platform"};
  size_t i;

  if (!cJSON_IsObject(mine)) {
    return p;
  }
  for (i = 0; i < RIP_ARRAY_LEN(keys); i++) {
    const char *v = jstr(mine, keys[i], NULL);
    if (v) {
      jset_str(p, keys[i], v);
    }
  }
  {
    const cJSON *opts = cJSON_GetObjectItemCaseSensitive(mine, "options");
    cJSON *into = cJSON_GetObjectItemCaseSensitive(p, "options");
    const cJSON *o;
    cJSON_ArrayForEach(o, opts) {
      cJSON_DeleteItemFromObjectCaseSensitive(into, o->string);
      cJSON_AddItemToObject(into, o->string, cJSON_Duplicate(o, 1));
    }
  }
  return p;
}

cJSON *settings_json(int with_secrets) {
  cJSON *j;
  cJSON *d;
  cJSON *profiles;
  const struct cat_system *s;

  pthread_mutex_lock(&g_lock);
  j = cJSON_Duplicate(g_cfg, 1);
  profiles = cJSON_CreateObject();
  {
    cJSON *cat = catalog_json();
    cJSON *sys;
    cJSON_ArrayForEach(sys, cJSON_GetObjectItemCaseSensitive(cat, "systems")) {
      s = catalog_system(jstr(sys, "id", ""));
      if (s) {
        cJSON_AddItemToObject(profiles, s->id, merged_profile(s));
      }
    }
    cJSON_Delete(cat);
  }
  pthread_mutex_unlock(&g_lock);
  cJSON_DeleteItemFromObjectCaseSensitive(j, "profiles");
  cJSON_AddItemToObject(j, "profiles", profiles);
  {
    // Each key file: where it is, the default, and whether it's there (how
    // many keys it holds).
    cJSON *files = cJSON_AddArrayToObject(j, "key_files");
    size_t i;
    for (i = 0; i < RIP_ARRAY_LEN(KEY_TYPES); i++) {
      cJSON *f = cJSON_CreateObject();
      char path[SVC_PATH];
      char def[SVC_PATH];
      long count;
      key_default(&KEY_TYPES[i], def, sizeof(def));
      svc_str_copy(path, sizeof(path), jstr(cJSON_GetObjectItemCaseSensitive(j, "keys"), KEY_TYPES[i].id, def));
      count = KEY_TYPES[i].count(path);
      cJSON_AddStringToObject(f, "id", KEY_TYPES[i].id);
      cJSON_AddStringToObject(f, "label", KEY_TYPES[i].label);
      cJSON_AddStringToObject(f, "path", path);
      cJSON_AddStringToObject(f, "default", def);
      cJSON_AddBoolToObject(f, "found", count >= 0);
      cJSON_AddNumberToObject(f, "count", count > 0 ? (double)count : 0);
      cJSON_AddItemToArray(files, f);
    }
  }
  if (!with_secrets) {
    // The movie lookup's key: whether there is one, never the key.
    cJSON_AddBoolToObject(j, "tmdb_key_set", jstr(j, "tmdb_key", "")[0] != '\0');
    cJSON_DeleteItemFromObjectCaseSensitive(j, "tmdb_key");
    cJSON_ArrayForEach(d, cJSON_GetObjectItemCaseSensitive(j, "destinations")) {
      cJSON *f = d->child;
      const char *type = jstr(d, "type", "");
      while (f) {
        cJSON *next = f->next;
        if (catalog_is_secret(type, f->string)) {
          char set_key[64];
          snprintf(set_key, sizeof(set_key), "%s_set", f->string);
          jset_bool(d, set_key, cJSON_IsString(f) && f->valuestring[0]);
          jset_str(d, f->string, "");
        }
        f = next;
      }
    }
  }
  return j;
}

cJSON *settings_destination(const char *id) {
  cJSON *d;

  pthread_mutex_lock(&g_lock);
  d = find_dest(cJSON_GetObjectItemCaseSensitive(g_cfg, "destinations"), id ? id : "");
  d = d ? cJSON_Duplicate(d, 1) : NULL;
  pthread_mutex_unlock(&g_lock);
  return d;
}

cJSON *settings_profile(const char *system) {
  const struct cat_system *s = catalog_system(system);
  cJSON *p;

  if (!s) {
    return NULL;
  }
  pthread_mutex_lock(&g_lock);
  p = merged_profile(s);
  pthread_mutex_unlock(&g_lock);
  return p;
}

/* Changes */

// A destination from the UI, checked, with secrets it left blank kept from old.
static cJSON *clean_destination(const cJSON *in, const cJSON *old, char *err, size_t en) {
  const char *id = jstr(in, "id", "");
  const char *type = jstr(in, "type", "");
  const cJSON *f;
  cJSON *d;

  if (!svc_is_id(id)) {
    snprintf(err, en, "a destination needs an id of lower-case letters, digits and dashes");
    return NULL;
  }
  if (!catalog_dest_type_known(type)) {
    snprintf(err, en, "unknown destination type '%s'", type);
    return NULL;
  }
  if (old && strcmp(jstr(old, "type", ""), type) != 0) {
    old = NULL;  // a new kind of destination keeps nothing of the old one
  }
  d = cJSON_CreateObject();
  cJSON_ArrayForEach(f, in) {
    size_t kl = strlen(f->string);
    if (kl > 4 && !strcmp(f->string + kl - 4, "_set")) {
      continue;
    }
    if (catalog_is_secret(type, f->string)) {
      if (cJSON_IsNull(f)) {
        continue;  // cleared on purpose
      }
      if (!cJSON_IsString(f) || !f->valuestring[0]) {
        const char *keep = old ? jstr(old, f->string, "") : "";
        if (keep[0]) {
          cJSON_AddStringToObject(d, f->string, keep);
        }
        continue;
      }
    }
    if (cJSON_IsString(f) || cJSON_IsNumber(f) || cJSON_IsBool(f)) {
      cJSON_AddItemToObject(d, f->string, cJSON_Duplicate(f, 0));
    }
  }
  // Secrets the UI didn't mention at all stay too.
  if (old) {
    cJSON_ArrayForEach(f, old) {
      if (catalog_is_secret(type, f->string) && !cJSON_GetObjectItemCaseSensitive(in, f->string)) {
        cJSON_AddItemToObject(d, f->string, cJSON_Duplicate(f, 0));
      }
    }
  }
  if (!jstr(d, "name", "")[0]) {
    jset_str(d, "name", id);
  }
  if (!strcmp(type, "usb") && strncmp(jstr(d, "path", ""), "/mnt/usb", 8) != 0) {
    snprintf(err, en, "%s: pick a folder on a USB drive", jstr(d, "name", id));
    cJSON_Delete(d);
    return NULL;
  }
  if (!strcmp(type, "local") && jstr(d, "path", "")[0] != '/') {
    snprintf(err, en, "%s: the folder must be a full path", jstr(d, "name", id));
    cJSON_Delete(d);
    return NULL;
  }
  return d;
}

// A profile as the UI sent it, keeping only what differs from the system's
// defaults: a default changed in a later version then reaches everyone who
// never changed it themselves.
static cJSON *clean_profile(const struct cat_system *s, const cJSON *in) {
  static const char *keys[] = {"format", "destination", "folder", "romm_platform"};
  cJSON *p = cJSON_CreateObject();
  cJSON *def = catalog_default_profile(s);
  enum rip_recipe_id recipe;
  const char *format = jstr(in, "format", jstr(def, "format", ""));
  const cJSON *opts = cJSON_GetObjectItemCaseSensitive(in, "options");
  size_t i;

  for (i = 0; i < RIP_ARRAY_LEN(keys); i++) {
    const char *v = jstr(in, keys[i], NULL);
    if (!v || !strcmp(v, jstr(def, keys[i], ""))) {
      continue;
    }
    if (i == 0 && catalog_recipe_id(v, &recipe) != 0) {
      continue;
    }
    cJSON_AddStringToObject(p, keys[i], v);
  }
  if (cJSON_IsObject(opts)) {
    cJSON *defaults = catalog_format_defaults(format);
    cJSON *keep = cJSON_CreateObject();
    const cJSON *o;
    cJSON_ArrayForEach(o, opts) {
      const cJSON *d = cJSON_GetObjectItemCaseSensitive(defaults, o->string);
      if (!d || !cJSON_Compare(o, d, 1)) {
        cJSON_AddItemToObject(keep, o->string, cJSON_Duplicate(o, 1));
      }
    }
    if (keep->child) {
      cJSON_AddItemToObject(p, "options", keep);
    } else {
      cJSON_Delete(keep);
    }
    cJSON_Delete(defaults);
  }
  cJSON_Delete(def);
  return p;
}

int settings_apply(const cJSON *j, char *err, size_t en) {
  static const char *bools[] = {"notify", "auto_start", "auto_eject"};
  static const char *strs[] = {"ui_theme", "staging_dir", "tmdb_key"};
  const cJSON *keys = cJSON_GetObjectItemCaseSensitive(j, "keys");
  const cJSON *dests = cJSON_GetObjectItemCaseSensitive(j, "destinations");
  const cJSON *profiles = cJSON_GetObjectItemCaseSensitive(j, "profiles");
  cJSON *new_dests = NULL;
  size_t i;

  err[0] = '\0';
  pthread_mutex_lock(&g_lock);
  if (cJSON_IsArray(dests)) {
    const cJSON *d;
    new_dests = cJSON_CreateArray();
    cJSON_ArrayForEach(d, dests) {
      cJSON *old = find_dest(cJSON_GetObjectItemCaseSensitive(g_cfg, "destinations"), jstr(d, "id", ""));
      cJSON *clean = clean_destination(d, old, err, en);
      if (!clean || find_dest(new_dests, jstr(clean, "id", ""))) {
        if (clean) {
          snprintf(err, en, "two destinations share the id '%s'", jstr(clean, "id", ""));
        }
        cJSON_Delete(clean);
        cJSON_Delete(new_dests);
        pthread_mutex_unlock(&g_lock);
        return RIP_ERR;
      }
      cJSON_AddItemToArray(new_dests, clean);
    }
  }
  for (i = 0; i < RIP_ARRAY_LEN(bools); i++) {
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(j, bools[i]);
    if (cJSON_IsBool(v)) {
      jset_bool(g_cfg, bools[i], cJSON_IsTrue(v));
    }
  }
  for (i = 0; i < RIP_ARRAY_LEN(strs); i++) {
    const char *v = jstr(j, strs[i], NULL);
    if (v && (strcmp(strs[i], "ui_theme") != 0 || svc_is_id(v))) {
      jset_str(g_cfg, strs[i], v);
    }
  }
  if (cJSON_IsObject(keys)) {
    // A file's full path, or "" for the default.
    cJSON *into = cJSON_GetObjectItemCaseSensitive(g_cfg, "keys");
    const cJSON *k;
    cJSON_ArrayForEach(k, keys) {
      const struct key_type *t = find_key_type(k->string);
      char path[SVC_PATH];
      if (!t || !cJSON_IsString(k)) {
        continue;
      }
      if (!k->valuestring[0]) {
        key_default(t, path, sizeof(path));
      } else if (k->valuestring[0] != '/' || strstr(k->valuestring, "/..")) {
        snprintf(err, en, "the %s file has to be a full path", t->label);
        pthread_mutex_unlock(&g_lock);
        cJSON_Delete(new_dests);
        return RIP_ERR;
      } else {
        svc_str_copy(path, sizeof(path), k->valuestring);
      }
      jset_str(into, t->id, path);
    }
  }
  if (cJSON_IsNumber(cJSON_GetObjectItemCaseSensitive(j, "read_retries"))) {
    int n = jint(j, "read_retries", 3);
    jset_num(g_cfg, "read_retries", n < 0 ? 0 : n > 20 ? 20 : n);
  }
  if (cJSON_IsNumber(cJSON_GetObjectItemCaseSensitive(j, "web_port"))) {
    int port = jint(j, "web_port", SVC_DEFAULT_PORT);
    if (port > 1024 && port < 65536) {
      jset_num(g_cfg, "web_port", port);
    }
  }
  if (new_dests) {
    cJSON_DeleteItemFromObjectCaseSensitive(g_cfg, "destinations");
    cJSON_AddItemToObject(g_cfg, "destinations", new_dests);
  }
  if (cJSON_IsObject(profiles)) {
    cJSON *into = cJSON_GetObjectItemCaseSensitive(g_cfg, "profiles");
    const cJSON *p;
    cJSON_ArrayForEach(p, profiles) {
      const struct cat_system *s = catalog_system(p->string);
      if (s && cJSON_IsObject(p)) {
        cJSON_DeleteItemFromObjectCaseSensitive(into, s->id);
        cJSON_AddItemToObject(into, s->id, clean_profile(s, p));
      }
    }
  }
  pthread_mutex_unlock(&g_lock);
  return settings_save();
}

int settings_set_secret(const char *dest_id, const char *field, const char *value) {
  cJSON *d;

  pthread_mutex_lock(&g_lock);
  d = find_dest(cJSON_GetObjectItemCaseSensitive(g_cfg, "destinations"), dest_id);
  if (d) {
    jset_str(d, field, value);
  }
  pthread_mutex_unlock(&g_lock);
  return d ? settings_save() : RIP_ENOENT;
}

/* Values */

static int get_int(const char *k, int def) {
  int v;

  pthread_mutex_lock(&g_lock);
  v = jint(g_cfg, k, def);
  pthread_mutex_unlock(&g_lock);
  return v;
}

static void get_str(const char *k, char *out, size_t n) {
  pthread_mutex_lock(&g_lock);
  svc_str_copy(out, n, jstr(g_cfg, k, ""));
  pthread_mutex_unlock(&g_lock);
}

int settings_web_port(void) {
  return get_int("web_port", SVC_DEFAULT_PORT);
}

int settings_notify(void) {
  return get_int("notify", 1);
}

int settings_auto_start(void) {
  return get_int("auto_start", 0);
}

int settings_auto_eject(void) {
  return get_int("auto_eject", 1);
}

unsigned settings_read_retries(void) {
  return (unsigned)get_int("read_retries", 3);
}

void settings_key_file(const char *id, char *out, size_t n) {
  pthread_mutex_lock(&g_lock);
  svc_str_copy(out, n, jstr(cJSON_GetObjectItemCaseSensitive(g_cfg, "keys"), id, ""));
  pthread_mutex_unlock(&g_lock);
}

void settings_tmdb_key(char *out, size_t n) {
  get_str("tmdb_key", out, n);
}

void settings_ui_theme(char *out, size_t n) {
  get_str("ui_theme", out, n);
}

void settings_staging_dir(char *out, size_t n) {
  get_str("staging_dir", out, n);
}
