#include "ripper/disc.h"

#include <ctype.h>
#include <dirent.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>

#include "ripper/common.h"
#include "ripper/iso9660.h"
#include "ripper/log.h"
#include "ripper/mmc.h"
#include "ripper/omnidrive.h"
#include "ripper/pkg.h"
#include "ripper/sfo.h"
#include "ripper/udf.h"
#include "ripper/xbox.h"

#define SMALL_FILE_MAX (64 * 1024)

const char *rip_disc_kind_name(enum rip_disc_kind kind) {
  static const char *names[RIP_DISC_KIND_COUNT] = {
      "Unknown",   "CD (data)", "CD (audio)",   "PS1",       "PS2 (CD)",   "PS2 (DVD)",     "PS3 (DVD)",
      "PS3 (BD)",  "PS4",       "PS5",          "DVD-Video", "DVD (data)", "Blu-ray Video", "Blu-ray (data)",
      "Saturn",    "Sega CD",   "PC Engine CD", "PC-FX",     "Neo Geo CD", "3DO",           "CD-i",
      "Video CD",  "Xbox",      "Xbox One",     "Xbox 360",  "GameCube",   "Wii",           "Wii U",
      "Dreamcast",
  };

  return kind < RIP_DISC_KIND_COUNT ? names[kind] : "Unknown";
}

void rip_format_title_id(const char *raw, char *out, size_t out_len) {
  size_t n = strlen(raw);

  if (n == 9 && !strchr(raw, '-') && isalpha((unsigned char)raw[0]) && isdigit((unsigned char)raw[4])) {
    snprintf(out, out_len, "%.4s-%.5s", raw, raw + 4);
  } else {
    snprintf(out, out_len, "%s", raw);
  }
}

int rip_parse_system_cnf(const char *cnf, size_t len, int *is_ps2, char *serial, size_t serial_len) {
  const char *p = cnf;
  const char *end = cnf + len;

  *is_ps2 = 0;
  serial[0] = '\0';
  while (p < end) {
    const char *eol = memchr(p, '\n', (size_t)(end - p));
    const char *line_end = eol ? eol : end;
    const char *q = p;
    int boot2 = 0;

    while (q < line_end && isspace((unsigned char)*q)) {
      q++;
    }
    if (line_end - q >= 5 && strncmp(q, "BOOT2", 5) == 0) {
      boot2 = 1;
      q += 5;
    } else if (line_end - q >= 4 && strncmp(q, "BOOT", 4) == 0) {
      q += 4;
    } else {
      p = eol ? eol + 1 : end;
      continue;
    }
    while (q < line_end && (isspace((unsigned char)*q) || *q == '=')) {
      q++;
    }
    {
      // The serial is the boot file name: after the last '\' or ':' and before ';'.
      const char *name = q;
      const char *s;
      size_t o = 0;

      for (s = q; s < line_end; s++) {
        if (*s == '\\' || *s == ':' || *s == '/') {
          name = s + 1;
        }
      }
      for (s = name; s < line_end && *s != ';' && !isspace((unsigned char)*s) && o + 1 < serial_len; s++) {
        if (*s == '.') {
          continue;
        }
        serial[o++] = *s == '_' ? '-' : (char)toupper((unsigned char)*s);
      }
      serial[o] = '\0';
    }
    *is_ps2 = boot2;
    return serial[0] ? RIP_OK : RIP_ENOENT;
  }
  return RIP_ENOENT;
}

static int iso_read_small(struct iso_fs *fs, const char *path, uint8_t **data, size_t *len) {
  struct iso_entry e;

  if (iso_lookup(fs, path, &e) != RIP_OK || e.is_dir) {
    return RIP_ENOENT;
  }
  return iso_read_file(fs, &e, SMALL_FILE_MAX, data, len);
}

static int iso_has_dir(struct iso_fs *fs, const char *path) {
  struct iso_entry e;

  return iso_lookup(fs, path, &e) == RIP_OK && e.is_dir;
}

static int iso_has(struct iso_fs *fs, const char *path) {
  struct iso_entry e;

  return iso_lookup(fs, path, &e) == RIP_OK;
}

