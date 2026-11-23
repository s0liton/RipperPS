// Disc formats: CD-ROM ECC, CUE sheets, the image drive's CD side, and CHD
// images checked by MAME's own chdman when it is installed.

#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "../src/core/ufs2_layout.h"
#include "ripper/audio_out.h"
#include "ripper/cdrom.h"
#include "ripper/chd.h"
#include "ripper/common.h"
#include "ripper/cue.h"
#include "ripper/disc.h"
#include "ripper/drive.h"
#include "ripper/exfat.h"
#include "ripper/hash.h"
#include "ripper/image_drive.h"
#include "ripper/mmc.h"
#include "ripper/pkg.h"
#include "ripper/reader.h"
#include "ripper/rip.h"
#include "ripper/sce_elf.h"
#include "ripper/udf.h"
#include "ripper/ufs2.h"
#include "ripper/util.h"
#include "test.h"
#include "udf_builder.h"

static void fill_random(uint8_t *p, size_t n, uint32_t seed) {
  size_t i;

  for (i = 0; i < n; i++) {
    seed = seed * 1103515245u + 12345u;
    p[i] = (uint8_t)(seed >> 16);
  }
}

static int have_chdman(void) {
  return system("chdman >/dev/null 2>&1") != 127 && system("command -v chdman >/dev/null 2>&1") == 0;
}

static int run(const char *fmt, const char *a, const char *b) {
  char cmd[2048];

  snprintf(cmd, sizeof(cmd), fmt, a, b);
  return system(cmd);
}

static int write_file(const char *path, const uint8_t *data, size_t len) {
  FILE *f = fopen(path, "wb");
  int ok = f && fwrite(data, 1, len, f) == len;

  if (f) {
    fclose(f);
  }
  return ok ? 0 : -1;
}

static void test_ecc(void) {
  uint8_t s[CD_FRAME];

  memset(s, 0, sizeof(s));
  fill_random(s + 16, CD_DATA, 7);
  cd_make_mode1(s, 1234);
  CHECK(cd_sector_ok(s) == 1);
  CHECK(cd_ecc_verify(s));
  s[100] ^= 1;
  CHECK(cd_sector_ok(s) == 0);
  CHECK(!cd_ecc_verify(s));
  // MSF 00:18:34 for LBA 1234 (+150 = 1384 frames)
  s[100] ^= 1;
  CHECK(s[12] == 0x00 && s[13] == 0x18 && s[14] == 0x34 && s[15] == 1);
}

// A two-track mixed-mode disc: 300 Mode 1 frames, then audio with a 150-frame
// pregap and 203 frames, one .bin per track.
#define DATA_FRAMES 300
#define AUDIO_PREGAP 150
#define AUDIO_FRAMES 203

static uint8_t *make_data_track(void) {
  uint8_t *t = calloc(DATA_FRAMES, CD_FRAME);
  uint32_t i;

  for (i = 0; i < DATA_FRAMES; i++) {
    uint8_t *s = t + (size_t)i * CD_FRAME;
    fill_random(s + 16, CD_DATA, i + 1);
    if (i == 16) {
      // An ISO 9660 descriptor would sit here; enough for the drive to serve.
      memcpy(s + 16, "\001CD001\001", 7);
    }
    cd_make_mode1(s, i);
  }
  return t;
}

static uint8_t *make_audio_track(void) {
  uint8_t *t = calloc(AUDIO_PREGAP + AUDIO_FRAMES, CD_FRAME);

  // Silent pregap, then "music".
  fill_random(t + (size_t)AUDIO_PREGAP * CD_FRAME, (size_t)AUDIO_FRAMES * CD_FRAME, 99);
  return t;
}

static void test_cue_and_drive(const char *dir) {
  uint8_t *data = make_data_track();
  uint8_t *audio = make_audio_track();
  struct cd_toc toc;
  struct cue_sheet cue;
  struct rip_transport t;
  char cue_text[2048];
  char path[600];
  const char *names[2] = {"Game (Track 1).bin", "Game (Track 2).bin"};
  uint8_t frame[CD_FRAME];
  uint8_t q[12];

  snprintf(path, sizeof(path), "%s/Game (Track 1).bin", dir);
  CHECK(write_file(path, data, (size_t)DATA_FRAMES * CD_FRAME) == 0);
  snprintf(path, sizeof(path), "%s/Game (Track 2).bin", dir);
  CHECK(write_file(path, audio, (size_t)(AUDIO_PREGAP + AUDIO_FRAMES) * CD_FRAME) == 0);

  // The sheet a rip of that disc would get.
  memset(&toc, 0, sizeof(toc));
  toc.count = 2;
  toc.sessions = 1;
  toc.tracks[0] = (struct cd_track){1, CD_CTRL_DATA, 1, CD_MODE1, 0, 0, DATA_FRAMES};
  toc.tracks[1] = (struct cd_track){2, 0, 1, CD_MODE_AUDIO, DATA_FRAMES + AUDIO_PREGAP, AUDIO_PREGAP, AUDIO_FRAMES};
  CHECK(cue_write(&toc, names, 1, cue_text, sizeof(cue_text)) > 0);
  CHECK(strstr(cue_text, "TRACK 02 AUDIO\r\n    INDEX 00 00:00:00\r\n    INDEX 01 00:02:00") != NULL);
  snprintf(path, sizeof(path), "%s/Game.cue", dir);
  CHECK(write_file(path, (const uint8_t *)cue_text, strlen(cue_text)) == 0);

  CHECK(cue_load(path, &cue) == RIP_OK);
  CHECK(cue.count == 2);
  CHECK(cue.tracks[1].start == DATA_FRAMES + AUDIO_PREGAP);
  CHECK(cue.tracks[1].pregap == AUDIO_PREGAP);
  CHECK(cue.tracks[0].length == DATA_FRAMES);
  CHECK(cue.leadout == DATA_FRAMES + AUDIO_PREGAP + AUDIO_FRAMES);

  // The image drive serves it back: TOC, raw frames, Q subchannel, pregaps.
  CHECK(rip_image_drive_open(&t, NULL) == RIP_OK);
  CHECK(rip_image_drive_load(&t, path) == RIP_OK);
  CHECK(cd_read_toc(&t, &toc) == RIP_OK);
  CHECK(toc.count == 2);
  CHECK(toc.leadout == DATA_FRAMES + AUDIO_PREGAP + AUDIO_FRAMES);
  CHECK(toc.tracks[1].start == DATA_FRAMES + AUDIO_PREGAP);
  CHECK(cd_detect_mode(&t, &toc.tracks[0]) == RIP_OK && toc.tracks[0].mode == CD_MODE1);
  CHECK(cd_read_frames(&t, 5, 1, 0, frame, NULL) == RIP_OK);
  CHECK_MEM(frame, data + 5 * CD_FRAME, CD_FRAME);
  CHECK(cd_read_q(&t, DATA_FRAMES + 10, q) == RIP_OK);
  CHECK(cd_unbcd(q[1]) == 2 && cd_unbcd(q[2]) == 0);  // track 2, index 0
  toc.tracks[1].pregap = 0;                           // forget it, then find it again
  cd_find_pregaps(&t, &toc);
  CHECK(toc.tracks[1].pregap == AUDIO_PREGAP);
  CHECK(toc.tracks[0].length == DATA_FRAMES);
  {
    // Offset correction: +6 samples reads 24 bytes further along.
    uint8_t shifted[CD_FRAME];
    uint32_t lba = DATA_FRAMES + AUDIO_PREGAP + 10;
    CHECK(cd_read_audio(&t, lba, 1, 6, toc.leadout, shifted) == RIP_OK);
    CHECK_MEM(shifted, audio + (size_t)(AUDIO_PREGAP + 10) * CD_FRAME + 24, CD_FRAME);
    // Past the lead-out reads as silence.
    CHECK(cd_read_audio(&t, toc.leadout - 1, 1, 6, toc.leadout, shifted) == RIP_OK);
    CHECK(shifted[CD_FRAME - 1] == 0 && shifted[CD_FRAME - 24] == 0);
  }
  rip_image_drive_eject(&t);
  CHECK(cd_read_toc(&t, &toc) != RIP_OK);
  rip_transport_close(&t);
  free(data);
  free(audio);
}

