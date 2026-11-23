#ifndef RIPPER_LANGS_H_
#define RIPPER_LANGS_H_

// Languages, for PS5 games installed from disc: a disc carries every
// region's languages, but the console installs only those its copy
// supports (playgo-scenario.json's chunkSupportedLanguages). The others'
// files are listed in the game's mount with no data behind them, and
// reading one never returns. A dump leaves their folders out.

#include <stddef.h>

#define LANGS_MAX 32

struct langs {
  const char *keys[LANGS_MAX];  // "en", "es-419", "pt-BR"...
  int n;
  const char *main;  // the copy's own language (chunkDefaultLanguage), else the first
};

// The languages in playgo-scenario.json's chunkSupportedLanguages. RIP_OK
// with at least one, RIP_ENOENT when the text has none.
int langs_from_scenario(const char *json, struct langs *out);

// The language a folder is named for ("eLID_German", "lang_ptBR", "japanese"),
// as a key; NULL when the name isn't a language's.
const char *langs_folder(const char *name);

int langs_has(const struct langs *l, const char *key);

// The system languages l covers, as drakmor's pgo_stub reads them in
// playgo_stub.dat ("english_us,english_gb,spanish_la"), into out. How many
// languages went in: 0 when none of them has a name there.
int langs_playgo(const struct langs *l, char *out, size_t n);

// "German", "Latin American Spanish": a key's name, for people.
const char *langs_name(const char *key);

#endif