static int load_mount_file(const char *root, const char *rel, uint8_t **out, size_t *out_len) {
  char path[512];
  FILE *f;
  uint8_t *buf;
  size_t n;

  snprintf(path, sizeof(path), "%s/%s", root, rel);
  f = fopen(path, "rb");
  if (!f) {
    return RIP_ENOENT;
  }
  buf = malloc(SMALL_FILE_MAX);
  if (!buf) {
    fclose(f);
    return RIP_ENOMEM;
  }
  n = fread(buf, 1, SMALL_FILE_MAX, f);
  fclose(f);
  *out = buf;
  *out_len = n;
  return RIP_OK;
}

static void title_from_sfo(const uint8_t *sfo, size_t len, struct rip_disc *d) {
  char raw[32];

  if (sfo_get_string(sfo, len, "TITLE_ID", raw, sizeof(raw)) == RIP_OK && raw[0]) {
    rip_format_title_id(raw, d->title_id, sizeof(d->title_id));
  }
  if (!d->title[0]) {
    sfo_get_string(sfo, len, "TITLE", d->title, sizeof(d->title));
  }
}

// PS4 discs carry param.sfo behind a 0x800-byte header; PS5 discs carry param.json.
// A string value in JSON text: the first "key": "value" after from. Enough
// for param.json, whose values here are plain strings.
static int json_string(const char *text, const char *from, const char *key, char *out, size_t n) {
  char pat[96];
  const char *p;
  const char *e;

  snprintf(pat, sizeof(pat), "\"%s\"", key);
  p = strstr(from ? from : text, pat);
  if (!p) {
    return RIP_ENOENT;
  }
  p = strchr(p + strlen(pat), ':');
  if (!p) {
    return RIP_ENOENT;
  }
  p = strchr(p, '"');
  if (!p) {
    return RIP_ENOENT;
  }
  e = strchr(++p, '"');
  if (!e) {
    return RIP_ENOENT;
  }
  snprintf(out, n, "%.*s", (int)(e - p), p);
  return RIP_OK;
}

// A PS5 param.json: the title ID, and the title in the default language.
static void title_from_ps5_json(const char *text, struct rip_disc *d) {
  char lang[32];
  char key[40];
  const char *at = NULL;

  json_string(text, NULL, "titleId", d->title_id, sizeof(d->title_id));
  if (json_string(text, NULL, "defaultLanguage", lang, sizeof(lang)) == RIP_OK) {
    snprintf(key, sizeof(key), "\"%s\"", lang);
    at = strstr(text, key);
    // Past the "defaultLanguage" value itself, to the language's own object.
    if (at && (at = strstr(at + strlen(key), key)) == NULL) {
      at = strstr(text, key);
    }
  }
  json_string(text, at, "titleName", d->title, sizeof(d->title));
}

// On a PS5 game disc bd/param.json is wrapped (SCECDF) and unreadable; the
// small app/<title ID>/app_sc.pkg next to the game carries a clear copy, the
// way a PS4 app.pkg carries param.sfo.
static void title_from_ps5_disc(const char *root, struct rip_disc *d) {
  char dir[512];
  DIR *dp;
  struct dirent *e;

  snprintf(dir, sizeof(dir), "%s/app", root);
  dp = opendir(dir);
  if (!dp) {
    return;
  }
  while ((e = readdir(dp)) != NULL) {
    char path[600];
    struct pkg pkg;
    int fd;
    unsigned i;
    if (strlen(e->d_name) != 9 || strncmp(e->d_name, "PPSA", 4) != 0) {
      continue;
    }
    snprintf(d->title_id, sizeof(d->title_id), "%.9s", e->d_name);
    snprintf(path, sizeof(path), "%.500s/%.9s/app_sc.pkg", dir, e->d_name);
    fd = open(path, O_RDONLY);
    if (fd < 0) {
      break;
    }
    if (pkg_open(&pkg, fd) == RIP_OK) {
      for (i = 0; i < pkg.count; i++) {
        uint8_t *data = NULL;
        size_t len = 0;
        if (strcmp(pkg.entries[i].name, "param.json") != 0 || pkg_entry_encrypted(&pkg.entries[i]) ||
            pkg_read_entry(&pkg, &pkg.entries[i], &data, &len) != RIP_OK) {
          continue;
        }
        {
          char *text = malloc(len + 1);
          if (text) {
            memcpy(text, data, len);
            text[len] = '\0';
            title_from_ps5_json(text, d);
            free(text);
          }
        }
        free(data);
        break;
      }
      pkg_close(&pkg);
    }
    close(fd);
    break;
  }
  closedir(dp);
}