// A whole CD rip through the virtual drive: the mixed-mode disc above comes
// back as Redump's tracks and sheet, and as a CHD chdman takes apart into the
// same bytes.
static void test_cd_rip(const char *dir) {
  uint8_t *data = make_data_track();
  uint8_t *audio = make_audio_track();
  struct rip_transport t;
  struct rip_drive drive;
  struct rip_reader reader;
  struct rip_disc disc;
  struct rip_choice choices[8];
  struct rip_job_opts opts;
  struct rip_job_result res;
  const struct rip_recipe *bincue = NULL;
  const struct rip_recipe *chd = NULL;
  char path[800];
  char out[600];
  uint16_t profile = 0;
  size_t n;
  size_t i;
  size_t len = 0;
  uint8_t *back;

  snprintf(path, sizeof(path), "%s/Game.cue", dir);
  CHECK(rip_image_drive_open(&t, NULL) == RIP_OK);
  CHECK(rip_image_drive_load(&t, path) == RIP_OK);
  CHECK(rip_drive_probe(&drive, &t, RIP_DRIVE_EXTERNAL) == RIP_OK);
  CHECK(rip_drive_media(&drive, &profile) == RIP_MEDIA_PRESENT);
  CHECK(rip_reader_open_mmc(&reader, &t) == RIP_OK);
  rip_disc_identify(&disc, profile, &reader, &t, NULL);
  CHECK(disc.has_toc && disc.toc.count == 2);
  n = rip_plan(&drive, &disc, choices, 8);
  for (i = 0; i < n; i++) {
    if (choices[i].avail == RIP_AVAIL_OK && choices[i].recipe->id == RIP_RECIPE_BINCUE) {
      bincue = choices[i].recipe;
    }
    if (choices[i].avail == RIP_AVAIL_OK && choices[i].recipe->id == RIP_RECIPE_CHD) {
      chd = choices[i].recipe;
    }
  }
  CHECK(bincue != NULL && chd != NULL);
  memset(&opts, 0, sizeof(opts));
  snprintf(out, sizeof(out), "%s/cdrip", dir);
  opts.out_dir = out;
  opts.name = "Ripped";
  if (bincue) {
    CHECK(rip_run(bincue, &drive, &reader, &disc, &opts, &res) == RIP_OK);
    snprintf(path, sizeof(path), "%s/Ripped/Ripped (Track 1).bin", out);
    back = test_read_file(path, &len);
    CHECK(back && len == (size_t)DATA_FRAMES * CD_FRAME && !memcmp(back, data, len));
    free(back);
    snprintf(path, sizeof(path), "%s/Ripped/Ripped (Track 2).bin", out);
    back = test_read_file(path, &len);
    CHECK(back && len == (size_t)(AUDIO_PREGAP + AUDIO_FRAMES) * CD_FRAME && !memcmp(back, audio, len));
    free(back);
    snprintf(path, sizeof(path), "%s/Ripped/Ripped.cue", out);
    back = test_read_file(path, &len);
    CHECK(back && strstr((char *)back,
                         "FILE \"Ripped (Track 2).bin\" BINARY\r\n  TRACK 02 AUDIO\r\n    INDEX 00 00:00:00\r\n"
                         "    INDEX 01 00:02:00") != NULL);
    free(back);
    CHECK(res.manifest_path[0] && rip_file_exists(res.manifest_path));
  }
  if (chd) {
    char cue[700];
    char bin[700];
    CHECK(rip_run(chd, &drive, &reader, &disc, &opts, &res) == RIP_OK);
    CHECK(strstr(res.image_path, ".chd") != NULL && rip_file_exists(res.image_path));
    if (have_chdman()) {
      snprintf(cue, sizeof(cue), "%s/back.cue", out);
      snprintf(bin, sizeof(bin), "%s/back.bin", out);
      CHECK(run("chdman verify -i '%s' >/dev/null 2>&1%s", res.image_path, "") == 0);
      CHECK(run("chdman extractcd -i '%s' -o '%s' -f >/dev/null 2>&1", res.image_path, cue) == 0);
      back = test_read_file(bin, &len);
      CHECK(back && len == (size_t)(DATA_FRAMES + AUDIO_PREGAP + AUDIO_FRAMES) * CD_FRAME);
      if (back && len == (size_t)(DATA_FRAMES + AUDIO_PREGAP + AUDIO_FRAMES) * CD_FRAME) {
        CHECK(!memcmp(back, data, (size_t)DATA_FRAMES * CD_FRAME));
        CHECK(!memcmp(back + (size_t)DATA_FRAMES * CD_FRAME, audio, (size_t)(AUDIO_PREGAP + AUDIO_FRAMES) * CD_FRAME));
      }
      free(back);
    }
  }
  rip_reader_close(&reader);
  rip_transport_close(&t);
  free(data);
  free(audio);
}

