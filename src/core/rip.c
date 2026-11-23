#include "ripper/rip.h"

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "rip_internal.h"
#include "ripper/aacs.h"
#include "ripper/audio_out.h"
#include "ripper/chd.h"
#include "ripper/ciso.h"
#include "ripper/common.h"
#include "ripper/css.h"
#include "ripper/dvd.h"
#include "ripper/iso9660.h"
#include "ripper/log.h"
#include "ripper/mmc.h"
#include "ripper/omnidrive.h"
#include "ripper/ps3.h"
#include "ripper/remux.h"
#include "ripper/rvz.h"
#include "ripper/util.h"
#include "ripper/wbfs.h"
#include "ripper/xbox.h"
#include "ripper/xiso.h"

static const struct rip_recipe RECIPES[] = {
    // Readable by the PS5's own drive.
    {RIP_DISC_PS2_DVD, RIP_RECIPE_ISO, "ISO", RIP_CAP_READ_DVD, RIP_RECIPE_READY},
    {RIP_DISC_PS2_DVD, RIP_RECIPE_CHD, "CHD", RIP_CAP_READ_DVD, RIP_RECIPE_READY},
    {RIP_DISC_PS3_BD, RIP_RECIPE_PS3_ISO, "Decrypted ISO", RIP_CAP_READ_BD, RIP_RECIPE_READY},
    {RIP_DISC_PS3_BD, RIP_RECIPE_FOLDER, "Game folder", RIP_CAP_READ_BD, RIP_RECIPE_READY},
    {RIP_DISC_PS3_DVD, RIP_RECIPE_PS3_ISO, "Decrypted ISO", RIP_CAP_READ_DVD, RIP_RECIPE_READY},
    {RIP_DISC_PS3_DVD, RIP_RECIPE_FOLDER, "Game folder", RIP_CAP_READ_DVD, RIP_RECIPE_READY},
    // PS4/PS5: the console decrypts the installed game (platform.h plat_game_dump).
    {RIP_DISC_PS4_BD, RIP_RECIPE_SYSTEM_DUMP, "Game folder", RIP_CAP_READ_BD, RIP_RECIPE_READY},
    {RIP_DISC_PS5_BD, RIP_RECIPE_SYSTEM_DUMP, "Game folder", RIP_CAP_READ_BD, RIP_RECIPE_READY},
    {RIP_DISC_DVD_DATA, RIP_RECIPE_ISO, "ISO", RIP_CAP_READ_DVD, RIP_RECIPE_READY},
    {RIP_DISC_DVD_DATA, RIP_RECIPE_CHD, "CHD", RIP_CAP_READ_DVD, RIP_RECIPE_READY},
    {RIP_DISC_DVD_VIDEO, RIP_RECIPE_MKV, "MKV", RIP_CAP_READ_DVD, RIP_RECIPE_READY},
    {RIP_DISC_DVD_VIDEO, RIP_RECIPE_MP4, "MP4", RIP_CAP_READ_DVD, RIP_RECIPE_READY},
    {RIP_DISC_DVD_VIDEO, RIP_RECIPE_ISO, "ISO", RIP_CAP_READ_DVD, RIP_RECIPE_READY},
    {RIP_DISC_BD_DATA, RIP_RECIPE_ISO, "ISO", RIP_CAP_READ_BD, RIP_RECIPE_READY},
    // Decrypted with the KEYDB.cfg's key (aacs.h), else as it is.
    {RIP_DISC_BD_VIDEO, RIP_RECIPE_ISO, "ISO", RIP_CAP_READ_BD, RIP_RECIPE_READY},
    // Need an external drive with a CD laser.
    {RIP_DISC_PS1, RIP_RECIPE_BINCUE, "BIN/CUE", RIP_CAP_READ_CD, RIP_RECIPE_READY},
    {RIP_DISC_PS1, RIP_RECIPE_CHD, "CHD", RIP_CAP_READ_CD, RIP_RECIPE_READY},
    // OPL and PCSX2 take a PS2 CD game as an ISO too.
    {RIP_DISC_PS2_CD, RIP_RECIPE_ISO, "ISO", RIP_CAP_READ_CD, RIP_RECIPE_READY},
    {RIP_DISC_PS2_CD, RIP_RECIPE_BINCUE, "BIN/CUE", RIP_CAP_READ_CD, RIP_RECIPE_READY},
    {RIP_DISC_PS2_CD, RIP_RECIPE_CHD, "CHD", RIP_CAP_READ_CD, RIP_RECIPE_READY},
    {RIP_DISC_CD_DATA, RIP_RECIPE_BINCUE, "BIN/CUE", RIP_CAP_READ_CD, RIP_RECIPE_READY},
    {RIP_DISC_CD_DATA, RIP_RECIPE_CHD, "CHD", RIP_CAP_READ_CD, RIP_RECIPE_READY},
    {RIP_DISC_CD_AUDIO, RIP_RECIPE_AUDIO, "FLAC", RIP_CAP_READ_CD, RIP_RECIPE_READY},
    {RIP_DISC_CD_AUDIO, RIP_RECIPE_MP3, "MP3", RIP_CAP_READ_CD, RIP_RECIPE_READY},
    {RIP_DISC_CD_AUDIO, RIP_RECIPE_AAC, "AAC", RIP_CAP_READ_CD, RIP_RECIPE_READY},
    {RIP_DISC_CD_AUDIO, RIP_RECIPE_ALAC, "ALAC", RIP_CAP_READ_CD, RIP_RECIPE_READY},
    {RIP_DISC_CD_AUDIO, RIP_RECIPE_OPUS, "Opus", RIP_CAP_READ_CD, RIP_RECIPE_READY},
    {RIP_DISC_CD_AUDIO, RIP_RECIPE_WAV, "WAV", RIP_CAP_READ_CD, RIP_RECIPE_READY},
    // The disc as it is, to burn a copy.
    {RIP_DISC_CD_AUDIO, RIP_RECIPE_BINCUE, "BIN/CUE", RIP_CAP_READ_CD, RIP_RECIPE_READY},
#define CD_GAME(kind, emu)                                                \
  {kind, RIP_RECIPE_CHD, "CHD", RIP_CAP_READ_CD, RIP_RECIPE_READY}, {     \
    kind, RIP_RECIPE_BINCUE, "BIN/CUE", RIP_CAP_READ_CD, RIP_RECIPE_READY \
  }
    CD_GAME(RIP_DISC_SATURN, "Beetle Saturn"),
    CD_GAME(RIP_DISC_SEGA_CD, "Genesis Plus GX"),
    CD_GAME(RIP_DISC_PCE_CD, "Beetle PCE"),
    CD_GAME(RIP_DISC_PCFX, "Beetle PC-FX"),
    CD_GAME(RIP_DISC_NEOGEO_CD, "NeoCD"),
    CD_GAME(RIP_DISC_3DO, "Opera"),
    CD_GAME(RIP_DISC_CDI, "SAME CDi"),
#undef CD_GAME
    {RIP_DISC_VIDEO_CD, RIP_RECIPE_BINCUE, "BIN/CUE", RIP_CAP_READ_CD, RIP_RECIPE_READY},
    // The game partition is behind the drive's lock: Kreon or OmniDrive firmware.
    {RIP_DISC_XBOX, RIP_RECIPE_XISO, "XISO (xemu)", RIP_CAP_READ_DVD | RIP_CAP_XBOX, RIP_RECIPE_READY},
    {RIP_DISC_XBOX, RIP_RECIPE_ISO, "ISO (Redump)", RIP_CAP_READ_DVD | RIP_CAP_XBOX, RIP_RECIPE_READY},
    {RIP_DISC_XBOX_360, RIP_RECIPE_ISO, "ISO (Xenia)", RIP_CAP_READ_DVD | RIP_CAP_XBOX, RIP_RECIPE_READY},
    // Nintendo's scrambling: OmniDrive's raw read, descrambled here.
    {RIP_DISC_GAMECUBE, RIP_RECIPE_ISO, "ISO (Dolphin)", RIP_CAP_READ_DVD | RIP_CAP_OMNIDRIVE, RIP_RECIPE_READY},
    {RIP_DISC_GAMECUBE, RIP_RECIPE_CISO, "CISO (Nintendont)", RIP_CAP_READ_DVD | RIP_CAP_OMNIDRIVE, RIP_RECIPE_READY},
    {RIP_DISC_GAMECUBE, RIP_RECIPE_RVZ, "RVZ (Dolphin)", RIP_CAP_READ_DVD | RIP_CAP_OMNIDRIVE, RIP_RECIPE_READY},
    {RIP_DISC_WII, RIP_RECIPE_WBFS, "WBFS (USB Loader GX)", RIP_CAP_READ_DVD | RIP_CAP_OMNIDRIVE, RIP_RECIPE_READY},
    {RIP_DISC_WII, RIP_RECIPE_ISO, "ISO (Dolphin)", RIP_CAP_READ_DVD | RIP_CAP_OMNIDRIVE, RIP_RECIPE_READY},
    {RIP_DISC_WII, RIP_RECIPE_RVZ, "RVZ (Dolphin)", RIP_CAP_READ_DVD | RIP_CAP_OMNIDRIVE, RIP_RECIPE_READY},
    {RIP_DISC_WII_U, RIP_RECIPE_ISO, "WUD (Cemu)", RIP_CAP_READ_BD | RIP_CAP_OMNIDRIVE, RIP_RECIPE_READY},
    // Both areas of a GD-ROM, with a .gdi beside the sheet; the drive has to reach the high-density one.
    {RIP_DISC_DREAMCAST, RIP_RECIPE_BINCUE, "GDI + BIN/CUE (Flycast)", RIP_CAP_READ_CD, RIP_RECIPE_READY},
    // Any Blu-ray drive copies it, but only an Xbox can open the packages: a backup to keep.
    {RIP_DISC_XBOX_ONE, RIP_RECIPE_ISO, "ISO (backup)", RIP_CAP_READ_BD, RIP_RECIPE_READY},
};

