#include "catalog.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ripper/chd.h"
#include "svc.h"

/* Disc types */

static const struct cat_system SYSTEMS[] = {
    {"ps1", "PlayStation", "games", {RIP_DISC_PS1}, "chd", "psx", "{system}"},
    {"ps2", "PlayStation 2", "games", {RIP_DISC_PS2_DVD, RIP_DISC_PS2_CD}, "iso", "ps2", "{system}"},
    {"ps3", "PlayStation 3", "games", {RIP_DISC_PS3_BD, RIP_DISC_PS3_DVD}, "ps3_iso", "ps3", "{system}"},
    {"ps4", "PlayStation 4", "games", {RIP_DISC_PS4_BD}, "dump", "ps4--1", "{system}"},
    {"ps5", "PlayStation 5", "games", {RIP_DISC_PS5_BD}, "dump", "ps5", "{system}"},
    {"saturn", "Sega Saturn", "games", {RIP_DISC_SATURN}, "chd", "saturn", "{system}"},
    {"segacd", "Sega CD", "games", {RIP_DISC_SEGA_CD}, "chd", "segacd", "{system}"},
    {"pcecd", "PC Engine CD", "games", {RIP_DISC_PCE_CD}, "chd", "turbografx-cd", "{system}"},
    {"pcfx", "PC-FX", "games", {RIP_DISC_PCFX}, "chd", "pc-fx", "{system}"},
    {"neogeocd", "Neo Geo CD", "games", {RIP_DISC_NEOGEO_CD}, "chd", "neo-geo-cd", "{system}"},
    {"3do", "3DO", "games", {RIP_DISC_3DO}, "chd", "3do", "{system}"},
    {"cdi", "Philips CD-i", "games", {RIP_DISC_CDI}, "chd", "philips-cd-i", "{system}"},
    {"xbox", "Xbox", "games", {RIP_DISC_XBOX}, "xiso", "xbox", "{system}"},
    {"xbox360", "Xbox 360", "games", {RIP_DISC_XBOX_360}, "iso", "xbox360", "{system}"},
    {"xboxone", "Xbox One / Series", "games", {RIP_DISC_XBOX_ONE}, "iso", "xboxone", "{system}"},
    {"gamecube", "GameCube", "games", {RIP_DISC_GAMECUBE}, "iso", "ngc", "{system}"},
    {"wii", "Wii", "games", {RIP_DISC_WII}, "wbfs", "wii", "{system}"},
    {"wiiu", "Wii U", "games", {RIP_DISC_WII_U}, "iso", "wiiu", "{system}"},
    {"dreamcast", "Dreamcast", "games", {RIP_DISC_DREAMCAST}, "bincue", "dc", "{system}"},
    {"dvd_video", "DVD-Video", "movies", {RIP_DISC_DVD_VIDEO}, "mkv", "", "{name}"},
    {"bd_video", "Blu-ray", "movies", {RIP_DISC_BD_VIDEO}, "iso", "", "{name}"},
    {"vcd", "Video CD", "movies", {RIP_DISC_VIDEO_CD}, "bincue", "", "{name}"},
    {"music", "Audio CD", "music", {RIP_DISC_CD_AUDIO}, "flac", "", "{artist}"},
    {"data", "Data Disc", "other", {RIP_DISC_CD_DATA, RIP_DISC_DVD_DATA, RIP_DISC_BD_DATA}, "iso", "", "{name}"},
};

const struct cat_system *catalog_system_for(enum rip_disc_kind kind) {
  size_t i;
  size_t k;

  for (i = 0; i < sizeof(SYSTEMS) / sizeof(SYSTEMS[0]); i++) {
    for (k = 0; k < 4 && SYSTEMS[i].kinds[k]; k++) {
      if (SYSTEMS[i].kinds[k] == kind) {
        return &SYSTEMS[i];
      }
    }
  }
  return NULL;
}

const struct cat_system *catalog_system(const char *id) {
  size_t i;

  for (i = 0; id && i < sizeof(SYSTEMS) / sizeof(SYSTEMS[0]); i++) {
    if (!strcmp(SYSTEMS[i].id, id)) {
      return &SYSTEMS[i];
    }
  }
  return NULL;
}