// An audio CD through the virtual drive: three tracks, a 2-second gap before
// track 2. A file per track carries the gap at the end of the track before
// (EAC's way), the sheet says where; one file for the disc holds it all.
// (WAV here; the console writes FLAC through the same path.)
static void test_audio_rip(const char *dir) {
  enum { T1 = 1000, GAP = 150, T2 = 800, T3 = 500, ALL = T1 + GAP + T2 + T3 };
  uint8_t *disc_pcm = malloc((size_t)ALL * CD_FRAME);
  struct cd_toc toc;
  struct rip_transport t;
  struct rip_drive drive;
  struct rip_reader reader;
  struct rip_disc disc;
  struct rip_choice choices[8];
  struct rip_job_opts opts;
  struct rip_job_result res;
  struct rip_audio_meta *meta = calloc(1, sizeof(*meta));
  const struct rip_recipe *audio = NULL;
  const char *names[3] = {"Album (Track 1).bin", "Album (Track 2).bin", "Album (Track 3).bin"};
  char text[2048];
  char path[800];
  char out[600];
  uint16_t profile = 0;
  size_t n;
  size_t i;
  size_t len = 0;
  uint8_t *back;
  int wav = 1;  // the WAV recipe: every build has it

  if (!disc_pcm || !meta) {
    CHECK(0);
    free(disc_pcm);
    free(meta);
    return;
  }
  fill_random(disc_pcm, (size_t)ALL * CD_FRAME, 5);
  memset(&toc, 0, sizeof(toc));
  toc.count = 3;
  toc.sessions = 1;
  toc.tracks[0] = (struct cd_track){1, 0, 1, CD_MODE_AUDIO, 0, 0, T1};
  toc.tracks[1] = (struct cd_track){2, 0, 1, CD_MODE_AUDIO, T1 + GAP, GAP, T2};
  toc.tracks[2] = (struct cd_track){3, 0, 1, CD_MODE_AUDIO, T1 + GAP + T2, 0, T3};
  snprintf(path, sizeof(path), "%s/%s", dir, names[0]);
  CHECK(write_file(path, disc_pcm, (size_t)T1 * CD_FRAME) == 0);
  snprintf(path, sizeof(path), "%s/%s", dir, names[1]);
  CHECK(write_file(path, disc_pcm + (size_t)T1 * CD_FRAME, (size_t)(GAP + T2) * CD_FRAME) == 0);
  snprintf(path, sizeof(path), "%s/%s", dir, names[2]);
  CHECK(write_file(path, disc_pcm + (size_t)(T1 + GAP + T2) * CD_FRAME, (size_t)T3 * CD_FRAME) == 0);
  CHECK(cue_write(&toc, names, 1, text, sizeof(text)) > 0);
  snprintf(path, sizeof(path), "%s/Album.cue", dir);
  CHECK(write_file(path, (const uint8_t *)text, strlen(text)) == 0);

  CHECK(rip_image_drive_open(&t, NULL) == RIP_OK);
  CHECK(rip_image_drive_load(&t, path) == RIP_OK);
  CHECK(rip_drive_probe(&drive, &t, RIP_DRIVE_EXTERNAL) == RIP_OK);
  CHECK(rip_drive_media(&drive, &profile) == RIP_MEDIA_PRESENT);
  CHECK(rip_reader_open_mmc(&reader, &t) == RIP_OK);
  rip_disc_identify(&disc, profile, &reader, &t, NULL);
  CHECK(disc.kind == RIP_DISC_CD_AUDIO && disc.musicbrainz_id[0]);
  n = rip_plan(&drive, &disc, choices, 8);
  for (i = 0; i < n; i++) {
    if (choices[i].avail == RIP_AVAIL_OK && choices[i].recipe->id == RIP_RECIPE_WAV) {
      audio = choices[i].recipe;
    }
  }
  CHECK(audio != NULL);
  snprintf(meta->artist, sizeof(meta->artist), "Some Band");
  snprintf(meta->album, sizeof(meta->album), "Some \"Album\"");
  snprintf(meta->titles[1], sizeof(meta->titles[1]), "Second: Song");
  memset(&opts, 0, sizeof(opts));
  snprintf(out, sizeof(out), "%s/audio", dir);
  opts.out_dir = out;
  opts.name = "Some Album";
  opts.audio_quality = 5;
  opts.audio = meta;
  if (audio && wav) {
    static const struct {
      const char *name;
      size_t from;
      size_t frames;
    } files[3] = {{"01 - Track 01.wav", 0, T1 + GAP},
                  {"02 - Second - Song.wav", T1 + GAP, T2},
                  {"03 - Track 03.wav", T1 + GAP + T2, T3}};
    CHECK(rip_run(audio, &drive, &reader, &disc, &opts, &res) == RIP_OK);
    for (i = 0; i < 3; i++) {
      snprintf(path, sizeof(path), "%s/Some Album/%s", out, files[i].name);
      back = test_read_file(path, &len);
      CHECK(back && len == 44 + files[i].frames * CD_FRAME &&
            !memcmp(back + 44, disc_pcm + files[i].from * CD_FRAME, files[i].frames * CD_FRAME));
      free(back);
    }
    snprintf(path, sizeof(path), "%s/Some Album/Some Album.cue", out);
    back = test_read_file(path, &len);
    CHECK(back && strstr((char *)back, "PERFORMER \"Some Band\"\r\nTITLE \"Some 'Album'\"\r\n") != NULL);
    CHECK(back && strstr((char *)back,
                         "FILE \"01 - Track 01.wav\" WAVE\r\n  TRACK 01 AUDIO\r\n    INDEX 01 00:00:00\r\n"
                         "  TRACK 02 AUDIO\r\n    TITLE \"Second: Song\"\r\n    INDEX 00 00:13:25\r\n"
                         "FILE \"02 - Second - Song.wav\" WAVE\r\n    INDEX 01 00:00:00\r\n"
                         "FILE \"03 - Track 03.wav\" WAVE\r\n  TRACK 03 AUDIO\r\n    INDEX 01 00:00:00\r\n") != NULL);
    free(back);

    // One file for the disc.
    snprintf(out, sizeof(out), "%s/audio-image", dir);
    opts.audio_image = 1;
    CHECK(rip_run(audio, &drive, &reader, &disc, &opts, &res) == RIP_OK);
    snprintf(path, sizeof(path), "%s/Some Album/Some Album.wav", out);
    back = test_read_file(path, &len);
    CHECK(back && len == 44 + (size_t)ALL * CD_FRAME && !memcmp(back + 44, disc_pcm, (size_t)ALL * CD_FRAME));
    free(back);
    snprintf(path, sizeof(path), "%s/Some Album/Some Album.cue", out);
    back = test_read_file(path, &len);
    CHECK(back && strstr((char *)back,
                         "  TRACK 02 AUDIO\r\n    TITLE \"Second: Song\"\r\n    INDEX 00 00:13:25\r\n"
                         "    INDEX 01 00:15:25\r\n") != NULL);
    free(back);
    // A drive with a read offset (the SH-224DB's +6): every read is shifted,
    // a frame past the chunk. It ran past the end of the raw buffer once, which
    // the sanitizer build catches here.
    {
      struct rip_drive offset_drive = drive;
      snprintf(offset_drive.inquiry.vendor, sizeof(offset_drive.inquiry.vendor), "TSSTcorp");
      snprintf(offset_drive.inquiry.product, sizeof(offset_drive.inquiry.product), "CDDVDW SH-224DB");
      snprintf(out, sizeof(out), "%s/audio-offset", dir);
      opts.audio_image = 0;
      CHECK(rip_run(audio, &offset_drive, &reader, &disc, &opts, &res) == RIP_OK);
    }
  }
  rip_reader_close(&reader);
  rip_transport_close(&t);
  free(disc_pcm);
  free(meta);
}

