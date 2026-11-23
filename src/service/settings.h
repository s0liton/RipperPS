#ifndef RIPPER_SETTINGS_H_
#define RIPPER_SETTINGS_H_

// config.json in the data folder:
//
//   {
//     "web_port": 8790, "notify": true, "auto_start": false, "auto_eject": true,
//     "read_retries": 3, "keys": {"ps3": "/data/ripper/keys/ps3.keys"}, "ui_theme": "acrylic",
//     "destinations": [
//       {"id": "console", "type": "local", "name": "Local Location", "path": "/data/ripper/rips"},
//       {"id": "usb", "type": "usb", "name": "USB drive", "path": "/mnt/usb0/rips"},
//       {"id": "romm", "type": "romm", "name": "RomM", "url": "https://...", "token": "rmm_...", ...},
//       {"id": "nas", "type": "smb", "name": "NAS", "host": "...", "share": "media", "path": "Movies", ...}
//     ],
//     "profiles": {
//       "ps2": {"format": "chd", "destination": "romm", "folder": "{system}", "options": {"chd_codec": "zlib"}},
//       ...
//     }
//   }
//
// Destinations' secrets (passwords, tokens) never leave the console: the API
// shows whether one is set, and an update that leaves it out keeps it.

#include "cJSON.h"

void settings_load(void);
int settings_save(void);

// Copies; the caller frees them.
cJSON *settings_json(int with_secrets);
cJSON *settings_destination(const char *id);  // NULL if there is none
// A system's profile with the catalog's defaults filled in for whatever it leaves out.
cJSON *settings_profile(const char *system);

// Applies what the UI sends: general settings, destinations, profiles. Any
// part may be missing. RIP_OK, or RIP_ERR with err filled.
int settings_apply(const cJSON *j, char *err, size_t en);
// Stores a destination's token (RomM pairing).
int settings_set_secret(const char *dest_id, const char *field, const char *value);

int settings_web_port(void);
int settings_notify(void);
int settings_auto_start(void);
int settings_auto_eject(void);
unsigned settings_read_retries(void);
// The key file for a kind of disc ("ps3"): the one picked, else the default.
void settings_key_file(const char *id, char *out, size_t n);
void settings_ui_theme(char *out, size_t n);
// The Movie Database API key for movie names and posters; "" when there is none.
void settings_tmdb_key(char *out, size_t n);
// Where rips wait before going to a network destination.
void settings_staging_dir(char *out, size_t n);

#endif
