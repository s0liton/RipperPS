// Every disc kind ripper offers, laid out the way the real discs are, through
// the same identification a drive's disc goes through: what it is, its name
// where the disc gives one, and whether this drive (stock, Kreon, OmniDrive)
// can rip it. CDs go through the virtual drive from a .cue, so the TOC and
// the sectors are read as from a drive; DVDs and Blu-rays through the fake
// drive, which also answers custom firmware's commands.

#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "fake_drive.h"
#include "iso_builder.h"
#include "ripper/cdrom.h"
#include "ripper/common.h"
#include "ripper/disc.h"
#include "ripper/drive.h"
#include "ripper/image_drive.h"
#include "ripper/mmc.h"
#include "ripper/reader.h"
#include "ripper/rip.h"
#include "ripper/util.h"
#include "test.h"
#include "udf_builder.h"

#define SECTORS 64

/* Plans */

static const struct rip_drive *cd_drive(void) {
  static struct rip_drive d;

  d.caps = RIP_CAP_READ_CD | RIP_CAP_CD_C2 | RIP_CAP_READ_DVD;
  return &d;
}

// The PS5's own drive: DVD and Blu-ray, no CD laser.
static const struct rip_drive *ps5_drive(void) {
  static struct rip_drive d;

  d.caps = RIP_CAP_READ_DVD | RIP_CAP_READ_BD | RIP_CAP_SONY_PROFILES;
  return &d;
}

static const struct rip_drive *firmware_drive(uint32_t caps) {
  static struct rip_drive d;

  d.caps = RIP_CAP_READ_DVD | RIP_CAP_READ_BD | caps;
  return &d;
}

static int can_rip(const struct rip_drive *drive, const struct rip_disc *disc) {
  return rip_default_recipe(drive, disc) != NULL;
}

// Offered, but this drive can't: the first format says another drive is needed.
static int needs_drive(const struct rip_drive *drive, const struct rip_disc *disc) {
  struct rip_choice c[8];
  size_t n = rip_plan(drive, disc, c, RIP_ARRAY_LEN(c));

  return n > 0 && c[0].avail == RIP_AVAIL_NEEDS_DRIVE && !can_rip(drive, disc);
}

/* CDs, through the virtual drive */

static void make_mode2(uint8_t s[CD_FRAME], uint32_t lba) {
  uint32_t f = lba + CD_PREGAP;
  uint8_t m = (uint8_t)(f / (60 * CD_FRAMES_PER_SEC));
  uint8_t sec = (uint8_t)((f / CD_FRAMES_PER_SEC) % 60);
  uint8_t fr = (uint8_t)(f % CD_FRAMES_PER_SEC);

  // Sync, the header in BCD, mode 2, and an XA form 1 subheader twice.
  s[0] = 0;
  memset(s + 1, 0xFF, 10);
  s[11] = 0;
  s[12] = (uint8_t)(((m / 10) << 4) | (m % 10));
  s[13] = (uint8_t)(((sec / 10) << 4) | (sec % 10));
  s[14] = (uint8_t)(((fr / 10) << 4) | (fr % 10));
  s[15] = 2;
  memset(s + 16, 0, 8);
  s[18] = s[22] = 0x08;
}

// 2048-byte sectors as a data track's raw frames, mode 1 or XA mode 2.
static uint8_t *data_track(const uint8_t *sectors, size_t count, int mode2) {
  uint8_t *bin = calloc(count, CD_FRAME);
  size_t i;

  for (i = 0; bin && i < count; i++) {
    uint8_t *s = bin + i * CD_FRAME;
    if (mode2) {
      make_mode2(s, (uint32_t)i);
      memcpy(s + 24, sectors + i * 2048, 2048);
    } else {
      memcpy(s + 16, sectors + i * 2048, 2048);
      cd_make_mode1(s, (uint32_t)i);
    }
  }
  return bin;
}

static uint8_t *audio_track(size_t frames) {
  uint8_t *bin = malloc(frames * CD_FRAME);
  size_t i;

  for (i = 0; bin && i < frames * CD_FRAME; i++) {
    bin[i] = (uint8_t)(i * 31 + 7);
  }
  return bin;
}

struct cd_track_spec {
  const char *mode;  // AUDIO, MODE1/2352, MODE2/2352, CDI/2352
  const uint8_t *bin;
  size_t frames;
  unsigned session;  // 0 or 1: the first
};

static int write_bytes(const char *path, const uint8_t *data, size_t len) {
  FILE *f = fopen(path, "wb");
  int ok = f && fwrite(data, 1, len, f) == len;

  if (f) {
    fclose(f);
  }
  return ok ? 0 : -1;
}

