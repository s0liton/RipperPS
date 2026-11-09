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

// For the drive test (drivetest.h), which follows a drive disc by disc.
// The state's name ("ready", ...), the disc's number and whether something has
// the drive; RIP_ENOENT if there's no such drive.
int drives_peek(const char *id, char *state, size_t sn, unsigned *disc_seq, int *busy);
// The drive as /api/drives gives it (free it), NULL if there's no such drive.
cJSON *drives_one_json(const char *id);
// The folder of the report written for that disc, RIP_ENOENT if none (yet).
int drives_report_dir(const char *id, unsigned disc_seq, char *out, size_t n);
// What a claimed drive shows while it's held: "Testing the drive".
void drives_busy_message(const char *id, const char *message);

#endif