/* Formats and their options */

struct choice {
  const char *id;
  const char *label;
  const char *only;  // a disc type this choice is for, NULL for all
};

struct option {
  const char *id;
  const char *label;
  const char *type;  // "choice", "toggle"
  const char *def;   // choice id, or "1"/"0"
  const struct choice *choices;
  const char *help;
};

struct format {
  const char *id;
  enum rip_recipe_id recipe;
  const char *label;
  const char *help;
  const struct option *options;
};

static const struct choice CHD_CODECS[] = {{"zlib", "Deflate", NULL}, {"zstd", "Zstandard", NULL}, {NULL, NULL, NULL}};
static const struct choice FLAC_LEVELS[] = {
    {"5", "Normal", NULL}, {"8", "Smallest", NULL}, {"0", "Fastest", NULL}, {NULL, NULL, NULL}};
static const struct choice AUDIO_LAYOUTS[] = {
    {"tracks", "One File per Track", NULL}, {"image", "One File and a CUE Sheet", NULL}, {NULL, NULL, NULL}};
// Dumps are always decrypted and fake-signed: emulators take fake-signed
// executables too. A PS4 game may be meant for a PS4, which wants its own kind.
static const struct choice DUMP_TARGETS[] = {{"ps5", "A PS5", NULL}, {"ps4", "A PS4", "ps4"}, {NULL, NULL, NULL}};
// Each lowers the game to that firmware's SDK, so it also runs on anything
// newer: the labels say which firmware each one is the pick for.
static const struct choice DUMP_BACKPORTS[] = {{"off", "Off", NULL},
                                               {"ps4-5.05", "PS4 5.05 to 5.56", "ps4"},
                                               {"ps4-6.00", "PS4 6.xx", "ps4"},
                                               {"ps4-7.00", "PS4 7.xx and 8.xx", "ps4"},
                                               {"ps4-9.00", "PS4 9.xx", "ps4"},
                                               {"ps4-10.00", "PS4 10.xx", "ps4"},
                                               {"ps4-11.00", "PS4 11.xx and newer", "ps4"},
                                               {"ps5-1", "PS5 1.xx", "ps5"},
                                               {"ps5-2", "PS5 2.xx", "ps5"},
                                               {"ps5-3", "PS5 3.xx", "ps5"},
                                               {"ps5-4", "PS5 4.xx", "ps5"},
                                               {"ps5-5", "PS5 5.xx", "ps5"},
                                               {"ps5-6", "PS5 6.xx", "ps5"},
                                               {"ps5-7", "PS5 7.xx", "ps5"},
                                               {"ps5-8", "PS5 8.xx", "ps5"},
                                               {"ps5-9", "PS5 9.xx", "ps5"},
                                               {"ps5-10", "PS5 10.xx and newer", "ps5"},
                                               {NULL, NULL, NULL}};

static const struct option NO_OPTIONS[] = {{NULL, NULL, NULL, NULL, NULL, NULL}};
static const struct choice VIDEO_QUALITY[] = {
    {"20", "Good", NULL}, {"18", "Best", NULL}, {"23", "Smaller", NULL}, {NULL, NULL, NULL}};
static const struct choice VIDEO_SPEED[] = {
    {"veryfast", "Fast", NULL}, {"medium", "Slower", NULL}, {"ultrafast", "Fastest", NULL}, {NULL, NULL, NULL}};
static const struct option MP4_OPTIONS[] = {{"video_quality", "Quality", "choice", "20", VIDEO_QUALITY, NULL},
                                            {"video_speed", "Encoding Speed", "choice", "veryfast", VIDEO_SPEED, NULL},
                                            {NULL, NULL, NULL, NULL, NULL, NULL}};
static const struct option CHD_OPTIONS[] = {{"chd_codec", "Compression", "choice", "zlib", CHD_CODECS, NULL},
                                            {NULL, NULL, NULL, NULL, NULL, NULL}};
static const struct option PS3_OPTIONS[] = {
    {"keep_encrypted", "Also Keep the Encrypted Image", "toggle", "0", NULL, NULL},
    {"split", "Split for FAT32 Drives", "toggle", "0", NULL, NULL},
    {NULL, NULL, NULL, NULL, NULL, NULL}};