#define DEFAULT_RETRIES 3
#define PROGRESS_STEP_BYTES (64ull * 1024 * 1024)

const char *rip_availability_name(enum rip_availability avail) {
  switch (avail) {
  case RIP_AVAIL_OK:
    return "available";
  case RIP_AVAIL_NEEDS_DRIVE:
    return "needs another drive";
  case RIP_AVAIL_PLANNED:
    return "planned";
  case RIP_AVAIL_BLOCKED:
    return "blocked";
  }
  return "?";
}

int rip_recipe_is_audio(enum rip_recipe_id id) {
  return rip_recipe_audio_codec(id) >= 0;
}

int rip_recipe_audio_codec(enum rip_recipe_id id) {
  switch (id) {
  case RIP_RECIPE_AUDIO:
    return AUDIO_FLAC;
  case RIP_RECIPE_MP3:
    return AUDIO_MP3;
  case RIP_RECIPE_AAC:
    return AUDIO_AAC;
  case RIP_RECIPE_ALAC:
    return AUDIO_ALAC;
  case RIP_RECIPE_OPUS:
    return AUDIO_OPUS;
  case RIP_RECIPE_WAV:
    return AUDIO_WAV;
  default:
    return -1;
  }
}

static const char *missing_caps_reason(uint32_t missing) {
  if (missing & RIP_CAP_READ_CD) {
    return "needs a drive that reads CDs";
  }
  if (missing & RIP_CAP_READ_BD) {
    return "this drive cannot read Blu-ray";
  }
  if (missing & RIP_CAP_READ_DVD) {
    return "this drive cannot read DVD";
  }
  if (missing & RIP_CAP_KREON) {
    return "needs a drive with Kreon firmware";
  }
  if (missing & RIP_CAP_XBOX) {
    return "needs a drive with Kreon or OmniDrive firmware";
  }
  if (missing & RIP_CAP_OMNIDRIVE) {
    return "needs a drive with OmniDrive firmware (1.0.4 or newer)";
  }
  return "this drive can't";
}

