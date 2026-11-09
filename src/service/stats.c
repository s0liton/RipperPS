#include "stats.h"

#include <pthread.h>

#include "platform.h"
#include "svc.h"

static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;

static void stats_path(char *out, size_t n) {
  svc_path_join(out, n, plat_data_dir(), "stats.json");
}

static cJSON *load(void) {
  char path[SVC_PATH];
  cJSON *j;

  stats_path(path, sizeof(path));
  j = jload(path);
  if (!cJSON_IsObject(j)) {
    cJSON_Delete(j);
    j = cJSON_CreateObject();
    jset_num(j, "since", (double)svc_now());
  }
  return j;
}

static void add(cJSON *o, const char *key, double v) {
  const cJSON *cur = cJSON_GetObjectItemCaseSensitive(o, key);
  jset_num(o, key, (cJSON_IsNumber(cur) ? cur->valuedouble : 0) + v);
}

void stats_record(const char *system, uint64_t bytes, int64_t seconds, uint64_t sent, int ok) {
  char path[SVC_PATH];
  cJSON *j;
  cJSON *systems;

  pthread_mutex_lock(&g_lock);
  j = load();
  if (ok) {
    add(j, "discs", 1);
    add(j, "bytes", (double)bytes);
    add(j, "seconds", (double)(seconds > 0 ? seconds : 0));
    add(j, "sent", (double)sent);
    systems = cJSON_GetObjectItemCaseSensitive(j, "systems");
    if (!cJSON_IsObject(systems)) {
      systems = cJSON_AddObjectToObject(j, "systems");
    }
    add(systems, system && system[0] ? system : "other", 1);
  } else {
    add(j, "failed", 1);
  }
  stats_path(path, sizeof(path));
  jsave(path, j);
  cJSON_Delete(j);
  pthread_mutex_unlock(&g_lock);
}

cJSON *stats_json(void) {
  cJSON *j;
  static const char *keys[] = {"discs", "failed", "bytes", "seconds", "sent"};
  size_t i;

  pthread_mutex_lock(&g_lock);
  j = load();
  pthread_mutex_unlock(&g_lock);
  for (i = 0; i < sizeof(keys) / sizeof(keys[0]); i++) {
    if (!cJSON_GetObjectItemCaseSensitive(j, keys[i])) {
      jset_num(j, keys[i], 0);
    }
  }
  if (!cJSON_GetObjectItemCaseSensitive(j, "systems")) {
    cJSON_AddObjectToObject(j, "systems");
  }
  return j;
}
