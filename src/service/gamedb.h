#ifndef RIPPER_GAMEDB_H_
#define RIPPER_GAMEDB_H_

// Game names by serial, from Redump (third_party/gamedb, built in compressed).
// The table is unpacked the first time it's asked, and kept.

#include <stddef.h>

// system: the catalog's id ("ps1", "ps2", "ps3", "saturn", "gamecube"...).
// serial: as the disc has it ("SLUS-00594", "GALE01"). 1 and the Redump name
// ("Syphon Filter (USA)") when it's known.
int gamedb_lookup(const char *system, const char *serial, char *name, size_t n);

// The name without Redump's tags: "Syphon Filter (USA) (Disc 1)" -> "Syphon Filter".
void gamedb_plain_title(const char *name, char *out, size_t n);

#endif