static void title_from_mount(const char *root, struct rip_disc *d) {
  uint8_t *data = NULL;
  size_t len = 0;

  if (d->kind == RIP_DISC_PS4_BD && load_mount_file(root, "bd/param.sfo", &data, &len) == RIP_OK) {
    if (len > 0x800) {
      title_from_sfo(data + 0x800, len - 0x800, d);
    }
    free(data);
  } else if (d->kind == RIP_DISC_PS5_BD) {
    if (load_mount_file(root, "bd/param.json", &data, &len) == RIP_OK && len && data[0] == '{') {
      char *text = malloc(len + 1);
      if (text) {
        memcpy(text, data, len);
        text[len] = '\0';
        title_from_ps5_json(text, d);
        free(text);
      }
    }
    free(data);
    if (!d->title[0]) {
      title_from_ps5_disc(root, d);
    }
  } else if ((d->kind == RIP_DISC_PS3_BD || d->kind == RIP_DISC_PS3_DVD) &&
             load_mount_file(root, "PS3_GAME/PARAM.SFO", &data, &len) == RIP_OK) {
    title_from_sfo(data, len, d);
    free(data);
  }
}

static enum rip_disc_kind kind_from_profile(uint16_t profile) {
  switch (profile) {
  case MMC_PROFILE_PS1_CD:
    return RIP_DISC_PS1;
  case MMC_PROFILE_PS2_CD:
    return RIP_DISC_PS2_CD;
  case MMC_PROFILE_PS2_DVD:
    return RIP_DISC_PS2_DVD;
  case MMC_PROFILE_PS3_DVD:
    return RIP_DISC_PS3_DVD;
  case MMC_PROFILE_PS3_BD:
    return RIP_DISC_PS3_BD;
  case MMC_PROFILE_PS4_BD:
    return RIP_DISC_PS4_BD;
  case MMC_PROFILE_PS5_BD:
    return RIP_DISC_PS5_BD;
  default:
    if (mmc_profile_is_cd(profile)) {
      return RIP_DISC_CD_DATA;
    }
    if (mmc_profile_is_dvd(profile)) {
      return RIP_DISC_DVD_DATA;
    }
    if (mmc_profile_is_bd(profile)) {
      return RIP_DISC_BD_DATA;
    }
    return RIP_DISC_UNKNOWN;
  }
}

// Refines a generic kind (or confirms a vendor one) from the file system.
static void identify_from_iso(struct iso_fs *fs, struct rip_disc *d) {
  uint8_t *data = NULL;
  size_t len = 0;
  int is_ps2 = 0;
  char serial[32];

  if (d->kind != RIP_DISC_CD_DATA && d->kind != RIP_DISC_DVD_DATA && d->kind != RIP_DISC_BD_DATA &&
      d->kind != RIP_DISC_PS1 && d->kind != RIP_DISC_PS2_CD && d->kind != RIP_DISC_PS2_DVD &&
      d->kind != RIP_DISC_PS3_BD && d->kind != RIP_DISC_PS3_DVD) {
    return;  // a CD system already found from its header
  }
  if (mmc_profile_is_cd(d->profile) && iso_has(fs, "IPL.TXT")) {
    d->kind = RIP_DISC_NEOGEO_CD;
    return;
  }
  // A Video CD keeps its streams in MPEGAV; a Super Video CD in MPEG2.
  if (mmc_profile_is_cd(d->profile) && ((iso_has_dir(fs, "VCD") && iso_has_dir(fs, "MPEGAV")) ||
                                        (iso_has_dir(fs, "SVCD") && iso_has_dir(fs, "MPEG2")))) {
    d->kind = RIP_DISC_VIDEO_CD;
    return;
  }
  if (iso_read_small(fs, "SYSTEM.CNF", &data, &len) == RIP_OK) {
    if (rip_parse_system_cnf((const char *)data, len, &is_ps2, serial, sizeof(serial)) == RIP_OK) {
      snprintf(d->title_id, sizeof(d->title_id), "%s", serial);
    }
    free(data);
    data = NULL;
    if (mmc_profile_is_cd(d->profile)) {
      d->kind = is_ps2 ? RIP_DISC_PS2_CD : RIP_DISC_PS1;
    } else if (mmc_profile_is_dvd(d->profile)) {
      d->kind = RIP_DISC_PS2_DVD;
    }
    return;
  }
  if (mmc_profile_is_cd(d->profile) && iso_has(fs, "PSX.EXE")) {
    d->kind = RIP_DISC_PS1;
    return;
  }
  if (iso_has(fs, "PS3_DISC.SFB")) {
    d->kind = mmc_profile_is_dvd(d->profile) ? RIP_DISC_PS3_DVD : RIP_DISC_PS3_BD;
    // PARAM.SFO usually sits in the unencrypted region; if not, the magic check fails.
    if (iso_read_small(fs, "PS3_GAME/PARAM.SFO", &data, &len) == RIP_OK) {
      title_from_sfo(data, len, d);
      free(data);
    }
    return;
  }
  if (d->kind == RIP_DISC_DVD_DATA && iso_has_dir(fs, "VIDEO_TS")) {
    d->kind = RIP_DISC_DVD_VIDEO;
  } else if (d->kind == RIP_DISC_BD_DATA && iso_has_dir(fs, "BDMV")) {
    d->kind = RIP_DISC_BD_VIDEO;
  }
  // TODO: most Blu-ray movies are UDF-only; add a UDF reader to detect BDMV there.
}

