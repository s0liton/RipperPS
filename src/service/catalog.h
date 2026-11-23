#ifndef RIPPER_CATALOG_H_
#define RIPPER_CATALOG_H_

// What can be ripped and how, as data the UIs draw from: the disc types
// (each a profile in the settings), the formats each can go to with their
// options, and the destination types with their fields. The web UI and the
// PS5 app build their forms from catalog_json(), so a format added here shows
// up in both.

#include "cJSON.h"
#include "ripper/disc.h"
#include "ripper/game.h"
#include "ripper/rip.h"

struct cat_system {
  const char *id;     // profile key: "ps2"
  const char *name;   // "PlayStation 2"
  const char *group;  // "games", "movies", "music", "other"
  enum rip_disc_kind kinds[4];
  const char *default_format;
  const char *romm_slug;  // RomM's platform slug, "" when RomM is not for it
  const char *folder;     // default folder template under a destination
};

const struct cat_system *catalog_system_for(enum rip_disc_kind kind);
const struct cat_system *catalog_system(const char *id);

// Format ids ("iso", "chd", "ps3_iso", ...) and the recipes behind them.
const char *catalog_format_id(enum rip_recipe_id recipe);
int catalog_recipe_id(const char *format, enum rip_recipe_id *out);

// A profile with every default filled in.
cJSON *catalog_default_profile(const struct cat_system *s);
// A format's options as they are by default (an empty object for an unknown format).
cJSON *catalog_format_defaults(const char *format);
// Format options from a profile or a rip request onto the job's options:
// unknown keys and values are dropped, missing ones get defaults.
void catalog_job_options(const char *format, const cJSON *options, struct rip_job_opts *opts);

// A game dump's options (run_on, backport) as what readies its folder;
// choices meant for the other console are ignored.
void catalog_game_finish(const char *system, const cJSON *options, struct game_finish *out);
// "folder", "ffpkg" or "exfat".
const char *catalog_game_container(const cJSON *options);

// Fields of a destination type that hold secrets.
int catalog_is_secret(const char *dest_type, const char *field);
int catalog_dest_type_known(const char *type);

cJSON *catalog_json(void);

#endif
