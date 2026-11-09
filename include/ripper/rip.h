#ifndef RIPPER_RIP_H_
#define RIPPER_RIP_H_

#include <stddef.h>
#include <stdint.h>

#include "ripper/chd.h"
#include "ripper/disc.h"
#include "ripper/drive.h"
#include "ripper/hash.h"
#include "ripper/keydb.h"
#include "ripper/reader.h"

// A recipe turns one kind of disc into one output format and states which
// drive capabilities it needs. What the user can do with a disc is the list of
// recipes for its kind, filtered by the drive that holds it.

enum rip_recipe_status {
  RIP_RECIPE_READY,
  RIP_RECIPE_PLANNED  // listed so the UI can show it, not implemented yet
};

enum rip_recipe_id {
  RIP_RECIPE_ISO,      // 2048-byte sectors straight to an .iso
  RIP_RECIPE_PS3_ISO,  // .iso, decrypted when a key is known
  RIP_RECIPE_CHD,
  RIP_RECIPE_BINCUE,
  RIP_RECIPE_FOLDER,       // extracted game folder
  RIP_RECIPE_SYSTEM_DUMP,  // PS4/PS5: the console decrypts, a dumper copies
  RIP_RECIPE_AUDIO,
  RIP_RECIPE_MKV,  // a DVD title: every stream as it is (FFmpeg)
  RIP_RECIPE_MP4,  // a DVD title: H.264 + AAC (FFmpeg, x264)
  // An audio CD in another codec than FLAC (RIP_RECIPE_AUDIO): audio_out.h.
  RIP_RECIPE_MP3,
  RIP_RECIPE_AAC,
  RIP_RECIPE_ALAC,
  RIP_RECIPE_OPUS,
  RIP_RECIPE_WAV
};

// Whether the recipe rips an audio CD to files, and in which codec (audio_out.h's).
int rip_recipe_is_audio(enum rip_recipe_id id);
int rip_recipe_audio_codec(enum rip_recipe_id id);

struct rip_recipe {
  enum rip_disc_kind kind;
  enum rip_recipe_id id;
  const char *output;  // what the user gets, e.g. "Decrypted ISO (RPCS3)"
  uint32_t needs;      // RIP_CAP_* bits
  enum rip_recipe_status status;
};

enum rip_availability {
  RIP_AVAIL_OK,
  RIP_AVAIL_NEEDS_DRIVE,  // a different drive could do it
  RIP_AVAIL_PLANNED,
  RIP_AVAIL_BLOCKED  // the disc has a protection not handled yet
};

struct rip_choice {
  const struct rip_recipe *recipe;
  enum rip_availability avail;
  const char *reason;
};

// drive may be NULL (image files), which counts as every capability.
size_t rip_plan(const struct rip_drive *drive, const struct rip_disc *disc, struct rip_choice *out, size_t max);
// The first available recipe, or NULL.
const struct rip_recipe *rip_default_recipe(const struct rip_drive *drive, const struct rip_disc *disc);
const char *rip_availability_name(enum rip_availability avail);

// What an audio CD is, from MusicBrainz: tags and file names. Empty fields
// fall back to "Track 01" and no tag.
struct rip_audio_meta {
  char artist[256];
  char album[256];
  char date[16];
  char release_id[40];   // MusicBrainz's, for the cover (Cover Art Archive)
  int has_cover;         // the release has a front cover there
  char titles[99][256];  // by track number - 1
};

typedef void (*rip_progress_fn)(const char *phase, uint64_t done, uint64_t total, void *user);

struct rip_job_opts {
  const char *out_dir;
  const char *name;                  // file name stem; NULL or "" picks one from the disc
  enum chd_codec chd_codec;          // CHD recipes
  const char *css_cache_dir;         // DVD-Video: where libdvdcss keeps title keys, may be NULL
  const struct rip_keydb *ps3_keys;  // may be NULL
  int ps3_keep_encrypted;            // also keep the encrypted image
  int cd_subchannel;                 // BIN/CUE: also a .sub of the whole disc (CloneCD layout)
  int audio_quality;                 // audio CDs: FLAC 0-8, MP3 VBR 0-9 or kbit/s, AAC/Opus kbit/s (audio_out.h)
  const uint8_t *cover;              // audio CDs: the front cover, embedded and saved as cover.jpg; may be NULL
  size_t cover_len;
  int audio_image;                     // audio CDs: one file and a CUE sheet, not a file per track
  const struct rip_audio_meta *audio;  // may be NULL
  int video_crf;                       // MP4: x264 quality (0: 20)
  const char *video_preset;            // MP4: x264 preset (NULL: veryfast)
  unsigned read_retries;
  rip_progress_fn progress;
  void *user;
  volatile int *cancel;  // set non-zero from elsewhere to stop
};

struct rip_job_result {
  char image_path[1024];
  char encrypted_path[1024];  // PS3 with ps3_keep_encrypted
  char manifest_path[1024];
  char name[256];            // Redump name when the image matched, else the stem
  struct rip_hashes hashes;  // of the image exactly as read from the disc
  int ps3_key_found;
  int decrypted;
  uint64_t unreadable;  // sectors that couldn't be read, written as zeros (CDs)
};

int rip_run(const struct rip_recipe *recipe, struct rip_drive *drive, struct rip_reader *reader,
            const struct rip_disc *disc, const struct rip_job_opts *opts, struct rip_job_result *result);

#endif