// Copies a fixed-width header field, trimmed: titles and serials in boot sectors.
static void header_field(const uint8_t *p, size_t n, char *out, size_t out_len) {
  size_t i;
  size_t o = 0;

  for (i = 0; i < n && o + 1 < out_len; i++) {
    out[o++] = p[i] >= 0x20 && p[i] < 0x7F ? (char)p[i] : ' ';
  }
  out[o] = '\0';
  while (o && out[o - 1] == ' ') {
    out[--o] = '\0';
  }
  {
    size_t lead = strspn(out, " ");
    memmove(out, out + lead, strlen(out + lead) + 1);
  }
}

// CD systems that say who they are in the first sector of their data track,
// before (or instead of) any file system.
static int identify_cd_header(struct rip_reader *r, const struct cd_toc *toc, struct rip_disc *d) {
  uint8_t s0[2048];
  unsigned i;

  for (i = 0; i < toc->count; i++) {
    const struct cd_track *tr = &toc->tracks[i];
    uint8_t s1[2048];
    if (cd_track_is_audio(tr)) {
      continue;
    }
    if (rip_reader_read(r, tr->start, 1, s0) != RIP_OK) {
      return 0;
    }
    if (i == 0 && !memcmp(s0, "SEGA SEGASATURN ", 16)) {
      d->kind = RIP_DISC_SATURN;
      header_field(s0 + 0x20, 10, d->title_id, sizeof(d->title_id));
      header_field(s0 + 0x60, 112, d->title, sizeof(d->title));
      return 1;
    }
    if (i == 0 && (!memcmp(s0, "SEGADISCSYSTEM", 14) || !memcmp(s0, "SEGABOOTDISC", 12))) {
      d->kind = RIP_DISC_SEGA_CD;
      header_field(s0 + 0x180 + 3, 11, d->title_id, sizeof(d->title_id));
      header_field(s0 + 0x150, 48, d->title, sizeof(d->title));
      if (!d->title[0]) {
        header_field(s0 + 0x120, 48, d->title, sizeof(d->title));
      }
      return 1;
    }
    if (!memcmp(s0, "SEGA SEGAKATANA", 15)) {
      // A GD-ROM shows a drive one session: the low-density area. A MIL-CD
      // is an ordinary CD with the game in a later session.
      d->kind = RIP_DISC_DREAMCAST;
      d->gd_rom = toc->sessions == 1;
      header_field(s0 + 0x40, 10, d->title_id, sizeof(d->title_id));
      header_field(s0 + 0x80, 128, d->title, sizeof(d->title));
      return 1;
    }
    if (i == 0 && s0[0] == 0x01 && !memcmp(s0 + 1, "ZZZZZ", 5) && s0[6] == 0x01) {
      d->kind = RIP_DISC_3DO;
      header_field(s0 + 0x28, 32, d->title, sizeof(d->title));
      return 1;
    }
    if (!memcmp(s0, "PC-FX:Hu_CD-ROM", 15)) {
      d->kind = RIP_DISC_PCFX;
      return 1;
    }
    // PC Engine CD: audio first, then a data track whose boot sector (its
    // second) names the system.
    if (i > 0 && rip_reader_read(r, tr->start + 1, 1, s1) == RIP_OK &&
        !memcmp(s1 + 0x20, "PC Engine CD-ROM SYSTEM", 23)) {
      d->kind = RIP_DISC_PCE_CD;
      return 1;
    }
    // CD-i: a Green Book volume descriptor where ISO 9660's would be. Drives
    // say so in the TOC's disc type too, but images and some drives don't.
    if (i == 0 && rip_reader_read(r, tr->start + 16, 1, s1) == RIP_OK && s1[0] == 0x01 &&
        !memcmp(s1 + 1, "CD-I ", 5)) {
      d->kind = RIP_DISC_CDI;
      header_field(s1 + 40, 32, d->title, sizeof(d->title));
      return 1;
    }
    return 0;  // the first data track decides
  }
  return 0;
}

