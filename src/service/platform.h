#ifndef RIPPER_PLATFORM_H_
#define RIPPER_PLATFORM_H_

// What the service needs from the machine it runs on. One implementation per
// build: src/platform/ps5/platform.c (the console) and
// src/platform/linux/platform.c (a PC, for development, with a virtual drive).

#include <stddef.h>
#include <stdint.h>

#include "ripper/drive.h"
#include "ripper/log.h"
#include "ripper/reader.h"
#include "ripper/transport.h"

const char *plat_name(void);     // "ps5" or "linux"
const char *plat_console(void);  // "PlayStation 5", for RomM

// Names the process and makes it the only copy running. 1 if a newer copy
// runs and this one should quit, 0 to carry on.
int plat_init(void);

const char *plat_data_dir(void);
const char *plat_rommps_dir(void);

void plat_log_line(enum rip_log_level level, const char *line);
// A toast on the TV (stderr on the host). title in bold, body under it.
void plat_notify(const char *title, const char *body);
// Holds off the console's idle rest mode for a while; called while jobs run
// (a rip nobody touches the controller during would otherwise be cut short).
void plat_keep_awake(void);
// A disc just went into the console's drive. The system answers by showing
// the disc on its home screen, sending the app in front to the background:
// when that app is RipperPS, it's given the focus back (PS5; nothing elsewhere).
void plat_disc_inserted(void);
// 1 while RipperPS is the app in front (PS5); 0 when it isn't, isn't running,
// or that can't be told (and on the host).
int plat_app_in_front(void);
void plat_local_ip(char *buf, size_t n);

// Optical drives. id is stable while the drive stays attached ("cd0",
// "virtual0"); label is what the UI calls it ("Built-in Blu-ray").
struct plat_drive {
  char id[16];
  char label[64];
  enum rip_drive_source source;
  int is_virtual;  // an image drive: takes images instead of discs
};

// The drives attached now.
int plat_drive_list(struct plat_drive *out, int max);
int plat_drive_open(const char *id, struct rip_transport *t);
// The fastest sector source for a disc in the drive (the PS5's block device),
// falling back to READ(12) through t.
int plat_drive_reader(const char *id, struct rip_transport *t, struct rip_reader *r);
// Where the system mounted the drive's disc, or NULL.
const char *plat_drive_mount(const char *id);
// Virtual drives: insert an image. RIP_ENOTSUP for real drives.
int plat_drive_load(const char *id, struct rip_transport *t, const char *image_path);

// Folders a local destination can be in: internal storage, USB drives.
// Each root is {path, label}; returns how many.
struct plat_root {
  char path[256];
  char label[64];
};
int plat_storage_roots(struct plat_root *out, int max);

// PS4 and PS5 game discs: the console decrypts them itself. A dump copies
// the installed game's files from the system's mount of it and decrypts its
// executables on the way.
struct plat_dump_status {
  int supported;           // this platform can dump games at all
  int installed;           // the title is installed (a disc game must be, to run)
  int complete;            // ... all of it: a disc game plays before its install finishes
  int installing;          // the system is copying it now, playable or not yet
  int partial;             // parts this console never fetches (other languages): a dump would hang
  uint64_t copied;         // ... this much so far
  uint64_t to_copy;        // ... of this much
  int mounted;             // the system has its files mounted now
  int running;             // another game is in the foreground (a launch closes it)
  uint64_t size;           // its package as installed so far
  uint64_t expected_size;  // the whole package, 0 when unknown
};
void plat_game_status(const char *title_id, struct plat_dump_status *st);
// Dumps title_id into out_dir (created). Starts the game first when it isn't
// mounted, which closes the one running. progress gets bytes over the whole
// game; a nonzero return or *cancel stops it.
typedef int (*plat_progress_fn)(void *ud, const char *phase, uint64_t done, uint64_t total);
int plat_game_dump(const char *title_id, const char *out_dir, plat_progress_fn progress, void *ud, volatile int *cancel,
                   char *err, size_t en);

// Installed games (PS4 and PS5), for dumping without their disc in.
struct plat_game {
  char title_id[16];  // as the system names it: "CUSA06122"
  char name[128];
  int ps5;
  int app;  // a media app (Netflix, Plex...), not a game
  uint64_t size;
};
int plat_game_list(struct plat_game *out, int max);

// PEM roots for HTTPS, or NULL for the system's.
const char *plat_ca_pem(size_t *len);

#endif