size_t rip_plan(const struct rip_drive *drive, const struct rip_disc *disc, struct rip_choice *out, size_t max) {
  uint32_t caps = drive ? drive->caps : 0xFFFFFFFFu;
  size_t n = 0;
  size_t i;

  for (i = 0; i < RIP_ARRAY_LEN(RECIPES) && n < max; i++) {
    const struct rip_recipe *r = &RECIPES[i];
    struct rip_choice *c;

    if (r->kind != disc->kind) {
      continue;
    }
    c = &out[n++];
    c->recipe = r;
    c->avail = RIP_AVAIL_OK;
    c->reason = "";
    // An Xbox game partition that reads as it is needs no unlock.
    if ((r->needs & ~caps & ~(disc->xbox_partition ? RIP_CAP_XBOX : 0u)) != 0) {
      c->avail = RIP_AVAIL_NEEDS_DRIVE;
      c->reason = missing_caps_reason(r->needs & ~caps);
    } else if ((r->id == RIP_RECIPE_MKV || r->id == RIP_RECIPE_MP4) &&
               !remux_available(r->id == RIP_RECIPE_MP4 ? REMUX_MP4 : REMUX_MKV)) {
      c->avail = RIP_AVAIL_PLANNED;
      c->reason = "not in this build (no FFmpeg)";
    } else if (rip_recipe_is_audio(r->id) && !audio_out_available((enum audio_codec)rip_recipe_audio_codec(r->id))) {
      c->avail = RIP_AVAIL_PLANNED;
      c->reason = "not in this build";
    } else if (r->status == RIP_RECIPE_PLANNED) {
      c->avail = RIP_AVAIL_PLANNED;
      c->reason = "not implemented yet";
    } else if (disc->kind == RIP_DISC_DREAMCAST && disc->gd_rom && !disc->gd_hd_readable) {
      c->avail = RIP_AVAIL_NEEDS_DRIVE;
      c->reason = "this drive can't read the GD-ROM's high-density area";
    }
  }
  return n;
}

const struct rip_recipe *rip_default_recipe(const struct rip_drive *drive, const struct rip_disc *disc) {
  struct rip_choice choices[8];
  size_t n = rip_plan(drive, disc, choices, RIP_ARRAY_LEN(choices));
  size_t i;

  for (i = 0; i < n; i++) {
    if (choices[i].avail == RIP_AVAIL_OK) {
      return choices[i].recipe;
    }
  }
  return NULL;
}

/* Naming */

static void choose_stem(const struct rip_disc *disc, char *stem, size_t len) {
  if (disc->title_id[0]) {
    snprintf(stem, len, "%s", disc->title_id);
  } else if (disc->volume_id[0]) {
    snprintf(stem, len, "%s", disc->volume_id);
  } else {
    snprintf(stem, len, "disc");
  }
  rip_sanitize_name(stem, len);
}

int rip_unique_path(const char *dir, const char *stem, const char *ext, char *out, size_t len) {
  int i;

  for (i = 1; i < 100; i++) {
    int n =
        i == 1 ? snprintf(out, len, "%s/%s%s", dir, stem, ext) : snprintf(out, len, "%s/%s (%d)%s", dir, stem, i, ext);
    if (n < 0 || (size_t)n >= len) {
      return RIP_ERR;
    }
    if (!rip_file_exists(out)) {
      return RIP_OK;
    }
  }
  return RIP_ERR;
}

/* Dumping */

void rip_report(struct rip_progress *ps, const char *phase, uint64_t done, uint64_t total, int force) {
  if (!ps->opts->progress) {
    return;
  }
  if (force || done >= ps->next_report) {
    ps->opts->progress(phase, done, total, ps->opts->user);
    ps->next_report = done + PROGRESS_STEP_BYTES;
  }
}

// Reads a chunk; on failure retries sector by sector so one bad sector does not
// lose the whole chunk, and reports exactly which LBA could not be read.
static int read_chunk(struct rip_reader *r, uint64_t lba, uint32_t count, uint8_t *buf, unsigned retries) {
  uint32_t i;

  if (rip_reader_read(r, lba, count, buf) == RIP_OK) {
    return RIP_OK;
  }
  for (i = 0; i < count; i++) {
    unsigned attempt;
    int ok = 0;

    for (attempt = 0; attempt <= retries && !ok; attempt++) {
      ok = rip_reader_read(r, lba + i, 1, buf + (size_t)i * r->sector_size) == RIP_OK;
    }
    if (!ok) {
      RIP_ERROR("unreadable sector %llu after %u retries", (unsigned long long)(lba + i), retries);
      return RIP_ERR;
    }
  }
  return RIP_OK;
}

// Where read sectors go: a plain image, or a CHD compressed as it fills.
struct sink {
  int fd;                  // .iso
  struct chd_writer *chd;  // .chd
  char part[1100];
  const char *path;
};