// A Redump-style sheet: one .bin per track, REM SESSION before a new session.
static void write_cue(const char *dir, const char *stem, const struct cd_track_spec *t, size_t n, char *cue,
                      size_t cn) {
  char text[2048];
  size_t at = 0;
  size_t i;

  for (i = 0; i < n; i++) {
    char path[600];
    unsigned s = t[i].session ? t[i].session : 1;
    snprintf(path, sizeof(path), "%s/%s-%02zu.bin", dir, stem, i + 1);
    CHECK(write_bytes(path, t[i].bin, t[i].frames * CD_FRAME) == 0);
    if (s > 1 && (i == 0 || (t[i - 1].session ? t[i - 1].session : 1) != s)) {
      at += (size_t)snprintf(text + at, sizeof(text) - at, "REM SESSION %02u\r\n", s);
    }
    at += (size_t)snprintf(text + at, sizeof(text) - at,
                           "FILE \"%s-%02zu.bin\" BINARY\r\n  TRACK %02zu %s\r\n    INDEX 01 00:00:00\r\n", stem, i + 1,
                           i + 1, t[i].mode);
  }
  snprintf(cue, cn, "%s/%s.cue", dir, stem);
  CHECK(write_bytes(cue, (const uint8_t *)text, at) == 0);
}

static int identify_cue(const char *cue, struct rip_disc *disc) {
  struct rip_transport t;
  struct rip_reader r;
  int have;

  memset(disc, 0, sizeof(*disc));
  if (rip_image_drive_open(&t, NULL) != RIP_OK) {
    return RIP_ERR;
  }
  if (rip_image_drive_load(&t, cue) != RIP_OK) {
    rip_transport_close(&t);
    return RIP_ERR;
  }
  have = rip_reader_open_mmc(&r, &t) == RIP_OK;
  rip_disc_identify(disc, MMC_PROFILE_CD_ROM, have ? &r : NULL, &t, NULL);
  if (have) {
    rip_reader_close(&r);
  }
  rip_transport_close(&t);
  return RIP_OK;
}

// One data track from sectors; identified, and checked to rip on a CD drive
// and to need one on the PS5's.
static void check_cd(const char *dir, const char *stem, const uint8_t *sectors, size_t count, const char *mode,
                     enum rip_disc_kind want, struct rip_disc *disc) {
  uint8_t *bin = data_track(sectors, count, strcmp(mode, "MODE1/2352") != 0);
  struct cd_track_spec t[1] = {{mode, bin, count, 0}};
  char cue[600];

  write_cue(dir, stem, t, 1, cue, sizeof(cue));
  CHECK(identify_cue(cue, disc) == RIP_OK);
  if (disc->kind != want) {
    fprintf(stderr, "  %s: identified as %s\n", stem, rip_disc_kind_name(disc->kind));
  }
  CHECK(disc->kind == want);
  CHECK(can_rip(cd_drive(), disc));
  CHECK(needs_drive(ps5_drive(), disc));
  free(bin);
}

static void test_playstation_cds(const char *dir) {
  const char ps1[] = "BOOT = cdrom:\\SCUS_944.91;1\r\nTCB = 4\r\n";
  const char ps2[] = "BOOT2 = cdrom0:\\SLUS_200.62;1\r\nVER = 1.00\r\n";
  struct isob_file f1[] = {{"SYSTEM.CNF", (const uint8_t *)ps1, sizeof(ps1) - 1, 0}};
  struct isob_file f2[] = {{"SYSTEM.CNF", (const uint8_t *)ps2, sizeof(ps2) - 1, 0}};
  struct isob_file f3[] = {{"PSX.EXE", (const uint8_t *)"PS-X EXE", 8, 0}};
  struct rip_disc disc;
  size_t len;
  uint8_t *img;

  // PS1 and PS2 CDs are XA mode 2, their serial in SYSTEM.CNF.
  img = isob_build(f1, 1, SECTORS, "SCUS_94491", &len);
  check_cd(dir, "ps1", img, SECTORS, "MODE2/2352", RIP_DISC_PS1, &disc);
  CHECK_STR(disc.title_id, "SCUS-94491");
  free(img);
  img = isob_build(f2, 1, SECTORS, "SLUS_20062", &len);
  check_cd(dir, "ps2cd", img, SECTORS, "MODE2/2352", RIP_DISC_PS2_CD, &disc);
  CHECK_STR(disc.title_id, "SLUS-20062");
  free(img);
  // Early PS1 discs boot PSX.EXE with no SYSTEM.CNF.
  img = isob_build(f3, 1, SECTORS, "PSX", &len);
  check_cd(dir, "ps1exe", img, SECTORS, "MODE2/2352", RIP_DISC_PS1, &disc);
  free(img);
}

