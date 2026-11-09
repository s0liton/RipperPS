// End-to-end: fake drive -> probe -> identify -> plan -> rip -> files on disk.

#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "fake_drive.h"
#include "iso_builder.h"
#include "ripper/common.h"
#include "ripper/disc.h"
#include "ripper/drive.h"
#include "ripper/hash.h"
#include "ripper/keydb.h"
#include "ripper/ps3.h"
#include "ripper/reader.h"
#include "ripper/rip.h"
#include "ripper/remux.h"
#include "ripper/util.h"
#include "test.h"

#define SECTORS 64

static void rm_dir(const char *dir) {
  DIR *d = opendir(dir);
  struct dirent *e;
  char path[1024];

  if (!d) {
    return;
  }
  while ((e = readdir(d)) != NULL) {
    if (strcmp(e->d_name, ".") && strcmp(e->d_name, "..")) {
      snprintf(path, sizeof(path), "%s/%s", dir, e->d_name);
      remove(path);
    }
  }
  closedir(d);
  rmdir(dir);
}

static void sha1_of(const uint8_t *data, size_t len, uint8_t out[20]) {
  struct rip_sha1 ctx;

  rip_sha1_init(&ctx);
  rip_sha1_update(&ctx, data, len);
  rip_sha1_final(&ctx, out);
}

static int file_equals(const char *path, const uint8_t *data, size_t len) {
  size_t got = 0;
  uint8_t *buf = test_read_file(path, &got);
  int same = buf && got == len && memcmp(buf, data, len) == 0;

  free(buf);
  return same;
}

struct rig {
  struct fake_drive fake;
  struct rip_transport t;
  struct rip_drive drive;
  struct rip_reader reader;
  struct rip_disc disc;
};

static int rig_up(struct rig *g, const uint8_t *img, size_t len, uint16_t profile, int sony) {
  uint16_t current = 0;

  memset(g, 0, sizeof(*g));
  g->fake.image = img;
  g->fake.image_len = len;
  g->fake.profile = profile;
  g->fake.dvd = 1;
  g->fake.bd = 1;
  g->fake.sony = sony;
  g->fake.fail_lba = -1;
  fake_drive_transport(&g->fake, &g->t);
  if (rip_drive_probe(&g->drive, &g->t, sony ? RIP_DRIVE_INTERNAL : RIP_DRIVE_EXTERNAL) != RIP_OK) {
    return RIP_ERR;
  }
  if (rip_drive_media(&g->drive, &current) != RIP_MEDIA_PRESENT || current != profile) {
    return RIP_ERR;
  }
  if (rip_reader_open_mmc(&g->reader, &g->t) != RIP_OK) {
    return RIP_ERR;
  }
  return rip_disc_identify(&g->disc, profile, &g->reader, &g->t, NULL);
}

static void test_ps2(uint16_t profile, int sony, int weak_sector) {
  const char cnf[] = "BOOT2 = cdrom0:\\SLUS_209.46;1\r\nVER = 1.00\r\n";
  struct isob_file files[] = {{"SYSTEM.CNF", (const uint8_t *)cnf, sizeof(cnf) - 1, 0}};
  struct rip_job_opts opts;
  struct rip_job_result res;
  const struct rip_recipe *recipe;
  struct rig g;
  uint8_t want_sha1[20];
  size_t len;
  uint8_t *img = isob_build(files, 1, SECTORS, "SLUS_20946", &len);
  char dir[64];
  char manifest[128];

  CHECK(rig_up(&g, img, len, profile, sony) == RIP_OK);
  CHECK(g.drive.caps & RIP_CAP_READ_DVD);
  CHECK(!(g.drive.caps & RIP_CAP_READ_CD));
  CHECK(g.disc.kind == RIP_DISC_PS2_DVD);
  CHECK_STR(g.disc.title_id, "SLUS-20946");
  if (weak_sector) {
    g.fake.fail_lba = 40;
  }

  recipe = rip_default_recipe(&g.drive, &g.disc);
  CHECK(recipe && recipe->id == RIP_RECIPE_ISO);
  test_tmpdir(dir, sizeof(dir));
  memset(&opts, 0, sizeof(opts));
  opts.out_dir = dir;
  CHECK(rip_run(recipe, &g.drive, &g.reader, &g.disc, &opts, &res) == RIP_OK);
  CHECK(file_equals(res.image_path, img, len));
  sha1_of(img, len, want_sha1);
  CHECK_MEM(res.hashes.sha1, want_sha1, 20);
  CHECK(res.hashes.size == len);
  snprintf(manifest, sizeof(manifest), "%s/SLUS-20946.txt", dir);
  CHECK(rip_file_exists(manifest));

  rip_reader_close(&g.reader);
  rm_dir(dir);
  free(img);
}

