#include "settings.h"

#include <pthread.h>
#include <stdio.h>
#include <string.h>

#include "catalog.h"
#include "platform.h"
#include "ripper/common.h"
#include "ripper/log.h"
#include "svc.h"

static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static cJSON *g_cfg;

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

static void fill_defaults(cJSON *j) {
  char path[SVC_PATH];
  cJSON *dests;

  put_default_num(j, "web_port", SVC_DEFAULT_PORT);
  put_default_bool(j, "notify", 1);
  put_default_bool(j, "auto_start", 0);
  put_default_bool(j, "auto_eject", 1);
  put_default_bool(j, "keep_front", 1);
  put_default_num(j, "read_retries", 3);
  svc_path_join(path, sizeof(path), plat_data_dir(), "keys/ps3.keys");
  put_default_str(j, "ps3_keys", path);
  svc_path_join(path, sizeof(path), plat_data_dir(), "staging");
  put_default_str(j, "staging_dir", path);
  put_default_str(j, "ui_theme", "acrylic");
  dests = cJSON_GetObjectItemCaseSensitive(j, "destinations");
  if (!cJSON_IsArray(dests)) {
    cJSON *d = cJSON_CreateObject();
    cJSON_DeleteItemFromObjectCaseSensitive(j, "destinations");
    dests = cJSON_AddArrayToObject(j, "destinations");
    svc_path_join(path, sizeof(path), plat_data_dir(), "rips");
    cJSON_AddStringToObject(d, "id", "console");
    cJSON_AddStringToObject(d, "type", "local");
    cJSON_AddStringToObject(d, "name", "This console");
    cJSON_AddStringToObject(d, "path", path);
    cJSON_AddItemToArray(dests, d);
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
      RIP_WARN("%s is not valid JSON; starting from defaults", path);
    }
    cJSON_Delete(j);
    j = cJSON_CreateObject();
  }
  fill_defaults(j);
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
  if (!strcmp(type, "local") && jstr(d, "path", "")[0] != '/') {
    snprintf(err, en, "%s: the folder must be a full path", jstr(d, "name", id));
    cJSON_Delete(d);
    return NULL;
  }
  return d;
}

static cJSON *clean_profile(const struct cat_system *s, const cJSON *in) {
  cJSON *p = cJSON_CreateObject();
  enum rip_recipe_id recipe;
  const char *format = jstr(in, "format", "");
  const cJSON *opts = cJSON_GetObjectItemCaseSensitive(in, "options");

  if (catalog_recipe_id(format, &recipe) == 0) {
    cJSON_AddStringToObject(p, "format", format);
  }
  if (jstr(in, "destination", NULL)) {
    cJSON_AddStringToObject(p, "destination", jstr(in, "destination", ""));
  }
  if (jstr(in, "folder", NULL)) {
    cJSON_AddStringToObject(p, "folder", jstr(in, "folder", ""));
  }
  if (jstr(in, "romm_platform", NULL)) {
    cJSON_AddStringToObject(p, "romm_platform", jstr(in, "romm_platform", ""));
  }
  if (cJSON_IsObject(opts)) {
    cJSON_AddItemToObject(p, "options", cJSON_Duplicate(opts, 1));
  }
  (void)s;
  return p;
}

int settings_apply(const cJSON *j, char *err, size_t en) {
  static const char *bools[] = {"notify", "auto_start", "auto_eject", "keep_front"};
  static const char *strs[] = {"ps3_keys", "ui_theme", "staging_dir", "tmdb_key"};
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

int settings_keep_front(void) {
  return get_int("keep_front", 1);
}

int settings_auto_eject(void) {
  return get_int("auto_eject", 1);
}

unsigned settings_read_retries(void) {
  return (unsigned)get_int("read_retries", 3);
}

void settings_ps3_keys(char *out, size_t n) {
  get_str("ps3_keys", out, n);
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
