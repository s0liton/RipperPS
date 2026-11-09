#ifndef RIPPER_JOBS_H_
#define RIPPER_JOBS_H_

// Rips and their delivery. A job takes a drive, reads the disc into a staging
// folder, lets the drive go, then takes the rip to its destination as a stage
// of its own (a rename, a copy to another drive, or an upload). A local
// destination on another filesystem is written to directly when staging
// hasn't the room. Jobs run side by side, one per drive. They're kept in
// jobs.json, running ones too, so one cut short comes back failed.
//
// States: ripping, delivering, done, failed, cancelled.
//
// A request, as the API takes it (from the confirm sheet):
//   {"drive": "cd0", "disc_seq": 3, "format": "chd", "destination": "romm",
//    "name": "Gran Turismo 4 (SCUS-97328)", "folder": "{system}",
//    "romm_platform": "ps2", "options": {"chd_codec": "zlib"}}

#include <stddef.h>
#include <stdint.h>

#include "cJSON.h"

void jobs_init(void);
int jobs_start(const cJSON *request, int *job_id, char *err, size_t en);
cJSON *jobs_json(void);
int jobs_cancel(int id, char *err, size_t en);
// Delivers a failed job's files again (the rip itself worked).
int jobs_retry(int id, char *err, size_t en);
void jobs_clear_finished(void);
int jobs_active(void);
// Keeps the console out of rest mode for that long (0 stops), jobs or not.
void jobs_keep_awake(int seconds);
int64_t jobs_awake_until(void);

#endif