// Most drives stop at the low-density area's lead-out; some read on into the
// high-density area, where the game is. Its first sector tells which.
static void gd_probe(struct rip_transport *t, struct rip_disc *d) {
  uint8_t frame[CD_FRAME];

  if (cd_read_frames(t, GD_HD_LBA, 1, 0, frame, NULL) == RIP_OK && !memcmp(frame + 16, "SEGA SEGAKATANA", 15)) {
    d->gd_hd_readable = 1;
    header_field(frame + 16 + 0x40, 10, d->title_id, sizeof(d->title_id));
    header_field(frame + 16 + 0x80, 128, d->title, sizeof(d->title));
  }
  RIP_INFO("GD-ROM: the high-density area is %sreadable in this drive", d->gd_hd_readable ? "" : "not ");
}

// A CD: the TOC first. All audio is a music CD, and so is an Enhanced CD
// (audio, then a data session). Data tracks are looked at for a system.
static void identify_cd(struct rip_disc *d, struct rip_reader *reader, struct rip_transport *t) {
  unsigned audio = 0;
  unsigned i;

  if (cd_read_toc(t, &d->toc) != RIP_OK) {
    RIP_WARN("this CD's TOC can't be read");
    return;
  }
  d->has_toc = 1;
  for (i = 0; i < d->toc.count; i++) {
    audio += cd_track_is_audio(&d->toc.tracks[i]);
  }
  if (audio) {
    cd_musicbrainz_id(&d->toc, d->musicbrainz_id);
  }
  if (audio == d->toc.count) {
    d->kind = RIP_DISC_CD_AUDIO;
    return;
  }
  if (audio && cd_track_is_audio(&d->toc.tracks[0]) && d->toc.sessions > 1 &&
      !cd_track_is_audio(&d->toc.tracks[d->toc.count - 1])) {
    // Music, then a data session: an Enhanced CD, or a Dreamcast MIL-CD,
    // whose data session boots the game.
    if (reader && identify_cd_header(reader, &d->toc, d) && d->kind == RIP_DISC_DREAMCAST) {
      return;
    }
    d->kind = RIP_DISC_CD_AUDIO;
    d->title[0] = '\0';
    d->title_id[0] = '\0';
    return;
  }
  if (d->toc.type == CD_TYPE_CDI) {
    d->kind = RIP_DISC_CDI;
    return;
  }
  if (reader) {
    identify_cd_header(reader, &d->toc, d);
  }
  if (d->kind == RIP_DISC_DREAMCAST && d->gd_rom) {
    gd_probe(t, d);
  }
}

// An Xbox game partition that reads as it is: XDVDFS's volume descriptor at
// sector 32 ("MICROSOFT*XBOX*MEDIA" at its start and end). The root folder
// tells the consoles apart: default.xbe for the Xbox, default.xex for the 360.
static int xdvdfs_has(const uint8_t *dir, size_t len, const char *name) {
  size_t nl = strlen(name);
  size_t i;

  // Each entry: left, right (u16), sector, size (u32), attributes, name length, name.
  for (i = 14; i + nl <= len; i++) {
    if (dir[i - 1] == nl && !strncasecmp((const char *)dir + i, name, nl)) {
      return 1;
    }
  }
  return 0;
}

static void looks_like_xiso(struct rip_reader *r, struct rip_disc *d) {
  uint8_t vd[2048];
  uint8_t *dir;
  uint32_t root;
  uint32_t size;
  uint32_t count;

  if (r->sector_size != 2048 || (r->sector_count && r->sector_count <= 33) || rip_reader_read(r, 32, 1, vd) != RIP_OK ||
      memcmp(vd, "MICROSOFT*XBOX*MEDIA", 20) != 0 || memcmp(vd + 0x7EC, "MICROSOFT*XBOX*MEDIA", 20) != 0) {
    return;
  }
  d->kind = RIP_DISC_XBOX;
  d->xbox_partition = 1;
  root = rip_le32(vd + 0x14);
  size = rip_le32(vd + 0x18);
  count = (size + 2047) / 2048;
  if (count == 0 || count > 32 || (r->sector_count && root + count > r->sector_count)) {
    return;
  }
  dir = malloc((size_t)count * 2048);
  if (dir && rip_reader_read(r, root, count, dir) == RIP_OK) {
    if (xdvdfs_has(dir, size, "default.xex")) {
      d->kind = RIP_DISC_XBOX_360;
    }
  }
  free(dir);
  RIP_INFO("Xbox game partition at sector 32: %s", rip_disc_kind_name(d->kind));
}