static const struct option WBFS_OPTIONS[] = {{"split", "Split for FAT32 Drives", "toggle", "1", NULL, NULL},
                                             {NULL, NULL, NULL, NULL, NULL, NULL}};
static const struct option BINCUE_OPTIONS[] = {{"subchannel", "Keep Subchannel Data", "toggle", "0", NULL, NULL},
                                               {NULL, NULL, NULL, NULL, NULL, NULL}};
static const struct option FLAC_OPTIONS[] = {{"flac_level", "Compression", "choice", "5", FLAC_LEVELS, NULL},
                                             {"layout", "Tracks As", "choice", "tracks", AUDIO_LAYOUTS, NULL},
                                             {"lookup", "Look Up the Album", "toggle", "1", NULL, NULL},
                                             {NULL, NULL, NULL, NULL, NULL, NULL}};
static const struct choice MP3_QUALITY[] = {{"2", "High (VBR)", NULL},
                                            {"0", "Best (VBR)", NULL},
                                            {"320", "320 kbit/s", NULL},
                                            {"5", "Smaller (VBR)", NULL},
                                            {NULL, NULL, NULL}};
static const struct choice AAC_RATES[] = {{"256", "256 kbit/s", NULL},
                                          {"320", "320 kbit/s", NULL},
                                          {"192", "192 kbit/s", NULL},
                                          {"128", "128 kbit/s", NULL},
                                          {NULL, NULL, NULL}};
static const struct choice OPUS_RATES[] = {{"160", "160 kbit/s", NULL},
                                           {"256", "256 kbit/s", NULL},
                                           {"128", "128 kbit/s", NULL},
                                           {"96", "96 kbit/s", NULL},
                                           {NULL, NULL, NULL}};
static const struct option MP3_OPTIONS[] = {{"mp3_quality", "Quality", "choice", "2", MP3_QUALITY, NULL},
                                            {"layout", "Tracks As", "choice", "tracks", AUDIO_LAYOUTS, NULL},
                                            {"lookup", "Look Up the Album", "toggle", "1", NULL, NULL},
                                            {NULL, NULL, NULL, NULL, NULL, NULL}};
static const struct option AAC_OPTIONS[] = {{"aac_bitrate", "Bit Rate", "choice", "256", AAC_RATES, NULL},
                                            {"layout", "Tracks As", "choice", "tracks", AUDIO_LAYOUTS, NULL},
                                            {"lookup", "Look Up the Album", "toggle", "1", NULL, NULL},
                                            {NULL, NULL, NULL, NULL, NULL, NULL}};
static const struct option OPUS_OPTIONS[] = {{"opus_bitrate", "Bit Rate", "choice", "160", OPUS_RATES, NULL},
                                             {"layout", "Tracks As", "choice", "tracks", AUDIO_LAYOUTS, NULL},
                                             {"lookup", "Look Up the Album", "toggle", "1", NULL, NULL},
                                             {NULL, NULL, NULL, NULL, NULL, NULL}};
static const struct option LOSSLESS_OPTIONS[] = {{"layout", "Tracks As", "choice", "tracks", AUDIO_LAYOUTS, NULL},
                                                 {"lookup", "Look Up the Album", "toggle", "1", NULL, NULL},
                                                 {NULL, NULL, NULL, NULL, NULL, NULL}};

static const struct choice DUMP_CONTAINERS[] = {{"folder", "A Folder", NULL},
                                                {"ffpkg", "UFS2 Image (.ffpkg)", NULL},
                                                {"exfat", "exFAT Image (.exfat)", NULL},
                                                {"pkg", "Fake PKG (.pkg)", "ps4"},
                                                {NULL, NULL, NULL}};

static const struct option DUMP_OPTIONS[] = {{"complete", "Complete Dump", "toggle", "0", NULL, NULL},
                                             {"run_on", "Runs On", "choice", "ps5", DUMP_TARGETS, NULL},
                                             {"backport", "Backport To", "choice", "off", DUMP_BACKPORTS, NULL},
                                             {"container", "Package As", "choice", "folder", DUMP_CONTAINERS, NULL},
                                             {NULL, NULL, NULL, NULL, NULL, NULL}};