static int sink_open(struct sink *k, const char *path, int chd, enum chd_codec codec, uint64_t sectors) {
  memset(k, 0, sizeof(*k));
  k->fd = -1;
  k->path = path;
  snprintf(k->part, sizeof(k->part), "%s.part", path);
  if (chd) {
    return chd_create_dvd(&k->chd, k->part, sectors, codec);
  }
  k->fd = open(k->part, O_WRONLY | O_CREAT | O_TRUNC, 0644);
  if (k->fd < 0) {
    RIP_ERROR("cannot create %s: %s", k->part, strerror(errno));
    return RIP_ERR;
  }
  return RIP_OK;
}

static int sink_write(struct sink *k, const uint8_t *buf, uint32_t sectors, uint32_t sector_size) {
  if (k->chd) {
    return chd_write_sectors(k->chd, buf, sectors);
  }
  if (rip_write_all(k->fd, buf, (size_t)sectors * sector_size) != 0) {
    RIP_ERROR("write to %s failed: %s", k->part, strerror(errno));
    return RIP_ERR;
  }
  return RIP_OK;
}

// Completes the file and gives it its final name, or removes it.
static int sink_close(struct sink *k, int ok) {
  int rc = ok ? RIP_OK : RIP_ERR;

  if (k->chd) {
    if (ok) {
      rc = chd_finish(k->chd, NULL);
    } else {
      chd_abort(k->chd);
    }
    k->chd = NULL;
  } else if (k->fd >= 0) {
    if (ok && fsync(k->fd) != 0) {
      rc = RIP_ERR;
    }
    if (close(k->fd) != 0 && rc == RIP_OK) {
      rc = RIP_ERR;
    }
    k->fd = -1;
  }
  if (rc == RIP_OK && rename(k->part, k->path) != 0) {
    rc = RIP_ERR;
  }
  if (rc != RIP_OK) {
    unlink(k->part);
  }
  return rc;
}

static int dump_image(struct rip_drive *drive, struct rip_reader *r, const char *path, int chd,
                      const struct rip_job_opts *opts, struct rip_hashes *hashes) {
  struct rip_progress ps = {opts, 0};
  struct rip_hash_ctx hctx;
  struct sink sink;
  uint64_t total = r->sector_count * r->sector_size;
  uint64_t lba = 0;
  uint32_t chunk = r->max_sectors ? r->max_sectors : 64;
  unsigned retries = opts->read_retries ? opts->read_retries : DEFAULT_RETRIES;
  uint8_t *buf;
  uint64_t started = rip_now_ms();
  int rc;

  buf = malloc((size_t)chunk * r->sector_size);
  if (!buf) {
    return RIP_ENOMEM;
  }
  rc = sink_open(&sink, path, chd, opts->chd_codec, r->sector_count);
  if (rc != RIP_OK) {
    free(buf);
    return rc;
  }
  if (drive && r->sector_count > 1) {
    mmc_set_max_speed(drive->t, (uint32_t)(r->sector_count - 1));
  }

  rip_hash_init(&hctx);
  rip_report(&ps, "reading", 0, total, 1);
  while (lba < r->sector_count) {
    uint32_t n = (uint32_t)(r->sector_count - lba < chunk ? r->sector_count - lba : chunk);
    size_t bytes = (size_t)n * r->sector_size;

    if (opts->cancel && *opts->cancel) {
      rc = RIP_ECANCEL;
      break;
    }
    rc = read_chunk(r, lba, n, buf, retries);
    if (rc != RIP_OK) {
      break;
    }
    rip_hash_update(&hctx, buf, bytes);
    rc = sink_write(&sink, buf, n, r->sector_size);
    if (rc != RIP_OK) {
      break;
    }
    lba += n;
    rip_report(&ps, "reading", lba * r->sector_size, total, 0);
  }
  free(buf);
  if (rc == RIP_OK) {
    rc = sink_close(&sink, 1);
  } else {
    sink_close(&sink, 0);
  }
  if (rc != RIP_OK) {
    return rc;
  }
  rip_hash_final(&hctx, hashes);
  rip_report(&ps, "reading", total, total, 1);
  {
    uint64_t ms = rip_now_ms() - started;
    RIP_INFO("read %llu MiB in %llu s (%.1f MiB/s)", (unsigned long long)(total >> 20), (unsigned long long)(ms / 1000),
             ms ? (double)total / 1048576.0 / ((double)ms / 1000.0) : 0.0);
  }
  return RIP_OK;
}

static int is_nintendo(enum rip_disc_kind kind) {
  return kind == RIP_DISC_GAMECUBE || kind == RIP_DISC_WII || kind == RIP_DISC_WII_U;
}

// Discs only custom firmware reads: through OmniDrive's raw read (Nintendo),
// or past the video partition with Kreon or OmniDrive (Xbox).
static int dump_custom(struct rip_drive *drive, struct rip_reader *reader, const struct rip_disc *disc,
                       const char *path, const struct rip_job_opts *opts, struct rip_hashes *hashes) {
  struct rip_reader custom;
  int rc;

  if (!drive) {
    return RIP_ENOTSUP;
  }
  if (is_nintendo(disc->kind)) {
    // The DVDs' length is the physical format's; a Wii U's, READ CAPACITY's.
    uint64_t sectors = disc->kind != RIP_DISC_WII_U && disc->sector_count ? disc->sector_count : reader->sector_count;
    rc = rip_reader_open_nintendo(&custom, drive->t, disc->kind == RIP_DISC_WII_U, sectors);
  } else {
    struct xbox_layout *x = malloc(sizeof(*x));
    rc = x ? xbox_read_layout(drive, x) : RIP_ENOMEM;
    if (rc == RIP_OK) {
      rc = rip_reader_open_xbox(&custom, drive, x);
      if (rc != RIP_OK) {
        xbox_relock(drive, x);
      }
    }
    free(x);
  }
  if (rc != RIP_OK) {
    RIP_ERROR("this drive couldn't start reading the disc (%d)", rc);
    return rc;
  }
  rc = dump_image(drive, &custom, path, 0, opts, hashes);
  rip_reader_close(&custom);
  return rc;
}