// PS2 DVD to CHD, named by the user; chdman reads back the very same image.
static void test_ps2_chd(void) {
  const char cnf[] = "BOOT2 = cdrom0:\\SLUS_209.46;1\r\n";
  struct isob_file files[] = {{"SYSTEM.CNF", (const uint8_t *)cnf, sizeof(cnf) - 1, 0}};
  struct rip_choice choices[8];
  struct rip_job_opts opts;
  struct rip_job_result res;
  const struct rip_recipe *chd = NULL;
  struct rig g;
  size_t len;
  size_t n;
  size_t i;
  uint8_t *img = isob_build(files, 1, SECTORS, "SLUS_20946", &len);
  char dir[64];
  char cmd[1400];
  char raw[128];

  CHECK(rig_up(&g, img, len, 0xFF61, 1) == RIP_OK);
  n = rip_plan(&g.drive, &g.disc, choices, RIP_ARRAY_LEN(choices));
  for (i = 0; i < n; i++) {
    if (choices[i].recipe->id == RIP_RECIPE_CHD && choices[i].avail == RIP_AVAIL_OK) {
      chd = choices[i].recipe;
    }
  }
  CHECK(chd != NULL);
  test_tmpdir(dir, sizeof(dir));
  memset(&opts, 0, sizeof(opts));
  opts.out_dir = dir;
  opts.name = "Gran Turismo 4 (USA)";
  opts.chd_codec = CHD_ZSTD;
  CHECK(chd && rip_run(chd, &g.drive, &g.reader, &g.disc, &opts, &res) == RIP_OK);
  CHECK(strstr(res.image_path, "/Gran Turismo 4 (USA).chd") != NULL);
  if (system("command -v chdman >/dev/null 2>&1") == 0) {
    snprintf(raw, sizeof(raw), "%s/raw.iso", dir);
    snprintf(cmd, sizeof(cmd), "chdman extractraw -i '%s' -o '%s' -f >/dev/null 2>&1", res.image_path, raw);
    CHECK(system(cmd) == 0);
    CHECK(file_equals(raw, img, len));
  }
  rip_reader_close(&g.reader);
  rm_dir(dir);
  free(img);
}

// Builds a plain PS3 image and its encrypted twin (sectors 32..47 encrypted).
static void make_ps3(uint8_t **plain, uint8_t **enc, size_t *len, const uint8_t key[16]) {
  static uint8_t eboot[16 * 2048];
  uint8_t sfo[128];
  size_t sfo_len = test_make_sfo(sfo, "TITLE_ID", "BLUS30001");
  const char sfb[] = ".SFB test";
  struct isob_file files[] = {
      {"PS3_DISC.SFB", (const uint8_t *)sfb, sizeof(sfb), 0},
      {"PS3_GAME/PARAM.SFO", sfo, sfo_len, 0},
      {"PS3_GAME/EBOOT.BIN", eboot, sizeof(eboot), 32},
  };
  struct ps3_layout layout;
  size_t i;

  for (i = 0; i < sizeof(eboot); i++) {
    eboot[i] = (uint8_t)(i * 13 + (i >> 11));
  }
  *plain = isob_build(files, RIP_ARRAY_LEN(files), SECTORS, "PS3VOLUME", len);
  rip_put_be32(*plain, 2);
  rip_put_be32(*plain + 8, 0);
  rip_put_be32(*plain + 12, 31);
  rip_put_be32(*plain + 16, 48);
  rip_put_be32(*plain + 20, SECTORS - 1);
  *enc = malloc(*len);
  memcpy(*enc, *plain, *len);
  ps3_parse_layout(*enc, &layout);
  ps3_crypt_sectors(&layout, key, 0, 0, *enc, SECTORS);
}

static void write_keys(const char *path, const uint8_t sha1[20], size_t size, const uint8_t key[16]) {
  char sha1_hex[41];
  char key_hex[33];
  FILE *f = fopen(path, "w");

  rip_hex(sha1, 20, sha1_hex);
  rip_hex(key, 16, key_hex);
  fprintf(f, "# test keys\n%s %zu %s Test Game (USA)\n", sha1_hex, size, key_hex);
  fclose(f);
}

