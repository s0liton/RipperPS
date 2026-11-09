#ifndef RIPPER_MUSICBRAINZ_H_
#define RIPPER_MUSICBRAINZ_H_

// What an audio CD is, from MusicBrainz by its disc ID: the first release
// with that disc, its artist, title and date, and this disc's track titles.

#include <stddef.h>

#include "ripper/rip.h"

// RIP_OK, RIP_ENOENT when MusicBrainz doesn't know the disc, RIP_ERR.
int mb_lookup(const char *discid, struct rip_audio_meta *meta, char *err, size_t en);

#endif