// Sega CD, 3DO: their system header is the data track's first sector.
static void test_header_cds(const char *dir) {
  uint8_t *img = calloc(SECTORS, 2048);
  struct rip_disc disc;

  // Sega CD: "SEGADISCSYSTEM", the overseas title at 0x150, "GM <serial>" at 0x180.
  memcpy(img, "SEGADISCSYSTEM  SEGA          01", 32);
  memcpy(img + 0x100, "SEGA GENESIS    ", 16);
  memset(img + 0x150, ' ', 48);
  memcpy(img + 0x150, "SONIC THE HEDGEHOG CD", 21);
  memcpy(img + 0x180, "GM MK-4407 -00", 14);
  check_cd(dir, "segacd", img, SECTORS, "MODE1/2352", RIP_DISC_SEGA_CD, &disc);
  CHECK_STR(disc.title, "SONIC THE HEDGEHOG CD");
  CHECK_STR(disc.title_id, "MK-4407 -00");

  // 3DO: the Opera file system's volume header, its label at 0x28.
  memset(img, 0, (size_t)SECTORS * 2048);
  img[0] = 0x01;
  memset(img + 1, 0x5A, 5);
  img[6] = 0x01;
  memcpy(img + 0x28, "CD-ROM", 6);
  check_cd(dir, "3do", img, SECTORS, "MODE1/2352", RIP_DISC_3DO, &disc);
  CHECK_STR(disc.title, "CD-ROM");

  // CD-i: a Green Book volume descriptor at sector 16, on a mode 2 track
  // some tools call CDI/2352. Its label sits where ISO 9660's would.
  memset(img, 0, (size_t)SECTORS * 2048);
  img[16 * 2048] = 0x01;
  memcpy(img + 16 * 2048 + 1, "CD-I \001", 6);
  memcpy(img + 16 * 2048 + 40, "THE APPRENTICE", 14);
  check_cd(dir, "cdi", img, SECTORS, "CDI/2352", RIP_DISC_CDI, &disc);
  CHECK_STR(disc.title, "THE APPRENTICE");
  free(img);
}

// PC Engine CD and PC-FX: a warning in audio first, then the data track.
static void test_nec_cds(const char *dir) {
  uint8_t *img = calloc(SECTORS, 2048);
  uint8_t *audio = audio_track(300);
  uint8_t *bin;
  struct cd_track_spec t[2];
  struct rip_disc disc;
  char cue[600];

  // PC Engine CD: the data track's second sector names the system at 0x20.
  memcpy(img + 2048 + 0x20, "PC Engine CD-ROM SYSTEM", 23);
  bin = data_track(img, SECTORS, 0);
  t[0] = (struct cd_track_spec){"AUDIO", audio, 300, 0};
  t[1] = (struct cd_track_spec){"MODE1/2352", bin, SECTORS, 0};
  write_cue(dir, "pce", t, 2, cue, sizeof(cue));
  CHECK(identify_cue(cue, &disc) == RIP_OK);
  CHECK(disc.kind == RIP_DISC_PCE_CD);
  CHECK(can_rip(cd_drive(), &disc));
  free(bin);

  // PC-FX: "PC-FX:Hu_CD-ROM" at the start of the data track (Mednafen's check).
  memset(img, 0, (size_t)SECTORS * 2048);
  memcpy(img, "PC-FX:Hu_CD-ROM ", 16);
  bin = data_track(img, SECTORS, 0);
  t[1].bin = bin;
  write_cue(dir, "pcfx", t, 2, cue, sizeof(cue));
  CHECK(identify_cue(cue, &disc) == RIP_OK);
  CHECK(disc.kind == RIP_DISC_PCFX);
  CHECK(can_rip(cd_drive(), &disc));
  free(bin);
  free(audio);
  free(img);
}

// Neo Geo CD and Video CDs: ISO 9660, told apart by their files.
static void test_iso_cds(const char *dir) {
  struct isob_file neo[] = {{"IPL.TXT", (const uint8_t *)"PROG.PRG,0,0\r\n", 14, 0}};
  struct isob_file vcd[] = {{"VCD/INFO.VCD", (const uint8_t *)"VIDEO_CD", 8, 0},
                            {"MPEGAV/AVSEQ01.DAT", (const uint8_t *)"RIFF", 4, 0}};
  struct isob_file svcd[] = {{"SVCD/INFO.SVD", (const uint8_t *)"SUPERVCD", 8, 0},
                             {"MPEG2/AVSEQ01.MPG", (const uint8_t *)"\0\0\001\272", 4, 0}};
  struct rip_disc disc;
  size_t len;
  uint8_t *img;

  img = isob_build(neo, 1, SECTORS, "NEOGEO", &len);
  check_cd(dir, "neogeo", img, SECTORS, "MODE1/2352", RIP_DISC_NEOGEO_CD, &disc);
  free(img);
  img = isob_build(vcd, 2, SECTORS, "VIDEOCD", &len);
  check_cd(dir, "vcd", img, SECTORS, "MODE2/2352", RIP_DISC_VIDEO_CD, &disc);
  free(img);
  img = isob_build(svcd, 2, SECTORS, "SUPERVCD", &len);
  check_cd(dir, "svcd", img, SECTORS, "MODE2/2352", RIP_DISC_VIDEO_CD, &disc);
  free(img);
}