static const struct format FORMATS[] = {
    {"iso", RIP_RECIPE_ISO, "ISO Image", NULL, NO_OPTIONS},
    {"chd", RIP_RECIPE_CHD, "CHD", NULL, CHD_OPTIONS},
    {"ps3_iso", RIP_RECIPE_PS3_ISO, "Decrypted ISO", "Needs the disc's key in the key file.", PS3_OPTIONS},
    {"folder", RIP_RECIPE_FOLDER, "Game Folder", NULL, NO_OPTIONS},
    {"bincue", RIP_RECIPE_BINCUE, "BIN/CUE", NULL, BINCUE_OPTIONS},
    {"dump", RIP_RECIPE_SYSTEM_DUMP, "Game Dump", "Install the game first.", DUMP_OPTIONS},
    {"flac", RIP_RECIPE_AUDIO, "FLAC", NULL, FLAC_OPTIONS},
    {"mp3", RIP_RECIPE_MP3, "MP3", NULL, MP3_OPTIONS},
    {"aac", RIP_RECIPE_AAC, "AAC", "In .m4a", AAC_OPTIONS},
    {"alac", RIP_RECIPE_ALAC, "ALAC", "Lossless, in .m4a", LOSSLESS_OPTIONS},
    {"opus", RIP_RECIPE_OPUS, "Opus", NULL, OPUS_OPTIONS},
    {"wav", RIP_RECIPE_WAV, "WAV", NULL, LOSSLESS_OPTIONS},
    {"mkv", RIP_RECIPE_MKV, "MKV", NULL, NO_OPTIONS},
    {"mp4", RIP_RECIPE_MP4, "MP4", "H.264 and AAC. Re-encoded, so it's slower.", MP4_OPTIONS},
    {"xiso", RIP_RECIPE_XISO, "XISO", NULL, NO_OPTIONS},
    {"ciso", RIP_RECIPE_CISO, "CISO", NULL, NO_OPTIONS},
    {"wbfs", RIP_RECIPE_WBFS, "WBFS", NULL, WBFS_OPTIONS},
    {"rvz", RIP_RECIPE_RVZ, "RVZ", NULL, NO_OPTIONS},
};

static const struct format *find_format(const char *id) {
  size_t i;

  for (i = 0; id && i < sizeof(FORMATS) / sizeof(FORMATS[0]); i++) {
    if (!strcmp(FORMATS[i].id, id)) {
      return &FORMATS[i];
    }
  }
  return NULL;
}

const char *catalog_format_id(enum rip_recipe_id recipe) {
  size_t i;

  for (i = 0; i < sizeof(FORMATS) / sizeof(FORMATS[0]); i++) {
    if (FORMATS[i].recipe == recipe) {
      return FORMATS[i].id;
    }
  }
  return "";
}

int catalog_recipe_id(const char *format, enum rip_recipe_id *out) {
  const struct format *f = find_format(format);

  if (!f) {
    return -1;
  }
  *out = f->recipe;
  return 0;
}

// The option's value from options, checked against its choices, else its default.
static const char *option_value(const struct option *o, const cJSON *options, char *buf, size_t n) {
  const cJSON *v = cJSON_GetObjectItemCaseSensitive(options, o->id);

  if (!strcmp(o->type, "toggle")) {
    if (cJSON_IsBool(v) || cJSON_IsNumber(v)) {
      snprintf(buf, n, "%d", cJSON_IsTrue(v) || (cJSON_IsNumber(v) && v->valueint));
      return buf;
    }
    return o->def;
  }
  if (cJSON_IsString(v)) {
    const struct choice *c;
    for (c = o->choices; c && c->id; c++) {
      if (!strcmp(c->id, v->valuestring)) {
        return c->id;
      }
    }
  }
  return o->def;
}

static cJSON *options_json(const struct format *f, const cJSON *given) {
  cJSON *out = cJSON_CreateObject();
  const struct option *o;

  for (o = f->options; o->id; o++) {
    char buf[8];
    const char *v = option_value(o, given, buf, sizeof(buf));
    if (!strcmp(o->type, "toggle")) {
      cJSON_AddBoolToObject(out, o->id, v[0] == '1');
    } else {
      cJSON_AddStringToObject(out, o->id, v);
    }
  }
  return out;
}

