#ifndef RIPPER_DRIVES_H_
#define RIPPER_DRIVES_H_

// The drive watcher: one slot per optical drive the platform reports, polled
// for discs. A disc found is identified and gets an offer: what it is, the
// formats it can go to on this drive, and the profile's suggestion, which the
// UI shows for the user to confirm or change before a rip starts.
//
// States, as the API names them:
//   empty        no disc
//   loading      a disc is spinning up
//   identifying  reading what it is
//   ready        identified, waiting for the user (or auto_start)
//   unsupported  this drive can't read it, or nothing can be made of it yet
//   ripping      a job is reading it
//   done         ripped; the disc is still in

#include <pthread.h>
#include <stddef.h>
#include <stdint.h>

#include "cJSON.h"
#include "ripper/disc.h"
#include "ripper/drive.h"
#include "ripper/reader.h"
#include "ripper/transport.h"

void drives_start(void);
cJSON *drives_json(void);

int drives_eject(const char *id, char *err, size_t en);
// Looks the disc up again by a name the user gave it (a movie's real title).
int drives_lookup(const char *id, const char *name, char *err, size_t en);
// The cover the details lookup found for the disc in it (malloc'd), RIP_ENOENT if none.
int drives_cover(const char *id, uint8_t **out, size_t *len, char *type, size_t tn);
int drives_reidentify(const char *id, char *err, size_t en);
int drives_load_image(const char *id, const char *path, char *err, size_t en);

// For a job: takes the drive while its disc is read. The transport and drive
// stay the watcher's; nothing else touches them until drives_release.
struct drive_claim {
  char id[16];
  char label[64];
  struct rip_transport *t;
  pthread_mutex_t *io;  // held by the job while it reads
  struct rip_drive *drive;
  struct rip_disc disc;
  unsigned disc_seq;
  // What the details lookup found (copies, or NULL): drives_claim_free() lets them go.
  uint8_t *cover;
  size_t cover_len;
  struct rip_audio_meta *audio;
};
int drives_claim(const char *id, unsigned disc_seq, int job_id, struct drive_claim *c, char *err, size_t en);
void drives_claim_free(struct drive_claim *c);
// ok: the rip worked, so the drive shows "done" (or ejects, if set to).
void drives_release(const char *id, int ok, const char *message);
// A PS4/PS5 game dumped from the drive's disc (released with ok 0, as the
// game needs its disc in to start): "done", and ejected if set to, if that
// disc is still in.
void drives_dumped(const char *id, unsigned disc_seq);

#endif