static int copy_file(const char *from, const char *to) {
  uint8_t *buf = malloc(1 << 20);
  int in = open(from, O_RDONLY);
  int out = open(to, O_WRONLY | O_CREAT | O_TRUNC, 0644);
  int rc = RIP_OK;
  long n;

  if (!buf || in < 0 || out < 0) {
    rc = RIP_ERR;
  }
  while (rc == RIP_OK && (n = rip_read_full(in, buf, 1 << 20)) > 0) {
    if (rip_write_all(out, buf, (size_t)n) != 0) {
      rc = RIP_ERR;
    }
  }
  free(buf);
  if (in >= 0) {
    close(in);
  }
  if (out >= 0 && close(out) != 0) {
    rc = RIP_ERR;
  }
  if (rc != RIP_OK) {
    unlink(to);
  }
  return rc;
}

static void write_manifest(const char *path, const struct rip_drive *drive, const struct rip_disc *disc,
                           const struct rip_job_result *res, const char *key_source) {
  char crc[9];
  char md5[33];
  char sha1[41];
  FILE *f = fopen(path, "w");

  if (!f) {
    RIP_WARN("cannot write manifest %s", path);
    return;
  }
  snprintf(crc, sizeof(crc), "%08x", res->hashes.crc32);
  rip_hex(res->hashes.md5, 16, md5);
  rip_hex(res->hashes.sha1, 20, sha1);
  fprintf(f, "disc: %s\n", rip_disc_kind_name(disc->kind));
  fprintf(f, "profile: %s (0x%04x)\n", mmc_profile_name(disc->profile), disc->profile);
  fprintf(f, "title_id: %s\n", disc->title_id);
  fprintf(f, "volume_id: %s\n", disc->volume_id);
  fprintf(f, "name: %s\n", res->name);
  if (drive) {
    fprintf(f, "drive: %s %s %s (%s)\n", drive->inquiry.vendor, drive->inquiry.product, drive->inquiry.revision,
            rip_drive_source_name(drive->source));
  }
  fprintf(f, "image: %s\n", res->image_path);
  if (res->encrypted_path[0]) {
    fprintf(f, "encrypted_image: %s\n", res->encrypted_path);
  }
  fprintf(f, "# Hashes are of the image exactly as read from the disc, before any decryption.\n");
  fprintf(f, "size: %llu\ncrc32: %s\nmd5: %s\nsha1: %s\n", (unsigned long long)res->hashes.size, crc, md5, sha1);
  if (disc->kind == RIP_DISC_PS3_BD || disc->kind == RIP_DISC_PS3_DVD) {
    fprintf(f, "decrypted: %s\nkey_source: %s\n", res->decrypted ? "yes" : "no", key_source);
  }
  fclose(f);
}

static void ps3_progress(uint64_t done, uint64_t total, void *user) {
  struct rip_progress *ps = user;

  rip_report(ps, "decrypting", done, total, done == total);
}

// Finds the key, renames to the Redump name, and decrypts in place.
static int finish_ps3(const struct rip_job_opts *opts, char *stem, size_t stem_len, struct rip_job_result *res,
                      const char **key_source) {
  const struct rip_key_entry *entry = NULL;
  struct rip_progress ps = {opts, 0};
  uint8_t key[16];
  char stem_path[1100];
  int rc;

  *key_source = "none";
  if (opts->ps3_keys) {
    entry = rip_keydb_find(opts->ps3_keys, res->hashes.sha1, res->hashes.size);
  }
  if (entry) {
    char named[1024];

    memcpy(key, entry->key, 16);
    *key_source = "keydb";
    // The Redump name, unless the user named the rip themselves.
    if (!opts->name || !opts->name[0]) {
      snprintf(stem, stem_len, "%s", entry->name);
      rip_sanitize_name(stem, stem_len);
      snprintf(res->name, sizeof(res->name), "%s", entry->name);
      if (rip_unique_path(opts->out_dir, stem, ".iso", named, sizeof(named)) == RIP_OK &&
          rename(res->image_path, named) == 0) {
        snprintf(res->image_path, sizeof(res->image_path), "%s", named);
      }
    }
    RIP_INFO("Redump match: %s", entry->name);
  } else {
    snprintf(stem_path, sizeof(stem_path), "%s/%s", opts->out_dir, stem);
    if (rip_key_from_sidecar(stem_path, key) != RIP_OK) {
      RIP_WARN("no PS3 key for this image; kept encrypted. Add %s.key or %s.dkey, or update the key file.", stem_path,
               stem_path);
      return RIP_OK;
    }
    *key_source = "sidecar";
  }
  res->ps3_key_found = 1;

  if (opts->ps3_keep_encrypted) {
    char dec[1024];
    char dec_stem[300];

    snprintf(dec_stem, sizeof(dec_stem), "%s (decrypted)", stem);
    if (rip_unique_path(opts->out_dir, dec_stem, ".iso", dec, sizeof(dec)) != RIP_OK ||
        copy_file(res->image_path, dec) != RIP_OK) {
      RIP_ERROR("could not copy the image for decryption");
      return RIP_ERR;
    }
    snprintf(res->encrypted_path, sizeof(res->encrypted_path), "%s", res->image_path);
    snprintf(res->image_path, sizeof(res->image_path), "%s", dec);
  }
  rc = ps3_decrypt_file(res->image_path, key, ps3_progress, &ps);
  if (rc != RIP_OK) {
    RIP_ERROR("decryption failed; %s is partly decrypted", res->image_path);
    return rc;
  }
  res->decrypted = 1;
  return RIP_OK;
}