static void katana(uint8_t *s) {
  memcpy(s, "SEGA SEGAKATANA SEGA ENTERPRISES", 32);
  memcpy(s + 0x40, "MK-51000  ", 10);
  memset(s + 0x80, ' ', 128);
  memcpy(s + 0x80, "SONIC ADVENTURE", 15);
}

// Dreamcast. A GD-ROM shows a stock drive one session, the low-density area,
// with IP.BIN's copy at its start; the game is in the high-density area from
// LBA 45000, which only some drives read on into. A MIL-CD is an ordinary CD:
// music first, the game in a second session.
static void test_dreamcast(const char *dir) {
  enum { HD = 45000, FRAMES = HD + 16 };
  uint8_t sector[2048];
  uint8_t frame[CD_FRAME];
  uint8_t *ld = calloc(SECTORS, 2048);
  uint8_t *audio = audio_track(300);
  uint8_t *bin;
  struct cd_track_spec t[2];
  struct rip_disc disc;
  char path[600];
  char cue[600];
  int fd;

  // A drive that reads on: one track running into the high-density area,
  // written sparse so the 100 MB in between take no room.
  snprintf(path, sizeof(path), "%s/gdrom-01.bin", dir);
  fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
  CHECK(fd >= 0);
  if (fd >= 0) {
    memset(sector, 0, sizeof(sector));
    katana(sector);
    memcpy(frame + 16, sector, 2048);
    cd_make_mode1(frame, 0);
    CHECK(pwrite(fd, frame, CD_FRAME, 0) == CD_FRAME);
    memcpy(frame + 16, sector, 2048);
    cd_make_mode1(frame, HD);
    CHECK(pwrite(fd, frame, CD_FRAME, (off_t)HD * CD_FRAME) == CD_FRAME);
    CHECK(ftruncate(fd, (off_t)FRAMES * CD_FRAME) == 0);
    close(fd);
  }
  snprintf(cue, sizeof(cue), "%s/gdrom.cue", dir);
  {
    const char text[] = "FILE \"gdrom-01.bin\" BINARY\r\n  TRACK 01 MODE1/2352\r\n    INDEX 01 00:00:00\r\n";
    CHECK(write_bytes(cue, (const uint8_t *)text, sizeof(text) - 1) == 0);
  }
  CHECK(identify_cue(cue, &disc) == RIP_OK);
  CHECK(disc.kind == RIP_DISC_DREAMCAST && disc.gd_rom && disc.gd_hd_readable);
  CHECK_STR(disc.title_id, "MK-51000");
  CHECK_STR(disc.title, "SONIC ADVENTURE");
  CHECK(can_rip(cd_drive(), &disc));

  // A drive that stops at the low-density lead-out: data, then the warning in audio.
  katana(ld);
  bin = data_track(ld, SECTORS, 0);
  t[0] = (struct cd_track_spec){"MODE1/2352", bin, SECTORS, 0};
  t[1] = (struct cd_track_spec){"AUDIO", audio, 300, 0};
  write_cue(dir, "gdld", t, 2, cue, sizeof(cue));
  CHECK(identify_cue(cue, &disc) == RIP_OK);
  CHECK(disc.kind == RIP_DISC_DREAMCAST && disc.gd_rom && !disc.gd_hd_readable);
  CHECK(needs_drive(cd_drive(), &disc));

  // A MIL-CD: not an Enhanced CD, though it looks like one from its TOC.
  t[0] = (struct cd_track_spec){"AUDIO", audio, 300, 1};
  t[1] = (struct cd_track_spec){"MODE2/2352", NULL, SECTORS, 2};
  free(bin);
  bin = data_track(ld, SECTORS, 1);
  t[1].bin = bin;
  write_cue(dir, "milcd", t, 2, cue, sizeof(cue));
  CHECK(identify_cue(cue, &disc) == RIP_OK);
  CHECK(disc.kind == RIP_DISC_DREAMCAST && !disc.gd_rom);
  CHECK(can_rip(cd_drive(), &disc));

  // And an Enhanced CD stays a music CD: its data session is no game.
  free(bin);
  {
    struct isob_file f[] = {{"README.TXT", (const uint8_t *)"bonus", 5, 0}};
    size_t len;
    uint8_t *img = isob_build(f, 1, SECTORS, "BONUS", &len);
    bin = data_track(img, SECTORS, 1);
    free(img);
  }
  t[1].bin = bin;
  write_cue(dir, "enhanced", t, 2, cue, sizeof(cue));
  CHECK(identify_cue(cue, &disc) == RIP_OK);
  CHECK(disc.kind == RIP_DISC_CD_AUDIO);
  free(bin);
  free(audio);
  free(ld);
}

