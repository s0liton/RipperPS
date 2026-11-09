#include "ripper/config.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ripper/common.h"
#include "ripper/log.h"

void rip_config_defaults(struct rip_config *cfg, const char *base_dir) {
  memset(cfg, 0, sizeof(*cfg));
  snprintf(cfg->out_dir, sizeof(cfg->out_dir), "%s/out", base_dir);
  snprintf(cfg->ps3_keys, sizeof(cfg->ps3_keys), "%s/keys/ps3.keys", base_dir);
  cfg->ps3_keep_encrypted = 0;
  cfg->auto_rip = 1;
  cfg->read_retries = 3;
}

static char *trim(char *s) {
  char *end;

  while (*s == ' ' || *s == '\t') {
    s++;
  }
  end = s + strlen(s);
  while (end > s && (end[-1] == ' ' || end[-1] == '\t' || end[-1] == '\r' || end[-1] == '\n')) {
    *--end = '\0';
  }
  return s;
}

int rip_config_load(struct rip_config *cfg, const char *path) {
  char line[1024];
  unsigned lineno = 0;
  FILE *f = fopen(path, "r");

  if (!f) {
    return RIP_ENOENT;
  }
  while (fgets(line, sizeof(line), f)) {
    char *key = trim(line);
    char *eq;
    char *value;

    lineno++;
    if (*key == '#' || *key == '\0') {
      continue;
    }
    eq = strchr(key, '=');
    if (!eq) {
      RIP_WARN("%s:%u: expected key=value", path, lineno);
      continue;
    }
    *eq = '\0';
    key = trim(key);
    value = trim(eq + 1);
    if (strcmp(key, "out_dir") == 0) {
      snprintf(cfg->out_dir, sizeof(cfg->out_dir), "%s", value);
    } else if (strcmp(key, "ps3_keys") == 0) {
      snprintf(cfg->ps3_keys, sizeof(cfg->ps3_keys), "%s", value);
    } else if (strcmp(key, "ps3_keep_encrypted") == 0) {
      cfg->ps3_keep_encrypted = atoi(value) != 0;
    } else if (strcmp(key, "auto_rip") == 0) {
      cfg->auto_rip = atoi(value) != 0;
    } else if (strcmp(key, "read_retries") == 0) {
      cfg->read_retries = (unsigned)strtoul(value, NULL, 10);
    } else if (strcmp(key, "verbose") == 0) {
      cfg->verbose = atoi(value) != 0;
    } else {
      RIP_WARN("%s:%u: unknown key '%s'", path, lineno, key);
    }
  }
  fclose(f);
  return RIP_OK;
}
