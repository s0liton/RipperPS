#ifndef RIPPER_DIAGNOSE_H_
#define RIPPER_DIAGNOSE_H_

// The drive test (core diag.c) run for the web UI: the drive is taken for a
// few minutes, the results zipped into <data>/diag/, ready to download and
// send back. One at a time.

#include <stddef.h>

#include "cJSON.h"

int diagnose_start(const char *drive_id, char *err, size_t en);
void diagnose_cancel(void);
// {"state": "idle|running|done|failed|cancelled", "step", "done", "total", "file",
//  "drive_name", "disc", "findings", "commands", "answered", "error"}
cJSON *diagnose_status(void);
// A finished zip's bytes by its name (free it), NULL if there's no such file.
char *diagnose_file(const char *name, size_t *len);

#endif