/* DVDs and Blu-rays, through the fake drive */

struct rig {
  struct fake_drive fake;
  struct rip_transport t;
  struct rip_drive drive;
  struct rip_reader reader;
  struct rip_disc disc;
};

static void rig_init(struct rig *g, const uint8_t *img, size_t len, uint16_t profile) {
  memset(g, 0, sizeof(*g));
  g->fake.image = img;
  g->fake.image_len = len;
  g->fake.profile = profile;
  g->fake.dvd = 1;
  g->fake.bd = 1;
  g->fake.fail_lba = -1;
}

// Probes the drive as set up and identifies its disc.
static int rig_go(struct rig *g) {
  uint16_t current = 0;

  fake_drive_transport(&g->fake, &g->t);
  if (rip_drive_probe(&g->drive, &g->t, g->fake.sony ? RIP_DRIVE_INTERNAL : RIP_DRIVE_EXTERNAL) != RIP_OK ||
      rip_drive_media(&g->drive, &current) != RIP_MEDIA_PRESENT || current != g->fake.profile ||
      rip_reader_open_mmc(&g->reader, &g->t) != RIP_OK) {
    return RIP_ERR;
  }
  rip_disc_identify(&g->disc, current, &g->reader, &g->t, NULL);
  rip_disc_refine(&g->disc, &g->drive);
  return RIP_OK;
}

static void rig_done(struct rig *g) {
  rip_reader_close(&g->reader);
}

static void identify_fs(const uint8_t *img, size_t len, uint16_t profile, enum rip_disc_kind want) {
  struct rig g;

  rig_init(&g, img, len, profile);
  CHECK(rig_go(&g) == RIP_OK);
  if (g.disc.kind != want) {
    fprintf(stderr, "  wanted %s, identified as %s\n", rip_disc_kind_name(want), rip_disc_kind_name(g.disc.kind));
  }
  CHECK(g.disc.kind == want);
  CHECK(can_rip(&g.drive, &g.disc));
  rig_done(&g);
}

static void test_video_and_data(void) {
  static const uint8_t ifo[] = "DVDVIDEO-VMG";
  struct isob_file dvd_iso[] = {{"VIDEO_TS/VIDEO_TS.IFO", ifo, sizeof(ifo), 0}};
  struct udfb_file dvd_udf[] = {{"VIDEO_TS/VIDEO_TS.IFO", ifo, sizeof(ifo)}};
  struct udfb_file bd_udf[] = {{"BDMV/index.bdmv", (const uint8_t *)"INDX0200", 8}};
  struct udfb_file bd_data[] = {{"PHOTOS/IMG_0001.JPG", (const uint8_t *)"\377\330\377", 3}};
  struct udfb_file xbox_one[] = {{"MSXC/Metadata/package.xml", (const uint8_t *)"<xml/>", 6}};
  struct isob_file data[] = {{"SETUP.EXE", (const uint8_t *)"MZ", 2, 0}};
  size_t len;
  uint8_t *img;

  img = isob_build(dvd_iso, 1, SECTORS, "MOVIE", &len);
  identify_fs(img, len, MMC_PROFILE_DVD_ROM, RIP_DISC_DVD_VIDEO);
  free(img);
  // Most DVD-Video carries ISO 9660 beside UDF; some only UDF 1.02.
  img = udfb_build(dvd_udf, 1, 0, "MOVIE", &len);
  identify_fs(img, len, MMC_PROFILE_DVD_ROM, RIP_DISC_DVD_VIDEO);
  free(img);
  // Blu-ray movies: UDF 2.50 only, no ISO 9660.
  img = udfb_build(bd_udf, 1, 1, "MOVIE", &len);
  identify_fs(img, len, MMC_PROFILE_BD_ROM, RIP_DISC_BD_VIDEO);
  free(img);
  img = udfb_build(bd_data, 1, 1, "BACKUP", &len);
  identify_fs(img, len, MMC_PROFILE_BD_ROM, RIP_DISC_BD_DATA);
  free(img);
  img = isob_build(data, 1, SECTORS, "PCGAME", &len);
  identify_fs(img, len, MMC_PROFILE_DVD_ROM, RIP_DISC_DVD_DATA);
  free(img);
  // Xbox One and Series discs: UDF with the game packages in MSXC.
  img = udfb_build(xbox_one, 1, 1, "XBOXONE", &len);
  identify_fs(img, len, MMC_PROFILE_BD_ROM, RIP_DISC_XBOX_ONE);
  free(img);
}