// Xbox and Xbox 360 discs show a stock drive only a small DVD-Video
// partition (the "put this disc in an Xbox" video); the game sits beyond it
// and needs a Kreon drive's unlock. The physical format information still
// describes the whole dual-layer disc, many times what can be read.
static void looks_like_xbox(struct rip_transport *t, struct rip_disc *d) {
  uint8_t pfi[4 + 2048];
  const uint8_t *b = pfi + 4;
  uint32_t start;
  uint32_t end;
  uint32_t end_l0;
  uint64_t physical;

  if (d->sector_count == 0 || d->sector_count > 1000000 ||
      mmc_read_disc_structure(t, 0, 0, 0, 0x00, pfi, sizeof(pfi), NULL) != RIP_OK) {
    return;
  }
  start = rip_be32(b + 4) & 0xFFFFFF;
  end = rip_be32(b + 8) & 0xFFFFFF;
  end_l0 = rip_be32(b + 12) & 0xFFFFFF;
  if (end <= start) {
    return;
  }
  if ((b[2] & 0x60) && (b[2] & 0x10) && end_l0 > start) {
    // Two layers, opposite track path: layer 1 runs back down from ~end_l0.
    physical = (uint64_t)(end_l0 - start + 1) + (end_l0 - (~end & 0xFFFFFF) + 1);
  } else {
    physical = (uint64_t)(end - start + 1);
  }
  RIP_INFO("DVD: %llu sectors readable, %llu on the disc (layers %u)", (unsigned long long)d->sector_count,
           (unsigned long long)physical, ((b[2] >> 5) & 3) + 1);
  if (physical > 1000000 && physical > d->sector_count * 8) {
    d->kind = RIP_DISC_XBOX;
    return;
  }
  // As redumper reads an XGD disc, its physical format is the video
  // partition's own: two layers, opposite track path, a few tens of MB on
  // each (an XGD2's breaks at PSN 0x0333AF). A pressed dual-layer disc puts
  // its break near the middle of gigabytes; one with under 256 MB on layer 0
  // and under 512 MB in all is what an Xbox disc shows a stock drive.
  if ((b[2] & 0x60) && (b[2] & 0x10) && end_l0 > start && end_l0 - start + 1 < 0x20000 && physical < 0x40000) {
    d->kind = RIP_DISC_XBOX;
  }
}

// Nintendo's DVDs start their physical format with FF. A GameCube's mini DVD
// holds 712,880 sectors, a Wii's more. Past this, only OmniDrive reads them.
static int32_t psn24(const uint8_t *p) {
  uint32_t v = rip_be32(p) & 0xFFFFFF;

  return (v & 0x800000u) ? (int32_t)(v | 0xFF000000u) : (int32_t)v;
}

static int layer_length(const uint8_t *b) {
  int32_t length = psn24(b + 8) + 1 - psn24(b + 4);

  if (b[2] & 0x10) {
    // Opposite track path: layer 1's end counts back down, so add both layers.
    length += 2 * (psn24(b + 12) + 1);
  }
  return length;
}

static void looks_like_nintendo(struct rip_transport *t, struct rip_disc *d) {
  uint8_t pfi[4 + 2048];
  const uint8_t *b = pfi + 4;
  int length;

  if (mmc_read_disc_structure(t, 0, 0, 0, 0x00, pfi, sizeof(pfi), NULL) != RIP_OK || b[0] != 0xFF) {
    return;
  }
  length = layer_length(b);
  if (((b[2] >> 5) & 3) && !(b[2] & 0x10) &&
      mmc_read_disc_structure(t, 0, 0, 1, 0x00, pfi, sizeof(pfi), NULL) == RIP_OK) {
    length += layer_length(b);  // parallel layers, each its own descriptor
  }
  if (length <= 0) {
    return;
  }
  d->sector_count = (uint64_t)length;
  d->sector_size = 2048;
  d->kind = length <= 712880 ? RIP_DISC_GAMECUBE : RIP_DISC_WII;
  RIP_INFO("Nintendo disc: %d sectors", length);
}

