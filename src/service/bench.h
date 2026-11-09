#ifndef RIPPER_BENCH_H_
#define RIPPER_BENCH_H_

#include <stddef.h>

#include "cJSON.h"

// Times reads from the drive's disc and this CPU's compression; for
// telling a slow drive from a slow rip. Takes the drive for about a minute.
cJSON *bench_drive(const char *id, char *err, size_t en);

// Each basic drive command once, its raw result: for discs that never come up.
cJSON *bench_probe(const char *id, char *err, size_t en);

#endif