cJSON *catalog_format_defaults(const char *format) {
  const struct format *f = find_format(format);

  return f ? options_json(f, NULL) : cJSON_CreateObject();
}

cJSON *catalog_default_profile(const struct cat_system *s) {
  cJSON *p = cJSON_CreateObject();
  const struct format *f = find_format(s->default_format);

  cJSON_AddStringToObject(p, "format", s->default_format);
  cJSON_AddStringToObject(p, "destination", "console");
  cJSON_AddStringToObject(p, "folder", s->folder);
  if (s->romm_slug[0]) {
    cJSON_AddStringToObject(p, "romm_platform", s->romm_slug);
  }
  cJSON_AddItemToObject(p, "options", f ? options_json(f, NULL) : cJSON_CreateObject());
  return p;
}

void catalog_job_options(const char *format, const cJSON *options, struct rip_job_opts *opts) {
  const struct format *f = find_format(format);
  const struct option *o;

  if (!f) {
    return;
  }
  for (o = f->options; o->id; o++) {
    char buf[8];
    const char *v = option_value(o, options, buf, sizeof(buf));
    if (!strcmp(o->id, "chd_codec")) {
      opts->chd_codec = !strcmp(v, "zstd") ? CHD_ZSTD : CHD_ZLIB;
    } else if (!strcmp(o->id, "keep_encrypted")) {
      opts->ps3_keep_encrypted = v[0] == '1';
    } else if (!strcmp(o->id, "split")) {
      opts->split_fat32 = v[0] == '1';
    } else if (!strcmp(o->id, "subchannel")) {
      opts->cd_subchannel = v[0] == '1';
    } else if (!strcmp(o->id, "flac_level") || !strcmp(o->id, "mp3_quality") || !strcmp(o->id, "aac_bitrate") ||
               !strcmp(o->id, "opus_bitrate")) {
      opts->audio_quality = atoi(v);
    } else if (!strcmp(o->id, "layout")) {
      opts->audio_image = !strcmp(v, "image");
    } else if (!strcmp(o->id, "video_quality")) {
      opts->video_crf = atoi(v);
    } else if (!strcmp(o->id, "video_speed")) {
      opts->video_preset = !strcmp(v, "medium") ? "medium" : !strcmp(v, "ultrafast") ? "ultrafast" : "veryfast";
    }
  }
}

/* Destination types */

struct field {
  const char *id;
  const char *label;
  const char *type;  // "text", "number", "secret", "toggle", "folder"
  const char *placeholder;
  const char *def;
};

struct dest_type {
  const char *type;
  const char *label;
  const char *help;
  int available;
  const struct field fields[8];
};

#ifdef RIPPER_HAVE_SMB2
#define HAVE_SMB2 1
#else
#define HAVE_SMB2 0
#endif
#ifdef RIPPER_HAVE_NFS
#define HAVE_NFS 1
#else
#define HAVE_NFS 0
#endif

static const struct dest_type DEST_TYPES[] = {
    {"local", "Local Location", "A folder on the console.", 1, {{"path", "Folder", "folder", "/data/ripper/rips", ""}}},
    {"usb",
     "USB Drive",
     "A folder on a USB drive. Plug it in before you pick the folder.",
     1,
     {{"path", "Folder", "folder", "/mnt/usb0/rips", ""}}},
    {"romm",
     "RomM",
     "RomM 5.4 or newer.",
     1,
     {{"url", "Server", "text", "https://romm.example.com", ""},
      {"token", "API Token", "secret", "Pair, or paste a client API token", ""},
      {"tls_verify", "Check the Server's Certificate", "toggle", NULL, "1"},
      {"scan", "Scan the Platform After an Upload", "toggle", NULL, "1"}}},
    {"smb",
     "SMB Share",
     "A Windows, Samba or NAS share.",
     HAVE_SMB2,
     {{"host", "Server", "text", "192.168.1.10 or nas.local", ""},
      {"share", "Share", "text", "media", ""},
      {"path", "Folder in the Share", "text", "Movies", ""},
      {"user", "User", "text", "", ""},
      {"password", "Password", "secret", "", ""},
      {"domain", "Domain", "text", "WORKGROUP (usually blank)", ""}}},
    {"nfs",
     "NFS Export",
     "An NFS export. It must allow the console's address.",
     HAVE_NFS,
     {{"host", "Server", "text", "192.168.1.10", ""},
      {"export", "Export", "text", "/volume1/media", ""},
      {"path", "Folder in the Export", "text", "Music", ""},
      {"uid", "Write as User ID", "number", "1000", "0"},
      {"gid", "Write as Group ID", "number", "1000", "0"}}},
    {"ftp",
     "FTP Server",
     "Plain FTP, or FTP over TLS.",
     1,
     {{"host", "Server", "text", "192.168.1.10", ""},
      {"port", "Port", "number", "21", "21"},
      {"path", "Folder", "text", "/rips", ""},
      {"user", "User", "text", "anonymous", ""},
      {"password", "Password", "secret", "", ""},
      {"tls", "Use TLS (FTPS)", "toggle", NULL, "0"}}},
};

