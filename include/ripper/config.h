#ifndef RIPPER_CONFIG_H_
#define RIPPER_CONFIG_H_

// key=value file; '#' starts a comment. Unknown keys are ignored with a warning.
struct rip_config {
  char out_dir[512];
  char ps3_keys[512];
  int ps3_keep_encrypted;
  int auto_rip;       // rip as soon as a disc is identified
  unsigned read_retries;
  int verbose;
};

void rip_config_defaults(struct rip_config *cfg, const char *base_dir);
// RIP_OK, or RIP_ENOENT when the file does not exist (defaults stay).
int rip_config_load(struct rip_config *cfg, const char *path);

#endif
