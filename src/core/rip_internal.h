#ifndef RIPPER_RIP_INTERNAL_H_
#define RIPPER_RIP_INTERNAL_H_

// What rip.c shares with the rips in other files (rip_cd.c).

#include "ripper/disc.h"
#include "ripper/drive.h"
#include "ripper/rip.h"

struct rip_progress {
  const struct rip_job_opts *opts;
  uint64_t next_report;
};

// Passes progress on every 64 MiB, or now when force is set.
void rip_report(struct rip_progress *ps, const char *phase, uint64_t done, uint64_t total, int force);

// Picks "<dir>/<stem><ext>", adding " (2)", " (3)"... instead of overwriting.
int rip_unique_path(const char *dir, const char *stem, const char *ext, char *out, size_t len);

// CDs: BIN/CUE (a folder of tracks and the sheet) or CHD, read as raw frames.
int rip_cd_run(const struct rip_recipe *recipe, struct rip_drive *drive, const struct rip_disc *disc,
               const struct rip_job_opts *opts, const char *stem, struct rip_job_result *res);

#endif