static void test_playstation_discs(void) {
  struct isob_file ps3[] = {{"PS3_DISC.SFB", (const uint8_t *)".SFB", 4, 0}};
  struct rig g;
  size_t len;
  uint8_t *img;

  // A PS3 game on DVD (a few were), in a PC drive.
  img = isob_build(ps3, 1, SECTORS, "PS3VOLUME", &len);
  identify_fs(img, len, MMC_PROFILE_DVD_ROM, RIP_DISC_PS3_DVD);
  free(img);

  // PS4 and PS5 discs in the PS5's drive: told by its vendor profiles.
  img = calloc(SECTORS, 2048);
  rig_init(&g, img, (size_t)SECTORS * 2048, MMC_PROFILE_PS4_BD);
  g.fake.sony = 1;
  CHECK(rig_go(&g) == RIP_OK);
  CHECK(g.disc.kind == RIP_DISC_PS4_BD);
  CHECK(can_rip(&g.drive, &g.disc));
  rig_done(&g);

  // A PS5 disc in a USB Blu-ray drive: a plain BD-ROM to it, named by its second sector.
  memcpy(img + 2048 + 0x400, "PlayStation5", 12);
  memcpy(img + 2048 + 0x410, "PPSA-01302                      ", 32);
  rig_init(&g, img, (size_t)SECTORS * 2048, MMC_PROFILE_BD_ROM);
  CHECK(rig_go(&g) == RIP_OK);
  CHECK(g.disc.kind == RIP_DISC_PS5_BD);
  CHECK_STR(g.disc.title_id, "PPSA01302");
  rig_done(&g);
  free(img);
}

static void put24(uint8_t *p, uint32_t v) {
  p[0] = (uint8_t)(v >> 16);
  p[1] = (uint8_t)(v >> 8);
  p[2] = (uint8_t)v;
}

// A layer descriptor: book type, layers and track path in byte 2, then the
// first sector, the last, and layer 0's last (PSNs, 24 bits each).
static void layer_descriptor(uint8_t p[2048], uint8_t b0, uint8_t b2, uint32_t first, uint32_t last, uint32_t l0) {
  memset(p, 0, 2048);
  p[0] = b0;
  p[2] = b2;
  put24(p + 5, first);
  put24(p + 9, last);
  put24(p + 13, l0);
}

// The security sector of an XGD2 disc, as tests/test_firmware.c makes it up.
static void xgd2_security_sector(uint8_t ss[2048]) {
  uint8_t *e = ss + 0x661;

  memset(ss, 0, 2048);
  ss[2] = 0x31;
  put24(ss + 5, 0x04FB20);
  put24(ss + 9, 0xFB04DF);
  put24(ss + 13, 2110367);
  ss[0x660] = 4;
  put24(e + 3, 0x060600);
  put24(e + 6, 0x0607FF);
  put24(e + 27 + 3, 0xFBFFFF - 0x200);
  put24(e + 27 + 6, 0xFBFFFF);
}

// Xbox and Xbox 360 discs on a stock drive: only the video partition reads,
// and the physical format describes just that partition (redumper reads it
// so: an XGD2's runs to PSN 0x0333AF on layer 0). Kreon unlocks the drive and
// hands out the security sector, which tells the two apart.
static void test_xbox(void) {
  struct isob_file video[] = {{"README.TXT", (const uint8_t *)"Put this disc in an Xbox", 24, 0}};
  uint8_t pfi[2048];
  uint8_t ss[2048];
  struct rig g;
  size_t len;
  uint8_t *img = isob_build(video, 1, SECTORS, "XBOX", &len);

  layer_descriptor(pfi, 0x01, 0x31, 0x030000, 0xFCE60F, 0x0333AF);
  xgd2_security_sector(ss);

  rig_init(&g, img, len, MMC_PROFILE_DVD_ROM);
  g.fake.pfi[0] = pfi;
  CHECK(rig_go(&g) == RIP_OK);
  CHECK(g.disc.kind == RIP_DISC_XBOX && !g.disc.xbox_partition);
  CHECK(needs_drive(&g.drive, &g.disc));
  CHECK(can_rip(firmware_drive(RIP_CAP_KREON | RIP_CAP_XBOX), &g.disc));
  rig_done(&g);

  rig_init(&g, img, len, MMC_PROFILE_DVD_ROM);
  g.fake.pfi[0] = pfi;
  g.fake.kreon = 1;
  g.fake.xbox_ss = ss;
  CHECK(rig_go(&g) == RIP_OK);
  CHECK(g.drive.caps & RIP_CAP_KREON);
  CHECK(g.disc.kind == RIP_DISC_XBOX_360);  // XGD2
  CHECK(g.disc.sector_count == 3820880);
  CHECK(!g.fake.unlocked);  // locked again after
  CHECK(can_rip(&g.drive, &g.disc));
  rig_done(&g);

  // An ordinary DVD whose physical format matches what reads isn't one.
  layer_descriptor(pfi, 0x01, 0x01, 0x030000, 0x030000 + SECTORS - 1, 0);
  rig_init(&g, img, len, MMC_PROFILE_DVD_ROM);
  g.fake.pfi[0] = pfi;
  CHECK(rig_go(&g) == RIP_OK);
  CHECK(g.disc.kind == RIP_DISC_DVD_DATA);
  rig_done(&g);
  free(img);
}