// A Wii U disc's Blu-ray disc information is all zeros.
static void looks_like_wii_u(struct rip_transport *t, struct rip_disc *d) {
  uint8_t *di = malloc(4 + 4096);
  size_t len;
  size_t i;
  int zero = 1;

  if (!di) {
    return;
  }
  if (mmc_read_disc_structure(t, 1, 0, 0, 0x00, di, 4 + 4096, NULL) == RIP_OK) {
    len = rip_be16(di) >= 2 ? rip_be16(di) - 2 : 0;
    for (i = 0; i < len && i < 4096; i++) {
      zero &= di[4 + i] == 0;
    }
    if (len && zero) {
      d->kind = RIP_DISC_WII_U;
    }
  }
  free(di);
}

// Header fields ("GALE01", "WUP-P-ARDE") and names.
static void nintendo_header(struct rip_disc *d, const uint8_t *s0) {
  if (d->kind == RIP_DISC_WII_U) {
    if (!memcmp(s0, "WUP-", 4)) {
      header_field(s0, 10, d->title_id, sizeof(d->title_id));
    }
    return;
  }
  if (rip_be32(s0 + 0x18) == 0x5D1C9EA3) {
    d->kind = RIP_DISC_WII;
  } else if (rip_be32(s0 + 0x1C) == 0xC2339F3D) {
    d->kind = RIP_DISC_GAMECUBE;
  } else {
    return;
  }
  header_field(s0, 6, d->title_id, sizeof(d->title_id));
  header_field(s0 + 0x20, 64, d->title, sizeof(d->title));
}

void rip_disc_refine(struct rip_disc *d, struct rip_drive *drive) {
  if (!drive) {
    return;
  }
  if (d->kind == RIP_DISC_XBOX && !d->xbox_partition && (drive->caps & RIP_CAP_XBOX)) {
    struct xbox_layout *x = malloc(sizeof(*x));
    if (x && xbox_read_layout(drive, x) == RIP_OK) {
      d->kind = x->version == 1 ? RIP_DISC_XBOX : RIP_DISC_XBOX_360;
      d->sector_count = x->sectors;
      xbox_relock(drive, x);
    }
    free(x);
    return;
  }
  if ((d->kind == RIP_DISC_GAMECUBE || d->kind == RIP_DISC_WII || d->kind == RIP_DISC_WII_U) &&
      (drive->caps & RIP_CAP_OMNIDRIVE)) {
    struct rip_reader r;
    uint8_t s0[2048];
    if (rip_reader_open_nintendo(&r, drive->t, d->kind == RIP_DISC_WII_U, d->sector_count ? d->sector_count : 1) ==
        RIP_OK) {
      if (rip_reader_read(&r, 0, 1, s0) == RIP_OK) {
        nintendo_header(d, s0);
      }
      rip_reader_close(&r);
    }
  }
}

int rip_disc_title_from_mount(struct rip_disc *d, const char *mount_root) {
  if (mount_root && !d->title[0]) {
    title_from_mount(mount_root, d);
  }
  return d->title[0] != '\0';
}

// A PS5 disc names itself in its second sector: "PlayStation5" at 0x400, the
// title ID ("PPSA-28038") at 0x410. Readable before the system mounts the
// disc, and in a USB Blu-ray drive, which reports it as a plain BD-ROM.
static int title_from_ps5_header(struct rip_reader *r, struct rip_disc *d) {
  uint8_t s1[2048];
  char id[16];

  if (r->sector_size != sizeof(s1) || r->sector_count < 2 || rip_reader_read(r, 1, 1, s1) != RIP_OK ||
      memcmp(s1 + 0x400, "PlayStation5", 12) != 0) {
    return 0;
  }
  header_field(s1 + 0x410, 10, id, sizeof(id));
  // As the disc's app folder and the system name it: "PPSA28038".
  if (strlen(id) == 10 && id[4] == '-') {
    snprintf(d->title_id, sizeof(d->title_id), "%.4s%.5s", id, id + 5);
  }
  return 1;
}