static void test_chd_dvd(const char *dir) {
  enum { SECTORS = 1001 };  // not a whole number of hunks
  uint8_t *img = calloc(SECTORS, CD_DATA);
  struct chd_writer *w;
  uint8_t sha1[20];
  char chd[600];
  char iso[600];
  char out[600];
  size_t len = 0;
  uint8_t *back;

  fill_random(img, (size_t)500 * CD_DATA, 3);  // half noise, half zeros
  snprintf(chd, sizeof(chd), "%s/dvd.chd", dir);
  snprintf(iso, sizeof(iso), "%s/dvd.iso", dir);
  snprintf(out, sizeof(out), "%s/dvd-out.iso", dir);
  CHECK(write_file(iso, img, (size_t)SECTORS * CD_DATA) == 0);
  CHECK(chd_create_dvd(&w, chd, SECTORS, CHD_ZLIB) == RIP_OK);
  CHECK(chd_write_sectors(w, img, 600) == RIP_OK);
  CHECK(chd_write_sectors(w, img + (size_t)600 * CD_DATA, SECTORS - 600) == RIP_OK);
  CHECK(chd_finish(w, sha1) == RIP_OK);
  if (!have_chdman()) {
    printf("  (chdman not installed: CHD images not checked)\n");
    free(img);
    return;
  }
  CHECK(run("chdman verify -i '%s' >/dev/null 2>&1%s", chd, "") == 0);
  CHECK(run("chdman extractraw -i '%s' -o '%s' -f >/dev/null 2>&1", chd, out) == 0);
  back = test_read_file(out, &len);
  CHECK(back && len == (size_t)SECTORS * CD_DATA && memcmp(back, img, len) == 0);
  free(back);

  // zstd too
  CHECK(chd_create_dvd(&w, chd, SECTORS, CHD_ZSTD) == RIP_OK);
  CHECK(chd_write_sectors(w, img, SECTORS) == RIP_OK);
  CHECK(chd_finish(w, NULL) == RIP_OK);
  CHECK(run("chdman verify -i '%s' >/dev/null 2>&1%s", chd, "") == 0);
  free(img);
}

static void test_chd_cd(const char *dir) {
  uint8_t *data = make_data_track();
  uint8_t *audio = make_audio_track();
  struct chd_cd_track tracks[2] = {{CD_MODE1, DATA_FRAMES, 0},
                                   {CD_MODE_AUDIO, AUDIO_PREGAP + AUDIO_FRAMES, AUDIO_PREGAP}};
  struct chd_writer *w;
  char chd[600];
  char cue[600];
  char bin[600];
  size_t len = 0;
  uint8_t *back;
  int codec;

  snprintf(chd, sizeof(chd), "%s/cd.chd", dir);
  snprintf(cue, sizeof(cue), "%s/cd-out.cue", dir);
  snprintf(bin, sizeof(bin), "%s/cd-out.bin", dir);
  for (codec = CHD_ZLIB; codec <= CHD_ZSTD; codec++) {
    CHECK(chd_create_cd(&w, chd, tracks, 2, (enum chd_codec)codec, 0) == RIP_OK);
    CHECK(chd_write_frames(w, data, NULL, DATA_FRAMES) == RIP_OK);
    CHECK(chd_write_frames(w, audio, NULL, 100) == RIP_OK);
    CHECK(chd_write_frames(w, audio + (size_t)100 * CD_FRAME, NULL, AUDIO_PREGAP + AUDIO_FRAMES - 100) == RIP_OK);
    CHECK(chd_finish(w, NULL) == RIP_OK);
    if (!have_chdman()) {
      continue;
    }
    CHECK(run("chdman verify -i '%s' >/dev/null 2>&1%s", chd, "") == 0);
    CHECK(run("chdman extractcd -i '%s' -o '%s' -f >/dev/null 2>&1", chd, cue) == 0);
    back = test_read_file(bin, &len);
    // extractcd writes both tracks into one .bin, the pregap included.
    CHECK(back && len == (size_t)(DATA_FRAMES + AUDIO_PREGAP + AUDIO_FRAMES) * CD_FRAME);
    if (back && len == (size_t)(DATA_FRAMES + AUDIO_PREGAP + AUDIO_FRAMES) * CD_FRAME) {
      CHECK(memcmp(back, data, (size_t)DATA_FRAMES * CD_FRAME) == 0);
      CHECK(memcmp(back + (size_t)DATA_FRAMES * CD_FRAME, audio, (size_t)(AUDIO_PREGAP + AUDIO_FRAMES) * CD_FRAME) ==
            0);
    }
    free(back);
  }
  {
    // Sectors with valid ECC have it dropped: the image is smaller than one
    // whose ECC doesn't check out.
    size_t good = 0;
    size_t bad = 0;
    uint32_t i;
    free(test_read_file(chd, &good));
    for (i = 0; i < DATA_FRAMES; i++) {
      data[(size_t)i * CD_FRAME + 2300] ^= 0xFF;
    }
    CHECK(chd_create_cd(&w, chd, tracks, 2, CHD_ZSTD, 0) == RIP_OK);
    CHECK(chd_write_frames(w, data, NULL, DATA_FRAMES) == RIP_OK);
    CHECK(chd_write_frames(w, audio, NULL, AUDIO_PREGAP + AUDIO_FRAMES) == RIP_OK);
    CHECK(chd_finish(w, NULL) == RIP_OK);
    free(test_read_file(chd, &bad));
    CHECK(good + (size_t)DATA_FRAMES * 200 < bad);
  }
  free(data);
  free(audio);
}

// UDF 1.02 (DVD) and 2.50 (Blu-ray, metadata partition): mount, find, read.
static void test_udf(const char *dir) {
  static uint8_t stream[3 * 2048 + 100];
  const char unit_key[] = "AACS unit key file";
  struct udfb_file files[] = {
      {"BDMV/index.bdmv", (const uint8_t *)"INDX0200", 8},
      {"BDMV/STREAM/00000.m2ts", stream, sizeof(stream)},
      {"AACS/Unit_Key_RO.inf", (const uint8_t *)unit_key, sizeof(unit_key)},
  };
  int rev;

  fill_random(stream, sizeof(stream), 11);
  for (rev = 0; rev < 2; rev++) {
    size_t len = 0;
    uint8_t *img = udfb_build(files, RIP_ARRAY_LEN(files), rev, "INCEPTION", &len);
    struct rip_reader r;
    struct udf_fs fs;
    struct udf_entry e;
    uint8_t *data = NULL;
    size_t got = 0;
    char path[600];
    int fd;

    CHECK(img != NULL);
    if (!img) {
      continue;
    }
    snprintf(path, sizeof(path), "%s/udf%d.iso", dir, rev);
    CHECK(write_file(path, img, len) == 0);
    fd = open(path, O_RDONLY);
    CHECK(rip_reader_open_fd(&r, fd, 2048, len, 1) == RIP_OK);
    CHECK(udf_mount(&fs, &r) == RIP_OK);
    CHECK_STR(fs.logical_id, "INCEPTION");
    CHECK(udf_lookup(&fs, "BDMV", &e) == RIP_OK && e.is_dir);
    CHECK(udf_lookup(&fs, "bdmv/stream/00000.M2TS", &e) == RIP_OK && !e.is_dir && e.size == sizeof(stream));
    CHECK(udf_read_file(&fs, &e, 1 << 20, &data, &got) == RIP_OK && got == sizeof(stream) &&
          memcmp(data, stream, got) == 0);
    free(data);
    CHECK(udf_lookup(&fs, "AACS/Unit_Key_RO.inf", &e) == RIP_OK && e.size == sizeof(unit_key));
    CHECK(udf_lookup(&fs, "BDMV/missing", &e) == RIP_ENOENT);
    rip_reader_close(&r);
    // 7-Zip reads UDF too: a second opinion on the test images themselves.
    if (system("command -v 7z >/dev/null 2>&1") == 0) {
      char cmd[1400];
      snprintf(cmd, sizeof(cmd), "7z l '%s' 2>/dev/null | grep -q 'BDMV/STREAM/00000.m2ts'", path);
      CHECK(system(cmd) == 0);
    }
    free(img);
  }
}