// XDVDFS at sector 32: a game partition that reads as it is, as an image or
// a burned backup. The root folder's default.xbe or default.xex says which console.
static uint8_t *xiso(const char *exe) {
  uint8_t *img = calloc(SECTORS, 2048);
  uint8_t *vd = img + 32 * 2048;
  uint8_t *dir = img + 40 * 2048;

  memcpy(vd, "MICROSOFT*XBOX*MEDIA", 20);
  rip_put_le32(vd + 0x14, 40);
  rip_put_le32(vd + 0x18, 2048);
  memcpy(vd + 0x7EC, "MICROSOFT*XBOX*MEDIA", 20);
  // left, right, sector, size, attributes, name length, name.
  rip_put_le32(dir + 4, 41);
  rip_put_le32(dir + 8, 1000);
  dir[12] = 0x20;
  dir[13] = (uint8_t)strlen(exe);
  memcpy(dir + 14, exe, strlen(exe));
  memset(dir + 14 + strlen(exe), 0xFF, 2048 - 14 - strlen(exe));
  memset(img + 41 * 2048, 0xAB, 1000);
  return img;
}

static void test_xiso(void) {
  size_t len = (size_t)SECTORS * 2048;
  uint8_t *img = xiso("default.xbe");
  struct rig g;
  struct rip_job_opts opts;
  struct rip_job_result res;
  const struct rip_recipe *recipe;
  char dir[64];
  size_t got = 0;
  uint8_t *out;

  rig_init(&g, img, len, MMC_PROFILE_DVD_ROM);
  CHECK(rig_go(&g) == RIP_OK);
  CHECK(g.disc.kind == RIP_DISC_XBOX && g.disc.xbox_partition);
  // No unlock needed: the stock drive rips it. As an XISO (the default) it's
  // cut after its last file; as an ISO it's the disc sector for sector.
  recipe = rip_default_recipe(&g.drive, &g.disc);
  CHECK(recipe && recipe->id == RIP_RECIPE_XISO);
  {
    struct rip_choice choices[8];
    size_t n = rip_plan(&g.drive, &g.disc, choices, 8);
    size_t i;
    for (i = 0; i < n; i++) {
      int whole = choices[i].recipe->id == RIP_RECIPE_ISO;
      test_tmpdir(dir, sizeof(dir));
      memset(&opts, 0, sizeof(opts));
      opts.out_dir = dir;
      opts.name = "Halo";
      CHECK(rip_run(choices[i].recipe, &g.drive, &g.reader, &g.disc, &opts, &res) == RIP_OK);
      out = test_read_file(res.image_path, &got);
      CHECK(out && (whole ? got == len : got > 33 * 2048 && got < len) && !memcmp(out, img, got));
      free(out);
      remove(res.image_path);
      remove(res.manifest_path);
      rmdir(dir);
    }
    CHECK(n == 2);
  }
  rig_done(&g);
  free(img);

  img = xiso("default.xex");
  rig_init(&g, img, len, MMC_PROFILE_DVD_ROM);
  CHECK(rig_go(&g) == RIP_OK);
  CHECK(g.disc.kind == RIP_DISC_XBOX_360 && g.disc.xbox_partition);
  CHECK(can_rip(ps5_drive(), &g.disc));
  rig_done(&g);
  free(img);
}

