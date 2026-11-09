#ifndef RIPPER_DETAILS_H_
#define RIPPER_DETAILS_H_

// What a disc is beyond what it says itself, looked up once it's identified:
// a name to show and to rip under, a line under it, and a cover.
//   - Audio CDs: MusicBrainz (album, artist, year), the Cover Art Archive.
//   - Games with a serial: Redump's name (gamedb), covers by serial (PS1, PS2),
//     the disc's own icon (PS3 ICON0.PNG, PS5 app_sc.pkg's icon0.png).
//   - Blu-ray movies: the disc's own title and thumbnail (BDMV/META).
//   - DVD-Video and data discs: the volume label, tidied.

#include <stddef.h>
#include <stdint.h>

#include "ripper/disc.h"
#include "ripper/rip.h"

struct disc_details {
  char title[256];     // "American Idiot", "Syphon Filter"
  char subtitle[256];  // "Green Day · 2004", "SCUS-94240 · USA"
  char name[256];      // what the rip is called: "Syphon Filter (USA)", the album
  char artist[256];    // audio CDs: the folder template's {artist}
  uint8_t *cover;      // the cover's bytes (JPEG or PNG), malloc'd, or NULL
  size_t cover_len;
  char cover_type[24];           // "image/jpeg"
  struct rip_audio_meta *audio;  // audio CDs: kept for the rip (track titles), malloc'd
};

// Reads a file off the disc (a path from its root); 0 when it could. The
// caller takes the drive's lock around it.
typedef int (*details_read_fn)(void *ud, const char *path, uint8_t **out, size_t *len);

// Blocking (network): run it off the drive watcher's thread. hint: a name
// the user gave it (a movie is looked up by it), or NULL.
void details_lookup(const char *system, const struct rip_disc *disc, const char *mount_root, details_read_fn read,
                    void *ud, const char *hint, struct disc_details *out);
void details_free(struct disc_details *d);

#endif