int rip_disc_identify(struct rip_disc *out, uint16_t profile, struct rip_reader *reader, struct rip_transport *t,
                      const char *mount_root) {
  struct iso_fs fs;

  memset(out, 0, sizeof(*out));
  out->profile = profile;
  out->kind = kind_from_profile(profile);
  if (t && mmc_profile_is_cd(profile)) {
    identify_cd(out, reader, t);
  }
  if (reader && out->kind != RIP_DISC_CD_AUDIO) {
    out->sector_size = reader->sector_size;
    out->sector_count = reader->sector_count;
    if (iso_mount(&fs, reader) == RIP_OK) {
      out->has_iso9660 = 1;
      snprintf(out->volume_id, sizeof(out->volume_id), "%s", fs.volume_id);
      identify_from_iso(&fs, out);
    }
  }
  // DVD-Video carries UDF beside ISO 9660; Blu-ray movies usually only UDF 2.50.
  if (reader && (out->kind == RIP_DISC_DVD_DATA || out->kind == RIP_DISC_BD_DATA || out->kind == RIP_DISC_DVD_VIDEO ||
                 out->kind == RIP_DISC_BD_VIDEO)) {
    struct udf_fs *udf = malloc(sizeof(*udf));
    if (udf && udf_mount(udf, reader) == RIP_OK) {
      struct udf_entry e;
      out->has_udf = 1;
      if (out->kind == RIP_DISC_DVD_DATA && udf_lookup(udf, "VIDEO_TS", &e) == RIP_OK && e.is_dir) {
        out->kind = RIP_DISC_DVD_VIDEO;
      } else if (out->kind == RIP_DISC_BD_DATA && udf_lookup(udf, "BDMV", &e) == RIP_OK && e.is_dir) {
        out->kind = RIP_DISC_BD_VIDEO;
      } else if (out->kind == RIP_DISC_BD_DATA && udf_lookup(udf, "MSXC", &e) == RIP_OK && e.is_dir) {
        out->kind = RIP_DISC_XBOX_ONE;
      }
      if (!out->volume_id[0]) {
        snprintf(out->volume_id, sizeof(out->volume_id), "%.32s",
                 udf->logical_id[0] ? udf->logical_id : udf->volume_id);
      }
    }
    free(udf);
  }
  if (t && (out->kind == RIP_DISC_DVD_VIDEO || out->kind == RIP_DISC_DVD_DATA)) {
    uint8_t cps = 0;
    if (mmc_dvd_copyright(t, &cps, NULL) == RIP_OK && cps == 1) {
      out->css = 1;
    }
  }
  if (t && mmc_profile_is_dvd(profile) && out->kind == RIP_DISC_DVD_DATA && !out->has_iso9660 && !out->has_udf) {
    looks_like_nintendo(t, out);
  }
  if (reader && (out->kind == RIP_DISC_DVD_DATA || out->kind == RIP_DISC_BD_DATA) && !out->has_iso9660 &&
      !out->has_udf) {
    looks_like_xiso(reader, out);
  }
  if (t && profile == MMC_PROFILE_DVD_ROM && !out->xbox_partition &&
      (out->kind == RIP_DISC_DVD_VIDEO || out->kind == RIP_DISC_DVD_DATA)) {
    looks_like_xbox(t, out);
  }
  if (t && mmc_profile_is_bd(profile) && out->kind == RIP_DISC_BD_DATA && !out->has_iso9660 && !out->has_udf) {
    looks_like_wii_u(t, out);
  }
  if (!out->title_id[0] && mount_root) {
    title_from_mount(mount_root, out);
  }
  // The system mounts a disc a while after the drive has it; the dump only needs the ID.
  if (!out->title_id[0] && reader && out->kind == RIP_DISC_PS5_BD) {
    title_from_ps5_header(reader, out);
  } else if (reader && out->kind == RIP_DISC_BD_DATA && title_from_ps5_header(reader, out)) {
    out->kind = RIP_DISC_PS5_BD;
  }
  if (out->has_toc) {
    RIP_INFO("CD: %u tracks in %u session(s), lead-out %u, type 0x%02x%s%s", out->toc.count, out->toc.sessions,
             out->toc.leadout, out->toc.type, out->musicbrainz_id[0] ? ", MusicBrainz " : "", out->musicbrainz_id);
  }
  RIP_INFO("disc: %s, profile %s (0x%04x), %llu x %u bytes, title '%s' (%s), volume '%s'%s",
           rip_disc_kind_name(out->kind), mmc_profile_name(profile), profile, (unsigned long long)out->sector_count,
           out->sector_size, out->title, out->title_id, out->volume_id, out->css ? ", CSS" : "");
  return RIP_OK;
}
