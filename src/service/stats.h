#ifndef RIPPER_STATS_H_
#define RIPPER_STATS_H_

// What this console has ripped, all told: discs by system, bytes read, hours
// spent, bytes sent on. Kept in <data>/stats.json.

#include <stdint.h>

#include "cJSON.h"

// A finished job: ok counts a disc, otherwise a failure.
void stats_record(const char *system, uint64_t bytes, int64_t seconds, uint64_t sent, int ok);
// {"discs", "failed", "bytes", "seconds", "sent", "since", "systems": {"ps1": 3, ...}}
cJSON *stats_json(void);

#endif