static int extract_progress(void *ud, uint64_t done, uint64_t total) {
  const struct rip_job_opts *opts = ud;
  if (opts->progress) {
    opts->progress("extracting", done, total, opts->user);
  }
  return opts->cancel && *opts->cancel;
}

// A PS3 game folder (PS3_GAME, PS3_DISC.SFB...), as RPCS3 and PS3 loaders take
// it: the decrypted image's files, the image itself gone after.
static int ps3_folder(const struct rip_job_opts *opts, const char *stem, struct rip_job_result *res) {
  char folder[1024];
  struct rip_reader r;
  struct iso_fs fs;
  struct stat st;
  int64_t size = stat(res->image_path, &st) == 0 ? (int64_t)st.st_size : -1;
  int fd;
  int rc;

  if (!res->decrypted) {
    RIP_ERROR("a PS3 game folder needs the disc's key; without one only an encrypted ISO can be made");
    return RIP_ERR;
  }
  if (rip_unique_path(opts->out_dir, stem, "", folder, sizeof(folder)) != RIP_OK) {
    return RIP_ERR;
  }
  fd = open(res->image_path, O_RDONLY);
  if (fd < 0 || size <= 0 || rip_reader_open_fd(&r, fd, 2048, (uint64_t)size, 1) != RIP_OK) {
    if (fd >= 0) {
      close(fd);
    }
    return RIP_ERR;
  }
  rc = iso_mount(&fs, &r);
  if (rc == RIP_OK) {
    rc = iso_extract(&fs, folder, extract_progress, (void *)opts);
  }
  rip_reader_close(&r);
  if (rc != RIP_OK) {
    RIP_ERROR("couldn't extract %s into %s", res->image_path, folder);
    return rc;
  }
  unlink(res->image_path);
  snprintf(res->image_path, sizeof(res->image_path), "%s", folder);
  return RIP_OK;
}

// webMAN and IRISMAN's parts: <name>.iso.0, .iso.1... of 0xFFFF0000 bytes, in
// a folder of their own (webMAN looks one folder deep). An image that fits
// FAT32 as it is stays whole.
#define PS3_SPLIT_PART 0xFFFF0000ull

static int ps3_split(const struct rip_job_opts *opts, const char *stem, struct rip_job_result *res) {
  char folder[1024];
  char prefix[2100];
  const char *base = strrchr(res->image_path, '/');
  struct stat st;

  if (stat(res->image_path, &st) != 0 || (uint64_t)st.st_size <= PS3_SPLIT_PART) {
    return RIP_OK;
  }
  if (rip_unique_path(opts->out_dir, stem, "", folder, sizeof(folder)) != RIP_OK || rip_mkdir_p(folder) != 0) {
    return RIP_ERR;
  }
  snprintf(prefix, sizeof(prefix), "%s/%s", folder, base ? base + 1 : res->image_path);
  if (rip_split_file(res->image_path, PS3_SPLIT_PART, prefix) != RIP_OK) {
    RIP_ERROR("couldn't split %s for FAT32", res->image_path);
    rmdir(folder);
    return RIP_ERR;
  }
  snprintf(res->image_path, sizeof(res->image_path), "%s", folder);
  return RIP_OK;
}

// The image as read becomes the format asked for: written beside it, then
// put in its place. The manifest keeps the read image's hashes, the ones
// Redump lists.
struct convert_progress {
  const struct rip_job_opts *opts;
  const char *phase;
};

static int converting(void *ud, uint64_t done, uint64_t total) {
  const struct convert_progress *c = ud;
  if (c->opts->progress) {
    c->opts->progress(c->phase, done, total, c->opts->user);
  }
  return c->opts->cancel && *c->opts->cancel;
}

static int convert_image(enum rip_recipe_id id, const struct rip_job_opts *opts, const char *stem,
                         struct rip_job_result *res) {
  const char *ext = id == RIP_RECIPE_CISO ? ".ciso" : id == RIP_RECIPE_RVZ ? ".rvz" : ".iso";
  struct convert_progress cp = {opts, id == RIP_RECIPE_XISO ? "trimming" : "compressing"};
  char out[sizeof(res->image_path)];
  char part[sizeof(res->image_path) + 8];
  int rc;

  if (id != RIP_RECIPE_XISO) {
    if (rip_unique_path(opts->out_dir, stem, ext, out, sizeof(out)) != RIP_OK) {
      return RIP_ERR;
    }
  } else {
    // An XISO keeps the name: it replaces the image it came from.
    snprintf(out, sizeof(out), "%s", res->image_path);
  }
  snprintf(part, sizeof(part), "%s.part", out);
  // Level 5, Dolphin's own default for RVZ.
  rc = id == RIP_RECIPE_CISO  ? ciso_from_iso(res->image_path, part, converting, &cp)
       : id == RIP_RECIPE_RVZ ? rvz_from_iso(res->image_path, part, 5, converting, &cp)
                              : xiso_from_iso(res->image_path, part, converting, &cp);
  if (rc != RIP_OK) {
    unlink(part);
    return rc;
  }
  if (unlink(res->image_path) != 0 || rename(part, out) != 0) {
    RIP_ERROR("cannot put %s in place: %s", out, strerror(errno));
    return RIP_ERR;
  }
  snprintf(res->image_path, sizeof(res->image_path), "%s", out);
  return RIP_OK;
}