// Writes one .bin and a sheet for it; returns the sheet's path in cue.
static void write_cd(const char *dir, const char *stem, const uint8_t *bin, size_t frames, const char *mode, char *cue,
                     size_t cn) {
  char path[600];
  char text[256];

  snprintf(path, sizeof(path), "%s/%s.bin", dir, stem);
  CHECK(write_file(path, bin, frames * CD_FRAME) == 0);
  snprintf(text, sizeof(text), "FILE \"%s.bin\" BINARY\r\n  TRACK 01 %s\r\n    INDEX 01 00:00:00\r\n", stem, mode);
  snprintf(cue, cn, "%s/%s.cue", dir, stem);
  CHECK(write_file(cue, (const uint8_t *)text, strlen(text)) == 0);
}

static int identify_image(const char *cue, struct rip_disc *disc) {
  struct rip_transport t;
  struct rip_reader r;
  int have;

  if (rip_image_drive_open(&t, NULL) != RIP_OK || rip_image_drive_load(&t, cue) != RIP_OK) {
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

// CDs are told apart by their TOC and the first sector of their data.
static void test_cd_identify(const char *dir) {
  enum { FRAMES = 300 };
  uint8_t *bin = calloc(FRAMES, CD_FRAME);
  struct rip_disc disc;
  char cue[600];
  uint32_t i;

  // A music CD: one audio track.
  fill_random(bin, (size_t)FRAMES * CD_FRAME, 5);
  write_cd(dir, "album", bin, FRAMES, "AUDIO", cue, sizeof(cue));
  CHECK(identify_image(cue, &disc) == RIP_OK);
  CHECK(disc.kind == RIP_DISC_CD_AUDIO);
  CHECK(disc.has_toc && disc.toc.count == 1);
  CHECK(strlen(disc.musicbrainz_id) == 28);

  // A Saturn game: its boot sector says so.
  memset(bin, 0, (size_t)FRAMES * CD_FRAME);
  for (i = 0; i < FRAMES; i++) {
    uint8_t *s = bin + (size_t)i * CD_FRAME;
    if (i == 0) {
      memcpy(s + 16, "SEGA SEGASATURN SEGA ENTERPRISESMK-81009  V1.000", 48);
      memcpy(s + 16 + 0x60, "NIGHTS INTO DREAMS", 18);
    }
    cd_make_mode1(s, i);
  }
  write_cd(dir, "saturn", bin, FRAMES, "MODE1/2352", cue, sizeof(cue));
  CHECK(identify_image(cue, &disc) == RIP_OK);
  CHECK(disc.kind == RIP_DISC_SATURN);
  CHECK_STR(disc.title_id, "MK-81009");
  CHECK_STR(disc.title, "NIGHTS INTO DREAMS");
  free(bin);
}

// A small PS4 package: a name table, two clear named entries (one in a
// folder), an encrypted one and one whose name tries to leave the folder.
static void test_pkg(const char *dir) {
  static uint8_t img[0x2000];
  const char names[] = "\0param.sfo\0trophy/trophy00.trp\0license.dat\0../evil";
  struct {
    uint32_t id, name, flags, offset, size;
    const char *data;
  } ents[] = {
      {0x0200, 0, 0, 0x1000, sizeof(names), NULL}, {0x1000, 1, 0, 0x1100, 8, "\0PSFsfo!"},
      {0x1400, 11, 0, 0x1200, 4, "TROP"},          {0x0400, 31, 0x80000000u, 0x1300, 4, "KEYS"},
      {0x1006, 43, 0, 0x1400, 4, "EVIL"},
  };
  char path[700];
  char out[600];
  struct pkg p;
  size_t len = 0;
  uint8_t *data;
  size_t i;
  int fd;

  memset(img, 0, sizeof(img));
  rip_put_be32(img, PKG_MAGIC);
  rip_put_be32(img + 0x10, RIP_ARRAY_LEN(ents));
  rip_put_be32(img + 0x18, 0x100);
  for (i = 0; i < RIP_ARRAY_LEN(ents); i++) {
    uint8_t *e = img + 0x100 + i * 32;
    rip_put_be32(e, ents[i].id);
    rip_put_be32(e + 4, ents[i].name);
    rip_put_be32(e + 8, ents[i].flags);
    rip_put_be32(e + 16, ents[i].offset);
    rip_put_be32(e + 20, ents[i].size);
    memcpy(img + ents[i].offset, ents[i].data ? ents[i].data : names, ents[i].size);
  }
  snprintf(path, sizeof(path), "%s/app.pkg", dir);
  CHECK(write_file(path, img, sizeof(img)) == 0);
  fd = open(path, O_RDONLY);
  CHECK(pkg_open(&p, fd) == RIP_OK);
  CHECK(p.count == RIP_ARRAY_LEN(ents));
  CHECK(pkg_find(&p, PKG_ENTRY_PARAM_SFO) && !strcmp(pkg_find(&p, PKG_ENTRY_PARAM_SFO)->name, "param.sfo"));
  CHECK(pkg_read_entry(&p, pkg_find(&p, 0x0400), &data, &len) == RIP_ENOTSUP);
  snprintf(out, sizeof(out), "%s/sce_sys", dir);
  CHECK(pkg_extract_named(&p, out) == 2);
  snprintf(path, sizeof(path), "%s/sce_sys/param.sfo", dir);
  data = test_read_file(path, &len);
  CHECK(data && len == 8 && !memcmp(data, "\0PSFsfo!", 8));
  free(data);
  snprintf(path, sizeof(path), "%s/sce_sys/trophy/trophy00.trp", dir);
  CHECK(rip_file_exists(path));
  snprintf(path, sizeof(path), "%s/sce_sys/license.dat", dir);
  CHECK(!rip_file_exists(path));
  snprintf(path, sizeof(path), "%s/evil", dir);
  CHECK(!rip_file_exists(path));
  pkg_close(&p);
  close(fd);
}

// A small PS4-style ELF: a loadable segment, a process parameter block and a
// version segment.
static size_t make_sce_elf(uint8_t *f, size_t cap) {
  uint8_t *ph = f + 0x40;
  size_t i;

  memset(f, 0, cap);
  memcpy(f,
         "\x7F"
         "ELF\x02\x01\x01\x09",
         8);
  rip_put_le16(f + 0x10, 0xFE10);
  rip_put_le16(f + 0x12, 0x3E);
  rip_put_le32(f + 0x20, 0x40);  // e_phoff
  rip_put_le16(f + 0x34, 0x40);
  rip_put_le16(f + 0x36, 0x38);
  rip_put_le16(f + 0x38, 3);
  for (i = 0; i < 3; i++) {
    static const uint32_t type[3] = {1, 0x61000001u, 0x6FFFFF01u};
    static const uint32_t off[3] = {0x1000, 0x6000, 0x7000};
    static const uint32_t size[3] = {0x4800, 0x40, 0x20};
    rip_put_le32(ph + i * 0x38, type[i]);
    rip_put_le32(ph + i * 0x38 + 0x08, off[i]);
    rip_put_le32(ph + i * 0x38 + 0x20, size[i]);
  }
  fill_random(f + 0x1000, 0x4800, 21);
  rip_put_le32(f + 0x6000, 0x40);
  rip_put_le32(f + 0x6008, 0x4942524F);  // "ORBI"
  rip_put_le32(f + 0x6010, 0x09008001);  // PS4 SDK 9.00
  rip_put_le32(f + 0x6014, 0x04000031);  // PS5 SDK 4.xx
  memcpy(f + 0x7000, "version segment, thirty-two b..", 32);
  return 0x7020;
}

static void test_sce_elf(const char *dir) {
  static uint8_t elf[0x8000];
  size_t len = make_sce_elf(elf, sizeof(elf));
  char in[600];
  char out[600];
  uint8_t *f;
  size_t flen = 0;
  uint32_t ps4 = 0;
  uint32_t ps5 = 0;

  snprintf(in, sizeof(in), "%s/eboot.elf", dir);
  snprintf(out, sizeof(out), "%s/eboot.bin", dir);
  CHECK(write_file(in, elf, len) == 0);
  CHECK(sce_is_elf(in) && !sce_is_self(in));

  // FSELF for a PS4: its magic, two entries for the one loadable segment,
  // the segment copied and its digests filled, the version segment last.
  CHECK(sce_write_fself(in, out, SCE_TARGET_PS4) == RIP_OK);
  CHECK(sce_is_self(out));
  f = test_read_file(out, &flen);
  CHECK(f && rip_le32(f) == SCE_SELF_PS4_MAGIC && rip_le16(f + 0x18) == 2);
  if (f && flen > 0x80) {
    uint64_t dig = rip_le32(f + 0x28);
    uint64_t seg = rip_le32(f + 0x48);
    struct rip_sha256 h;
    uint8_t d[32];
    CHECK(seg + 0x4800 <= flen && !memcmp(f + seg, elf + 0x1000, 0x4800));
    rip_sha256_init(&h);
    rip_sha256_update(&h, elf + 0x1000, 0x4000);
    rip_sha256_final(&h, d);
    CHECK(dig + 32 <= flen && !memcmp(f + dig, d, 32));
    CHECK(!memcmp(f + flen - 32, elf + 0x7000, 32));
  }
  free(f);
  CHECK(sce_write_fself(in, out, SCE_TARGET_PS5) == RIP_OK);
  f = test_read_file(out, &flen);
  CHECK(f && rip_le32(f) == SCE_SELF_PS5_MAGIC);
  {
    // Made the PS4's: byte for byte what writing it for a PS4 gives.
    uint8_t *ps4_made;
    size_t ps4_len = 0;
    char ps4_out[300];
    snprintf(ps4_out, sizeof(ps4_out), "%s/eboot.ps4", dir);
    CHECK(sce_write_fself(in, ps4_out, SCE_TARGET_PS4) == RIP_OK);
    CHECK(sce_retarget_fself(out, SCE_TARGET_PS4) == 1);
    CHECK(sce_retarget_fself(out, SCE_TARGET_PS4) == 0);
    free(f);
    f = test_read_file(out, &flen);
    ps4_made = test_read_file(ps4_out, &ps4_len);
    CHECK(f && ps4_made && flen == ps4_len && !memcmp(f, ps4_made, flen));
    free(ps4_made);
    unlink(ps4_out);
    // A plain ELF isn't one.
    CHECK(sce_retarget_fself(in, SCE_TARGET_PS4) == -1);
  }
  free(f);

  // Backports: lowered when above the target, left alone when not.
  CHECK(sce_backport_target("ps4-5.05", &ps4, &ps5) == RIP_OK && ps4 == 0x05050001 && ps5 == 0);
  CHECK(sce_backport_target("nonsense", &ps4, &ps5) == RIP_ENOENT);
  CHECK(sce_backport_elf(in, 0x05050001, 0) == 1);
  f = test_read_file(in, &flen);
  CHECK(f && rip_le32(f + 0x6010) == 0x05050001 && rip_le32(f + 0x6014) == 0x04000031);
  free(f);
  CHECK(sce_backport_elf(in, 0x05050001, 0) == 0);
  CHECK(sce_backport_elf(in, 0, 0x02000009) == 1);
  f = test_read_file(in, &flen);
  CHECK(f && rip_le32(f + 0x6014) == 0x02000009);
  free(f);
  {
    const char json[] =
        "{\"requiredSystemSoftwareVersion\": \"0x0450000000000000\", \"sdkVersion\": \"0x0100000000000000\"}";
    snprintf(in, sizeof(in), "%s/param.json", dir);
    CHECK(write_file(in, (const uint8_t *)json, sizeof(json) - 1) == 0);
    CHECK(sce_backport_json(in, 0x02000009) == 1);
    f = test_read_file(in, &flen);
    CHECK(f && strstr((char *)f, "\"0x0200000000000000\"") && strstr((char *)f, "\"0x0100000000000000\""));
    free(f);
  }
  {
    // A param.sfo with SYSTEM_VER and PUBTOOLINFO.
    static const char pub[] = "c_date=20190101,sdk_ver=09008001,st_type=digital50";
    uint8_t sfo[0x100];
    memset(sfo, 0, sizeof(sfo));
    memcpy(sfo, "\0PSF", 4);
    rip_put_le32(sfo + 4, 0x101);
    rip_put_le32(sfo + 0x08, 0x34);  // keys
    rip_put_le32(sfo + 0x0C, 0x50);  // data
    rip_put_le32(sfo + 0x10, 2);
    rip_put_le16(sfo + 0x14, 0);
    rip_put_le16(sfo + 0x16, 0x0404);
    rip_put_le32(sfo + 0x18, 4);
    rip_put_le32(sfo + 0x1C, 4);
    rip_put_le32(sfo + 0x20, 0);
    rip_put_le16(sfo + 0x24, 11);
    rip_put_le16(sfo + 0x26, 0x0204);
    rip_put_le32(sfo + 0x28, sizeof(pub));
    rip_put_le32(sfo + 0x2C, 0x40);
    rip_put_le32(sfo + 0x30, 4);
    memcpy(sfo + 0x34, "SYSTEM_VER\0PUBTOOLINFO", 23);
    rip_put_le32(sfo + 0x50, 0x09008001);
    memcpy(sfo + 0x54, pub, sizeof(pub));
    snprintf(in, sizeof(in), "%s/param.sfo", dir);
    CHECK(write_file(in, sfo, sizeof(sfo)) == 0);
    CHECK(sce_backport_sfo(in, 0x05050001) == 1);
    f = test_read_file(in, &flen);
    CHECK(f && rip_le32(f + 0x50) == 0x05050001 && strstr((char *)f + 0x54, "sdk_ver=05050001"));
    free(f);
  }
}

// The exFAT writer: boot regions with good checksums, and the files' data
// where the directory says. (Mounted by Linux's exfat driver by hand too:
// identical to the source, see DEVLOG.)
static void test_exfat(const char *dir) {
  static uint8_t data[150000];
  char src[600];
  char path[700];
  char img[600];
  uint8_t *im;
  size_t len = 0;
  uint32_t sum = 0;
  size_t i;

  snprintf(src, sizeof(src), "%s/exsrc", dir);
  snprintf(path, sizeof(path), "%s/sce_sys", src);
  CHECK(rip_mkdir_p(path) == 0);
  fill_random(data, sizeof(data), 77);
  snprintf(path, sizeof(path), "%s/eboot.bin", src);
  CHECK(write_file(path, data, sizeof(data)) == 0);
  snprintf(path, sizeof(path), "%s/sce_sys/param.json", src);
  CHECK(write_file(path, (const uint8_t *)"{}", 2) == 0);
  snprintf(img, sizeof(img), "%s/game.exfat", dir);
  CHECK(exfat_build(src, img, "TEST", NULL, NULL) == RIP_OK);
  im = test_read_file(img, &len);
  CHECK(im && len % (1024 * 1024) == 0);
  if (!im || len < 24 * 512) {
    free(im);
    return;
  }
  CHECK(!memcmp(im + 3, "EXFAT   ", 8) && im[510] == 0x55 && im[511] == 0xAA);
  CHECK(im[108] == 9 && im[109] == 7);  // 512-byte sectors, 64 KiB clusters
  for (i = 0; i < 11 * 512; i++) {
    if (i != 106 && i != 107 && i != 112) {
      sum = ((sum & 1) ? 0x80000000u : 0) + (sum >> 1) + im[i];
    }
  }
  CHECK(rip_le32(im + 11 * 512) == sum && rip_le32(im + 12 * 512 - 4) == sum);
  CHECK(!memcmp(im, im + 12 * 512, 12 * 512));  // the backup boot region
  {
    // eboot.bin's data sits whole in the cluster heap, cluster-aligned.
    uint64_t heap = (uint64_t)rip_le32(im + 88) * 512;
    uint64_t at;
    int found = 0;
    for (at = heap; at + sizeof(data) <= len && !found; at += 65536) {
      found = !memcmp(im + at, data, sizeof(data));
    }
    CHECK(found);
  }
  free(im);
}

// A small fsck for the UFS2 writer: the superblock passes FreeBSD's checks,
// every file reads back through its block pointers, and the groups' free maps
// and counts agree with the blocks and inodes the tree uses. (dissect.ffs
// reads the images too, see DEVLOG.)
struct ufs_check {
  const uint8_t *im;
  size_t len;
  const struct ufs_fs *fs;
  uint8_t *seen;  // blocks referenced
  uint64_t blocks;
  uint64_t inodes;
  uint64_t dirs;
  int bad;
};

static const struct ufs2_dinode *ufs_inode(struct ufs_check *u, uint64_t ino) {
  const struct ufs_fs *fs = u->fs;
  uint64_t cg = ino / fs->fs_ipg;
  uint64_t blk = (uint64_t)fs->fs_fpg * cg + (uint64_t)fs->fs_iblkno + (ino % fs->fs_ipg) / fs->fs_inopb;
  return (const struct ufs2_dinode *)(u->im + blk * (uint64_t)fs->fs_bsize) + ino % fs->fs_inopb;
}

static void ufs_use(struct ufs_check *u, uint64_t blk) {
  if (!blk || blk >= (uint64_t)u->fs->fs_size || (u->seen[blk / 8] >> (blk % 8)) & 1) {
    u->bad++;
    return;
  }
  u->seen[blk / 8] |= (uint8_t)(1u << (blk % 8));
  u->blocks++;
}

// The k-th data block of an inode, marking the indirect blocks on the way.
static uint64_t ufs_bmap(struct ufs_check *u, const struct ufs2_dinode *di, uint64_t k, int mark) {
  uint64_t n = (uint64_t)u->fs->fs_nindir;
  const int64_t *ind;

  if (k < UFS_NDADDR) {
    return (uint64_t)di->di_db[k];
  }
  k -= UFS_NDADDR;
  if (k < n) {
    if (mark && k == 0) {
      ufs_use(u, (uint64_t)di->di_ib[0]);
    }
    ind = (const int64_t *)(u->im + (uint64_t)di->di_ib[0] * (uint64_t)u->fs->fs_bsize);
    return (uint64_t)ind[k];
  }
  k -= n;
  if (mark && k == 0) {
    ufs_use(u, (uint64_t)di->di_ib[1]);
  }
  ind = (const int64_t *)(u->im + (uint64_t)di->di_ib[1] * (uint64_t)u->fs->fs_bsize);
  if (mark && k % n == 0) {
    ufs_use(u, (uint64_t)ind[k / n]);
  }
  ind = (const int64_t *)(u->im + (uint64_t)ind[k / n] * (uint64_t)u->fs->fs_bsize);
  return (uint64_t)ind[k % n];
}

// Reads a file's contents into a new buffer, marking its blocks.
static uint8_t *ufs_read(struct ufs_check *u, const struct ufs2_dinode *di) {
  uint64_t bs = (uint64_t)u->fs->fs_bsize;
  uint64_t nblk = (di->di_size + bs - 1) / bs;
  uint8_t *out = malloc((size_t)di->di_size + 1);
  uint64_t before = u->blocks;
  uint64_t k;

  for (k = 0; out && k < nblk; k++) {
    uint64_t b = ufs_bmap(u, di, k, 1);
    uint64_t take = di->di_size - k * bs < bs ? di->di_size - k * bs : bs;
    ufs_use(u, b);
    if (b * bs + take > u->len) {
      u->bad++;
      break;
    }
    memcpy(out + k * bs, u->im + b * bs, (size_t)take);
  }
  if ((u->blocks - before) * (bs / 512) != di->di_blocks) {
    u->bad++;
  }
  return out;
}

// Walks a directory, comparing each file with the same path under src.
static void ufs_walk(struct ufs_check *u, uint64_t ino, uint64_t parent, const char *src) {
  const struct ufs2_dinode *di = ufs_inode(u, ino);
  uint8_t *d = ufs_read(u, di);
  uint64_t at = 0;
  unsigned subdirs = 0;

  u->inodes++;
  u->dirs++;
  if (!d || (di->di_mode & 0170000) != 0040000 || di->di_size % UFS_DIRBLKSIZ) {
    u->bad++;
    free(d);
    return;
  }
  while (at < di->di_size) {
    uint32_t e = rip_le32(d + at);
    uint16_t reclen = rip_le16(d + at + 4);
    char name[256];
    char path[1024];
    if (reclen < 12 || (at % UFS_DIRBLKSIZ) + reclen > UFS_DIRBLKSIZ) {
      u->bad++;
      break;
    }
    memcpy(name, d + at + 8, d[at + 7]);
    name[d[at + 7]] = 0;
    at += reclen;
    if (!e) {
      continue;
    }
    if (!strcmp(name, ".")) {
      u->bad += e != ino;
      continue;
    }
    if (!strcmp(name, "..")) {
      u->bad += e != parent;
      continue;
    }
    snprintf(path, sizeof(path), "%s/%s", src, name);
    if (d[at - reclen + 6] == 4) {
      subdirs++;
      ufs_walk(u, e, ino, path);
    } else {
      const struct ufs2_dinode *f = ufs_inode(u, e);
      size_t want_len = 0;
      uint8_t *want = test_read_file(path, &want_len);
      uint8_t *got = ufs_read(u, f);
      u->inodes++;
      if (!want || !got || want_len != f->di_size || memcmp(want, got, want_len) || f->di_nlink != 1) {
        u->bad++;
      }
      free(want);
      free(got);
    }
  }
  if (di->di_nlink != 2 + subdirs) {
    u->bad++;
  }
  free(d);
}

static void ufs_check_image(const char *img, const char *src, const char *label) {
  struct ufs_check u;
  const struct ufs_fs *fs;
  uint64_t bs;
  uint64_t c;
  int64_t nbfree = 0;
  int64_t nifree = 0;
  int64_t ndir = 0;
  uint64_t free_marked = 0;

  memset(&u, 0, sizeof(u));
  u.im = test_read_file(img, &u.len);
  CHECK(u.im && u.len > 2 * 65536);
  if (!u.im || u.len <= 2 * 65536) {
    free((void *)u.im);
    return;
  }
  fs = u.fs = (const struct ufs_fs *)(u.im + UFS_SBLOCK);
  bs = (uint64_t)fs->fs_bsize;
  CHECK(fs->fs_magic == UFS2_MAGIC && fs->fs_bsize == 65536 && fs->fs_fsize == 65536 && fs->fs_frag == 1);
  CHECK((uint64_t)fs->fs_size * bs == u.len && fs->fs_sbsize == UFS_SBLOCKSIZE && fs->fs_sblockloc == UFS_SBLOCK);
  CHECK(fs->fs_fsbtodb == 7 && fs->fs_nindir == 8192 && fs->fs_inopb == 256 && fs->fs_maxsymlinklen == 120);
  CHECK(fs->fs_sblkno == 2 && fs->fs_cblkno == 3 && fs->fs_iblkno == 4 &&
        fs->fs_dblkno == 4 + (int32_t)(fs->fs_ipg / 256) && fs->fs_ipg % 256 == 0);
  CHECK(fs->fs_size > ((int64_t)fs->fs_ncg - 1) * fs->fs_fpg && fs->fs_size <= (int64_t)fs->fs_ncg * fs->fs_fpg);
  CHECK(fs->fs_csaddr == fs->fs_dblkno && fs->fs_cssize == 65536 && fs->fs_cgsize == 65536);
  CHECK(fs->fs_contigsumsize == 2 && fs->fs_maxcontig == 2 && fs->fs_bmask == -65536 && fs->fs_qbmask == 65535);
  CHECK(!strcmp((const char *)fs->fs_volname, label));
  u.seen = calloc(1, (size_t)(fs->fs_size / 8 + 1));
  if (!u.seen) {
    free((void *)u.im);
    return;
  }
  ufs_walk(&u, UFS_ROOTINO, UFS_ROOTINO, src);
  CHECK(u.bad == 0);

  // Each group: magic, a superblock copy, free blocks exactly those not used
  // by metadata or the tree, inode counts that add up.
  for (c = 0; c < fs->fs_ncg; c++) {
    uint64_t base = (uint64_t)fs->fs_fpg * c;
    const struct ufs_cg *cg = (const struct ufs_cg *)(u.im + (base + 3) * bs);
    const uint8_t *blksfree = (const uint8_t *)cg + cg->cg_freeoff;
    const uint8_t *iused = (const uint8_t *)cg + cg->cg_iusedoff;
    const struct ufs_csum *cs = (const struct ufs_csum *)(u.im + (uint64_t)fs->fs_csaddr * bs) + c;
    uint64_t meta_end = (uint64_t)fs->fs_dblkno + (c == 0 ? 1 : 0);
    uint32_t i;
    uint32_t used_inodes = 0;
    int64_t cg_free = 0;
    CHECK(cg->cg_magic == UFS_CG_MAGIC && cg->cg_cgx == c && cg->cg_niblk == fs->fs_ipg);
    CHECK(!memcmp(u.im + (base + 2) * bs, u.im + UFS_SBLOCK, sizeof(struct ufs_fs)));
    CHECK(cg->cg_nextfreeoff <= 65536 && cg->cg_initediblk <= fs->fs_ipg);
    for (i = 0; i < cg->cg_ndblk; i++) {
      int is_free = (blksfree[i / 8] >> (i % 8)) & 1;
      int is_meta = (c == 0 && i < meta_end) || (c > 0 && i >= 2 && i < meta_end);
      int is_used = (u.seen[(base + i) / 8] >> ((base + i) % 8)) & 1;
      if (is_free == (is_meta || is_used)) {
        u.bad++;
      }
      cg_free += is_free;
    }
    for (i = 0; i < fs->fs_ipg; i++) {
      used_inodes += (iused[i / 8] >> (i % 8)) & 1;
    }
    CHECK(cg->cg_cs.cs_nbfree == cg_free && cs->cs_nbfree == cg_free);
    CHECK((uint32_t)cg->cg_cs.cs_nifree == fs->fs_ipg - used_inodes && cs->cs_nifree == cg->cg_cs.cs_nifree);
    CHECK(cs->cs_ndir == cg->cg_cs.cs_ndir);
    nbfree += cg_free;
    nifree += cg->cg_cs.cs_nifree;
    ndir += cg->cg_cs.cs_ndir;
    free_marked += (uint64_t)cg_free;
  }
  CHECK(u.bad == 0);
  CHECK(fs->fs_cstotal.cs_nbfree == nbfree && fs->fs_cstotal.cs_nifree == nifree);
  CHECK(fs->fs_cstotal.cs_ndir == ndir && (uint64_t)ndir == u.dirs);
  CHECK((uint64_t)fs->fs_ncg * fs->fs_ipg - (uint64_t)nifree == u.inodes + 2);
  free(u.seen);
  free((void *)u.im);
}

static void test_ufs2(const char *dir) {
  static uint8_t data[1600000];  // past the 12 direct blocks
  char src[600];
  char path[700];
  char img[600];
  int i;

  snprintf(src, sizeof(src), "%s/ufsrc", dir);
  snprintf(path, sizeof(path), "%s/sce_sys/about", src);
  CHECK(rip_mkdir_p(path) == 0);
  snprintf(path, sizeof(path), "%s/sce_module", src);
  CHECK(rip_mkdir_p(path) == 0);
  fill_random(data, sizeof(data), 91);
  snprintf(path, sizeof(path), "%s/eboot.bin", src);
  CHECK(write_file(path, data, sizeof(data)) == 0);
  snprintf(path, sizeof(path), "%s/sce_sys/param.json", src);
  CHECK(write_file(path, (const uint8_t *)"{}", 2) == 0);
  snprintf(path, sizeof(path), "%s/sce_sys/about/empty", src);
  CHECK(write_file(path, data, 0) == 0);
  // Enough names to spill sce_module over several directory chunks.
  for (i = 0; i < 60; i++) {
    snprintf(path, sizeof(path), "%s/sce_module/libSceSomething_with_a_long_name_%02d.prx", src, i);
    CHECK(write_file(path, data + i * 1000, 65536 + (size_t)i) == 0);
  }
  snprintf(img, sizeof(img), "%s/game.ffpkg", dir);
  CHECK(ufs2_build(src, img, "TEST", NULL, NULL) == RIP_OK);
  ufs_check_image(img, src, "TEST");
}

void test_formats(void) {
  char dir[64];
  char cmd[128];

  test_tmpdir(dir, sizeof(dir));
  test_ecc();
  test_cue_and_drive(dir);
  test_cd_rip(dir);
  test_audio_rip(dir);
  test_chd_dvd(dir);
  test_chd_cd(dir);
  test_udf(dir);
  test_cd_identify(dir);
  test_pkg(dir);
  test_sce_elf(dir);
  test_exfat(dir);
  test_ufs2(dir);
  snprintf(cmd, sizeof(cmd), "rm -rf '%s'", dir);
  if (system(cmd) != 0) {
    fprintf(stderr, "could not remove %s\n", dir);
  }
}