// Nintendo's discs: a physical format whose first byte is FF, a GameCube's
// 712,880 sectors at most. Stock drives read none of it; OmniDrive's raw
// read does, and the disc's header then names the game.
static void test_nintendo(void) {
  uint8_t pfi[2][2048];
  uint8_t *img = calloc(SECTORS, 2048);
  struct rig g;

  // GameCube: "GALE01", the magic at 0x1C, the title at 0x20.
  memcpy(img, "GALE01", 6);
  rip_put_be32(img + 0x1C, 0xC2339F3D);
  memcpy(img + 0x20, "Super Smash Bros Melee", 22);
  layer_descriptor(pfi[0], 0xFF, 0x00, 0x030000, 0x030000 + 712880 - 1, 0);
  rig_init(&g, img, (size_t)SECTORS * 2048, MMC_PROFILE_DVD_ROM);
  g.fake.pfi[0] = pfi[0];
  CHECK(rig_go(&g) == RIP_OK);
  CHECK(g.disc.kind == RIP_DISC_GAMECUBE && g.disc.sector_count == 712880);
  CHECK(needs_drive(&g.drive, &g.disc));
  rig_done(&g);
  rig_init(&g, img, (size_t)SECTORS * 2048, MMC_PROFILE_DVD_ROM);
  g.fake.pfi[0] = pfi[0];
  g.fake.omnidrive = 1;
  g.fake.nintendo = 1;
  CHECK(rig_go(&g) == RIP_OK);
  CHECK(g.drive.caps & RIP_CAP_OMNIDRIVE);
  CHECK(g.disc.kind == RIP_DISC_GAMECUBE);
  CHECK_STR(g.disc.title_id, "GALE01");
  CHECK_STR(g.disc.title, "Super Smash Bros Melee");
  CHECK(can_rip(&g.drive, &g.disc));
  rig_done(&g);

  // Wii, dual layer: parallel track path, so each layer has its own descriptor.
  memset(img, 0, 2048);
  memcpy(img, "RMCE01", 6);
  rip_put_be32(img + 0x18, 0x5D1C9EA3);
  memcpy(img + 0x20, "MARIO KART WII", 14);
  layer_descriptor(pfi[0], 0xFF, 0x20, 0x030000, 0x030000 + 2077920 - 1, 0);
  layer_descriptor(pfi[1], 0xFF, 0x20, 0x030000, 0x030000 + 2077920 - 1, 0);
  rig_init(&g, img, (size_t)SECTORS * 2048, MMC_PROFILE_DVD_ROM);
  g.fake.pfi[0] = pfi[0];
  g.fake.pfi[1] = pfi[1];
  CHECK(rig_go(&g) == RIP_OK);
  CHECK(g.disc.kind == RIP_DISC_WII && g.disc.sector_count == 2 * 2077920);
  CHECK(needs_drive(&g.drive, &g.disc));
  rig_done(&g);
  rig_init(&g, img, (size_t)SECTORS * 2048, MMC_PROFILE_DVD_ROM);
  g.fake.pfi[0] = pfi[0];
  g.fake.pfi[1] = pfi[1];
  g.fake.omnidrive = 1;
  g.fake.nintendo = 1;
  CHECK(rig_go(&g) == RIP_OK);
  CHECK(g.disc.kind == RIP_DISC_WII);
  CHECK_STR(g.disc.title_id, "RMCE01");
  CHECK_STR(g.disc.title, "MARIO KART WII");
  CHECK(can_rip(&g.drive, &g.disc));
  rig_done(&g);

  // Wii U: a Blu-ray whose disc information is all zeros, "WUP-P-ARDE" in sector 0.
  memset(img, 0, 2048);
  memcpy(img, "WUP-P-ARDE", 10);
  {
    static const uint8_t zeros[4096];
    rig_init(&g, img, (size_t)SECTORS * 2048, MMC_PROFILE_BD_ROM);
    g.fake.bd_di = zeros;
    g.fake.bd_di_len = sizeof(zeros);
    CHECK(rig_go(&g) == RIP_OK);
    CHECK(g.disc.kind == RIP_DISC_WII_U);
    CHECK(needs_drive(&g.drive, &g.disc));
    rig_done(&g);
    rig_init(&g, img, (size_t)SECTORS * 2048, MMC_PROFILE_BD_ROM);
    g.fake.bd_di = zeros;
    g.fake.bd_di_len = sizeof(zeros);
    g.fake.omnidrive = 1;
    g.fake.nintendo = 2;
    CHECK(rig_go(&g) == RIP_OK);
    CHECK(g.disc.kind == RIP_DISC_WII_U);
    CHECK_STR(g.disc.title_id, "WUP-P-ARDE");
    CHECK(can_rip(&g.drive, &g.disc));
    rig_done(&g);
  }
  free(img);
}

void test_identify(void) {
  char dir[64];
  char cmd[128];

  test_tmpdir(dir, sizeof(dir));
  test_playstation_cds(dir);
  test_header_cds(dir);
  test_nec_cds(dir);
  test_iso_cds(dir);
  test_dreamcast(dir);
  test_video_and_data();
  test_playstation_discs();
  test_xbox();
  test_xiso();
  test_nintendo();
  snprintf(cmd, sizeof(cmd), "rm -rf '%s'", dir);
  if (system(cmd) != 0) {
    fprintf(stderr, "could not remove %s\n", dir);
  }
}