// A Wii disc the way USB Loader GX and WiiFlow find it: "<name> [ID6]/ID6.wbfs",
// in FAT32-sized parts when asked.
static int convert_wbfs(const struct rip_job_opts *opts, const char *stem, struct rip_job_result *res) {
  struct convert_progress cp = {opts, "converting"};
  char id[7] = "";
  char folder[sizeof(res->image_path)];
  char name[300];
  char out[sizeof(res->image_path) + 16];
  int fd = open(res->image_path, O_RDONLY);
  int rc;
  int i;

  if (fd < 0 || rip_seek_read(fd, 0, id, 6) != 0) {
    RIP_ERROR("cannot read %s: %s", res->image_path, strerror(errno));
    if (fd >= 0) {
      close(fd);
    }
    return RIP_ERR;
  }
  close(fd);
  for (i = 0; i < 6; i++) {
    if (!isalnum((unsigned char)id[i])) {
      id[i] = '_';
    }
  }
  snprintf(name, sizeof(name), strstr(stem, id) ? "%s" : "%s [%s]", stem, id);
  if (rip_unique_path(opts->out_dir, name, "", folder, sizeof(folder)) != RIP_OK || rip_mkdir_p(folder) != 0) {
    RIP_ERROR("cannot create the folder for %s", name);
    return RIP_ERR;
  }
  snprintf(out, sizeof(out), "%s/%s.wbfs", folder, id);
  rc = wbfs_from_iso(res->image_path, out, opts->split_fat32 ? WBFS_SPLIT_FAT32 : 0, converting, &cp);
  if (rc != RIP_OK) {
    rmdir(folder);
    return rc;
  }
  if (unlink(res->image_path) != 0) {
    RIP_WARN("cannot remove %s: %s", res->image_path, strerror(errno));
  }
  snprintf(res->image_path, sizeof(res->image_path), "%s", folder);
  return RIP_OK;
}

static int movie_progress(void *ud, uint64_t done, uint64_t total) {
  const struct rip_job_opts *opts = ud;
  if (opts->progress) {
    opts->progress("converting", done, total, opts->user);
  }
  return opts->cancel && *opts->cancel;
}

// A DVD's main title to MKV or MP4: read through CSS straight off the disc.
static int rip_movie(const struct rip_recipe *recipe, struct rip_drive *drive, struct rip_reader *reader,
                     const struct rip_disc *disc, const struct rip_job_opts *opts, const char *stem,
                     struct rip_job_result *res) {
  struct rip_reader css;
  struct rip_reader *r = reader;
  struct dvd_disc *dvd = calloc(1, sizeof(*dvd));
  struct remux_opts ro;
  const struct dvd_title *t;
  int mp4 = recipe->id == RIP_RECIPE_MP4;
  int rc;

  if (!dvd) {
    return RIP_ENOMEM;
  }
  if (disc->css) {
    if (!drive || rip_reader_open_css(&css, reader, drive->t, opts->css_cache_dir) != RIP_OK) {
      RIP_ERROR("CSS: the drive couldn't be authenticated for this disc");
      free(dvd);
      return RIP_ERR;
    }
    r = &css;
  }
  rc = dvd_load(r, dvd);
  if (rc != RIP_OK || dvd->main_title < 0) {
    RIP_ERROR("this DVD's titles couldn't be read");
    rc = RIP_ERR;
    goto done;
  }
  t = &dvd->titles[dvd->main_title];
  if (rip_unique_path(opts->out_dir, stem, mp4 ? ".mp4" : ".mkv", res->image_path, sizeof(res->image_path)) != RIP_OK) {
    rc = RIP_ERR;
    goto done;
  }
  memset(&ro, 0, sizeof(ro));
  ro.format = mp4 ? REMUX_MP4 : REMUX_MKV;
  ro.crf = opts->video_crf;
  ro.preset = opts->video_preset;
  ro.progress = movie_progress;
  ro.ud = (void *)opts;
  if (drive && disc->sector_count > 1) {
    mmc_set_max_speed(drive->t, (uint32_t)(disc->sector_count - 1));
  }
  rc = remux_title(r, t, res->image_path, &ro);
  if (rc != RIP_OK) {
    res->image_path[0] = '\0';
  }

done:
  dvd_free(dvd);
  free(dvd);
  if (r == &css) {
    rip_reader_close(&css);
  }
  return rc;
}