static void test_ps3(void) {
  uint8_t key[16];
  uint8_t *plain;
  uint8_t *enc;
  size_t len;
  uint8_t enc_sha1[20];
  char dir[64];
  char keys_path[128];
  struct rip_keydb db;
  struct rip_job_opts opts;
  struct rip_job_result res;
  const struct rip_recipe *recipe;
  struct rig g;
  int pass;

  memset(key, 0x42, sizeof(key));
  key[0] = 0x01;
  make_ps3(&plain, &enc, &len, key);
  CHECK(memcmp(plain, enc, len) != 0);
  sha1_of(enc, len, enc_sha1);

  // Pass 0: key found, decrypt in place. 1: also keep the encrypted image. 2: no key.
  for (pass = 0; pass < 3; pass++) {
    CHECK(rig_up(&g, enc, len, 0xFF71, 1) == RIP_OK);
    CHECK(g.disc.kind == RIP_DISC_PS3_BD);
    CHECK_STR(g.disc.title_id, "BLUS-30001");
    recipe = rip_default_recipe(&g.drive, &g.disc);
    CHECK(recipe && recipe->id == RIP_RECIPE_PS3_ISO);

    test_tmpdir(dir, sizeof(dir));
    snprintf(keys_path, sizeof(keys_path), "%s/ps3.keys", dir);
    write_keys(keys_path, enc_sha1, len, key);
    CHECK(rip_keydb_load(&db, keys_path) == RIP_OK && db.count == 1);
    memset(&opts, 0, sizeof(opts));
    opts.out_dir = dir;
    opts.ps3_keys = pass == 2 ? NULL : &db;
    opts.ps3_keep_encrypted = pass == 1;

    CHECK(rip_run(recipe, &g.drive, &g.reader, &g.disc, &opts, &res) == RIP_OK);
    CHECK_MEM(res.hashes.sha1, enc_sha1, 20);
    if (pass == 2) {
      CHECK(!res.decrypted);
      CHECK(file_equals(res.image_path, enc, len));
    } else {
      CHECK(res.decrypted);
      CHECK_STR(res.name, "Test Game (USA)");
      CHECK(file_equals(res.image_path, plain, len));
    }
    if (pass == 1) {
      CHECK(file_equals(res.encrypted_path, enc, len));
      CHECK(strstr(res.image_path, "(decrypted)") != NULL);
    }
    rip_keydb_free(&db);
    rip_reader_close(&g.reader);
    rm_dir(dir);
  }

  // The same disc in a standard drive: identified from PS3_DISC.SFB instead.
  CHECK(rig_up(&g, enc, len, 0x0040, 0) == RIP_OK);
  CHECK(g.disc.kind == RIP_DISC_PS3_BD);
  rip_reader_close(&g.reader);
  free(plain);
  free(enc);
}

static void test_plan(void) {
  struct rip_drive internal;
  struct rip_disc disc;
  struct rip_choice choices[8];
  size_t n;

  memset(&internal, 0, sizeof(internal));
  internal.caps = RIP_CAP_READ_DVD | RIP_CAP_READ_BD | RIP_CAP_SONY_PROFILES;
  memset(&disc, 0, sizeof(disc));

  disc.kind = RIP_DISC_PS1;
  n = rip_plan(&internal, &disc, choices, RIP_ARRAY_LEN(choices));
  CHECK(n >= 1);
  CHECK(choices[0].avail == RIP_AVAIL_NEEDS_DRIVE);
  CHECK(rip_default_recipe(&internal, &disc) == NULL);

  // CSS is handled (libdvdcss): a protected DVD-Video can be ripped. MKV and
  // MP4 need FFmpeg, which this test build doesn't have; ISO always works.
  disc.kind = RIP_DISC_DVD_VIDEO;
  disc.css = 1;
  n = rip_plan(&internal, &disc, choices, RIP_ARRAY_LEN(choices));
  CHECK(n == 3 && choices[0].recipe->id == RIP_RECIPE_MKV && choices[1].recipe->id == RIP_RECIPE_MP4);
  CHECK(choices[2].recipe->id == RIP_RECIPE_ISO && choices[2].avail == RIP_AVAIL_OK);
  CHECK(choices[0].avail == (remux_available(REMUX_MKV) ? RIP_AVAIL_OK : RIP_AVAIL_PLANNED));
  CHECK(rip_default_recipe(&internal, &disc) != NULL);

  // PS4: a dump of the installed game; whether it's installed is the service's question.
  disc.kind = RIP_DISC_PS4_BD;
  disc.css = 0;
  n = rip_plan(&internal, &disc, choices, RIP_ARRAY_LEN(choices));
  CHECK(n == 1 && choices[0].recipe->id == RIP_RECIPE_SYSTEM_DUMP && choices[0].avail == RIP_AVAIL_OK);
}

// PS5: the title ID from the disc's second sector, before the system mounts it
// (the dump looks the installed game up by it). Laid out as a tester's disc.
static void test_ps5_header(void) {
  size_t len = (size_t)32 * 2048;
  uint8_t *img = calloc(1, len);
  struct rig g;

  CHECK(img != NULL);
  if (!img) {
    return;
  }
  memcpy(img + 0x800 + 0x400, "PlayStation5", 12);
  memcpy(img + 0x800 + 0x410, "PPSA-28038                      ", 32);
  CHECK(rig_up(&g, img, len, MMC_PROFILE_PS5_BD, 1) == RIP_OK);
  CHECK(g.disc.kind == RIP_DISC_PS5_BD);
  CHECK(!strcmp(g.disc.title_id, "PPSA28038"));
  rip_reader_close(&g.reader);
  free(img);
}

static void test_empty_tray(void) {
  struct fake_drive fake;
  struct rip_transport t;
  struct rip_drive drive;
  uint16_t profile;

  memset(&fake, 0, sizeof(fake));
  fake.dvd = 1;
  fake.fail_lba = -1;
  fake_drive_transport(&fake, &t);
  CHECK(rip_drive_probe(&drive, &t, RIP_DRIVE_INTERNAL) == RIP_OK);
  CHECK(rip_drive_media(&drive, &profile) == RIP_MEDIA_EMPTY);
}

void test_rip_all(void) {
  test_ps2(0x0010, 0, 0);
  test_ps2(0xFF61, 1, 1);
  test_ps2_chd();
  test_ps3();
  test_plan();
  test_ps5_header();
  test_empty_tray();
}