static const struct dest_type *find_dest_type(const char *type) {
  size_t i;

  for (i = 0; type && i < sizeof(DEST_TYPES) / sizeof(DEST_TYPES[0]); i++) {
    if (!strcmp(DEST_TYPES[i].type, type)) {
      return &DEST_TYPES[i];
    }
  }
  return NULL;
}

int catalog_dest_type_known(const char *type) {
  return find_dest_type(type) != NULL;
}

int catalog_is_secret(const char *dest_type, const char *field) {
  const struct dest_type *t = find_dest_type(dest_type);
  size_t i;

  for (i = 0; t && i < 8 && t->fields[i].id; i++) {
    if (!strcmp(t->fields[i].id, field)) {
      return !strcmp(t->fields[i].type, "secret");
    }
  }
  return 0;
}

/* JSON */

// A format as a system's discs see it: only the choices meant for them, and
// no option left with nothing to choose.
static cJSON *format_json(const struct format *f, const char *system) {
  cJSON *j = cJSON_CreateObject();
  cJSON *opts = cJSON_AddArrayToObject(j, "options");
  const struct option *o;

  cJSON_AddStringToObject(j, "id", f->id);
  cJSON_AddStringToObject(j, "label", f->label);
  cJSON_AddStringToObject(j, "help", f->help ? f->help : "");
  for (o = f->options; o->id; o++) {
    cJSON *oj;
    if (strcmp(o->type, "toggle") != 0) {
      const struct choice *c;
      int usable = 0;
      for (c = o->choices; c && c->id; c++) {
        usable += !c->only || !strcmp(c->only, system);
      }
      if (usable < 2) {
        continue;
      }
    }
    oj = cJSON_CreateObject();
    cJSON_AddStringToObject(oj, "id", o->id);
    cJSON_AddStringToObject(oj, "label", o->label);
    cJSON_AddStringToObject(oj, "type", o->type);
    if (!strcmp(o->type, "toggle")) {
      cJSON_AddBoolToObject(oj, "default", o->def[0] == '1');
    } else {
      const struct choice *c;
      cJSON *cs = cJSON_AddArrayToObject(oj, "choices");
      cJSON_AddStringToObject(oj, "default", o->def);
      for (c = o->choices; c && c->id; c++) {
        cJSON *cj;
        if (c->only && strcmp(c->only, system) != 0) {
          continue;
        }
        cj = cJSON_CreateObject();
        cJSON_AddStringToObject(cj, "id", c->id);
        cJSON_AddStringToObject(cj, "label", c->label);
        if (c->only) {
          cJSON_AddStringToObject(cj, "only", c->only);
        }
        cJSON_AddItemToArray(cs, cj);
      }
    }
    if (o->help) {
      cJSON_AddStringToObject(oj, "help", o->help);
    }
    cJSON_AddItemToArray(opts, oj);
  }
  return j;
}