int rip_run(const struct rip_recipe *recipe, struct rip_drive *drive, struct rip_reader *reader,
            const struct rip_disc *disc, const struct rip_job_opts *opts, struct rip_job_result *res) {
  const char *key_source = "none";
  const char *ext;
  char stem[256];
  char manifest_stem[300];
  int rc;

  memset(res, 0, sizeof(*res));
  if (!recipe || recipe->status != RIP_RECIPE_READY) {
    return RIP_ENOTSUP;
  }
  if (recipe->id != RIP_RECIPE_ISO && recipe->id != RIP_RECIPE_PS3_ISO && recipe->id != RIP_RECIPE_CHD &&
      !(recipe->id == RIP_RECIPE_FOLDER && (disc->kind == RIP_DISC_PS3_BD || disc->kind == RIP_DISC_PS3_DVD)) &&
      recipe->id != RIP_RECIPE_BINCUE && !rip_recipe_is_audio(recipe->id) && recipe->id != RIP_RECIPE_MKV &&
      recipe->id != RIP_RECIPE_MP4 && recipe->id != RIP_RECIPE_XISO && recipe->id != RIP_RECIPE_CISO &&
      recipe->id != RIP_RECIPE_WBFS && recipe->id != RIP_RECIPE_RVZ) {
    return RIP_ENOTSUP;
  }
  if (recipe->id == RIP_RECIPE_MKV || recipe->id == RIP_RECIPE_MP4) {
    if (rip_mkdir_p(opts->out_dir) != 0) {
      RIP_ERROR("cannot create %s", opts->out_dir);
      return RIP_ERR;
    }
    if (opts->name && opts->name[0]) {
      snprintf(stem, sizeof(stem), "%s", opts->name);
      rip_sanitize_name(stem, sizeof(stem));
    } else {
      choose_stem(disc, stem, sizeof(stem));
    }
    snprintf(res->name, sizeof(res->name), "%s", stem);
    return rip_movie(recipe, drive, reader, disc, opts, stem, res);
  }
  if (disc->has_toc &&
      (recipe->id == RIP_RECIPE_BINCUE || recipe->id == RIP_RECIPE_CHD || rip_recipe_is_audio(recipe->id))) {
    // A CD: raw frames, track by track.
    if (rip_mkdir_p(opts->out_dir) != 0) {
      RIP_ERROR("cannot create %s", opts->out_dir);
      return RIP_ERR;
    }
    if (opts->name && opts->name[0]) {
      snprintf(stem, sizeof(stem), "%s", opts->name);
      rip_sanitize_name(stem, sizeof(stem));
    } else {
      choose_stem(disc, stem, sizeof(stem));
    }
    snprintf(res->name, sizeof(res->name), "%s", stem);
    RIP_INFO("ripping %s to %s (%s)", rip_disc_kind_name(disc->kind), opts->out_dir, recipe->output);
    return rip_cd_run(recipe, drive, disc, opts, stem, res);
  }
  if (!reader || reader->sector_size != 2048 || reader->sector_count == 0) {
    RIP_ERROR("this disc cannot be read as 2048-byte sectors");
    return RIP_ENOTSUP;
  }
  if (rip_mkdir_p(opts->out_dir) != 0) {
    RIP_ERROR("cannot create %s", opts->out_dir);
    return RIP_ERR;
  }

  if (opts->name && opts->name[0]) {
    snprintf(stem, sizeof(stem), "%s", opts->name);
    rip_sanitize_name(stem, sizeof(stem));
  } else {
    choose_stem(disc, stem, sizeof(stem));
  }
  snprintf(res->name, sizeof(res->name), "%s", stem);
  ext = recipe->id == RIP_RECIPE_CHD ? ".chd" : disc->kind == RIP_DISC_WII_U ? ".wud" : ".iso";
  if (rip_unique_path(opts->out_dir, stem, ext, res->image_path, sizeof(res->image_path)) != RIP_OK) {
    return RIP_ERR;
  }
  RIP_INFO("ripping %s -> %s (%s)", rip_disc_kind_name(disc->kind), res->image_path, recipe->output);

  if (is_nintendo(disc->kind) ||
      ((disc->kind == RIP_DISC_XBOX || disc->kind == RIP_DISC_XBOX_360) && !disc->xbox_partition)) {
    rc = dump_custom(drive, reader, disc, res->image_path, opts, &res->hashes);
  } else if (disc->kind == RIP_DISC_BD_VIDEO) {
    // A Blu-ray movie: its streams decrypted as they're read, when the
    // KEYDB.cfg has the disc's key; else the image as it is.
    struct rip_reader aacs;
    int ar = rip_reader_open_aacs(&aacs, reader, opts->aacs_keydb, res->aacs_why, sizeof(res->aacs_why));
    if (ar == RIP_OK) {
      rc = dump_image(drive, &aacs, res->image_path, recipe->id == RIP_RECIPE_CHD, opts, &res->hashes);
      rip_reader_close(&aacs);
      res->aacs = 1;
      res->decrypted = rc == RIP_OK;
    } else {
      res->aacs = ar == RIP_ENOTSUP ? 0 : 2;
      rc = dump_image(drive, reader, res->image_path, recipe->id == RIP_RECIPE_CHD, opts, &res->hashes);
    }
  } else if (disc->css) {
    // DVD-Video: the drive authenticated, the VOBs descrambled as they're read.
    struct rip_reader css;
    if (!drive || rip_reader_open_css(&css, reader, drive->t, opts->css_cache_dir) != RIP_OK) {
      RIP_ERROR("CSS: the drive couldn't be authenticated for this disc");
      return RIP_ERR;
    }
    rc = dump_image(drive, &css, res->image_path, recipe->id == RIP_RECIPE_CHD, opts, &res->hashes);
    rip_reader_close(&css);
  } else {
    rc = dump_image(drive, reader, res->image_path, recipe->id == RIP_RECIPE_CHD, opts, &res->hashes);
  }
  if (rc != RIP_OK) {
    return rc;
  }
  if (recipe->id == RIP_RECIPE_XISO || recipe->id == RIP_RECIPE_CISO || recipe->id == RIP_RECIPE_RVZ) {
    rc = convert_image(recipe->id, opts, stem, res);
  } else if (recipe->id == RIP_RECIPE_WBFS) {
    rc = convert_wbfs(opts, stem, res);
  }
  if (recipe->id == RIP_RECIPE_PS3_ISO || recipe->id == RIP_RECIPE_FOLDER) {
    rc = finish_ps3(opts, stem, sizeof(stem), res, &key_source);
    if (rc == RIP_OK && recipe->id == RIP_RECIPE_FOLDER) {
      rc = ps3_folder(opts, stem, res);
    } else if (rc == RIP_OK && opts->split_fat32) {
      rc = ps3_split(opts, stem, res);
    }
  }

  snprintf(manifest_stem, sizeof(manifest_stem), "%s", stem);
  if (rip_unique_path(opts->out_dir, manifest_stem, ".txt", res->manifest_path, sizeof(res->manifest_path)) == RIP_OK) {
    write_manifest(res->manifest_path, drive, disc, res, key_source);
  }
  return rc;
}