// The formats a disc type can go to: every recipe of its kinds, best first.
static cJSON *system_formats(const struct cat_system *s) {
  cJSON *arr = cJSON_CreateArray();
  size_t i;

  for (i = 0; i < sizeof(FORMATS) / sizeof(FORMATS[0]); i++) {
    struct rip_disc disc;
    struct rip_choice choices[8];
    size_t k;
    size_t n;
    int found = 0;
    int planned = 1;

    for (k = 0; k < 4 && s->kinds[k]; k++) {
      size_t c;
      memset(&disc, 0, sizeof(disc));
      disc.kind = s->kinds[k];
      n = rip_plan(NULL, &disc, choices, 8);
      for (c = 0; c < n; c++) {
        if (choices[c].recipe->id == FORMATS[i].recipe) {
          found = 1;
          if (choices[c].recipe->status == RIP_RECIPE_READY) {
            planned = 0;
          }
        }
      }
    }
    if (found) {
      cJSON *f = format_json(&FORMATS[i], s->id);
      cJSON_AddBoolToObject(f, "planned", planned);
      if (!strcmp(FORMATS[i].id, s->default_format)) {
        cJSON_InsertItemInArray(arr, 0, f);
      } else {
        cJSON_AddItemToArray(arr, f);
      }
    }
  }
  return arr;
}

cJSON *catalog_json(void) {
  cJSON *j = cJSON_CreateObject();
  cJSON *systems = cJSON_AddArrayToObject(j, "systems");
  cJSON *types = cJSON_AddArrayToObject(j, "destination_types");
  size_t i;
  size_t k;

  for (i = 0; i < sizeof(SYSTEMS) / sizeof(SYSTEMS[0]); i++) {
    const struct cat_system *s = &SYSTEMS[i];
    cJSON *sj = cJSON_CreateObject();
    cJSON_AddStringToObject(sj, "id", s->id);
    cJSON_AddStringToObject(sj, "name", s->name);
    cJSON_AddStringToObject(sj, "group", s->group);
    cJSON_AddStringToObject(sj, "romm_slug", s->romm_slug);
    cJSON_AddItemToObject(sj, "formats", system_formats(s));
    cJSON_AddItemToObject(sj, "defaults", catalog_default_profile(s));
    cJSON_AddItemToArray(systems, sj);
  }
  for (i = 0; i < sizeof(DEST_TYPES) / sizeof(DEST_TYPES[0]); i++) {
    const struct dest_type *t = &DEST_TYPES[i];
    cJSON *tj = cJSON_CreateObject();
    cJSON *fields = cJSON_AddArrayToObject(tj, "fields");
    cJSON_AddStringToObject(tj, "type", t->type);
    cJSON_AddStringToObject(tj, "label", t->label);
    cJSON_AddStringToObject(tj, "help", t->help);
    cJSON_AddBoolToObject(tj, "available", t->available);
    for (k = 0; k < 8 && t->fields[k].id; k++) {
      cJSON *fj = cJSON_CreateObject();
      cJSON_AddStringToObject(fj, "id", t->fields[k].id);
      cJSON_AddStringToObject(fj, "label", t->fields[k].label);
      cJSON_AddStringToObject(fj, "type", t->fields[k].type);
      if (t->fields[k].placeholder) {
        cJSON_AddStringToObject(fj, "placeholder", t->fields[k].placeholder);
      }
      cJSON_AddStringToObject(fj, "default", t->fields[k].def ? t->fields[k].def : "");
      cJSON_AddItemToArray(fields, fj);
    }
    cJSON_AddItemToArray(types, tj);
  }
  return j;
}

static const struct choice *find_choice(const struct choice *list, const char *id) {
  for (; list && list->id; list++) {
    if (id && !strcmp(list->id, id)) {
      return list;
    }
  }
  return NULL;
}

const char *catalog_game_container(const cJSON *options) {
  const struct choice *c = find_choice(DUMP_CONTAINERS, jstr(options, "container", "folder"));

  return c ? c->id : "folder";
}

void catalog_game_finish(const char *system, const cJSON *options, struct game_finish *out) {
  const struct choice *back = find_choice(DUMP_BACKPORTS, jstr(options, "backport", "off"));

  memset(out, 0, sizeof(*out));
  out->fself = 1;
  out->target =
      !strcmp(system, "ps4") && !strcmp(jstr(options, "run_on", "ps5"), "ps4") ? SCE_TARGET_PS4 : SCE_TARGET_PS5;
  if (back && back->only && !strcmp(back->only, system)) {
    sce_backport_target(back->id, &out->backport_ps4, &out->backport_ps5);
  }
}
