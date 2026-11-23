// Blu-ray movies' AACS (src/core/aacs.c): the KEYDB.cfg, the unit keys, the
// aligned units against vectors made with openssl's AES from the spec, and a
// whole disc read back through the decrypting reader.
#include <dirent.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "fake_drive.h"
#include "ripper/aacs.h"
#include "ripper/aes.h"
#include "ripper/common.h"
#include "ripper/diag.h"
#include "ripper/hash.h"
#include "ripper/mmc.h"
#include "ripper/reader.h"
#include "ripper/udf.h"
#include "ripper/util.h"
#include "test.h"
#include "udf_builder.h"

// A unit of 32 source packets: the copy permission bits set in the first
// header, 0x47 at each packet's sync byte, a pattern after.
static void make_unit(uint8_t *u, unsigned seed) {
  unsigned i;
  unsigned k;

  for (i = 0; i < 32; i++) {
    uint8_t *p = u + i * 192;
    p[0] = i == 0 ? 0xC0 : 0x00;
    p[1] = (uint8_t)i;
    p[2] = 0x12;
    p[3] = 0x34;
    p[4] = 0x47;
    for (k = 5; k < 192; k++) {
      p[k] = (uint8_t)((i * 7 + k * 13 + seed) & 0xFF);
    }
  }
}

static void sha1_hex(const uint8_t *p, size_t n, char out[41]) {
  struct rip_sha1 h;
  uint8_t d[20];

  rip_sha1_init(&h);
  rip_sha1_update(&h, p, n);
  rip_sha1_final(&h, d);
  rip_hex(d, 20, out);
}

static void test_units(void) {
  uint8_t uk[16];
  uint8_t wrong[16];
  uint8_t *u = malloc(AACS_UNIT);
  uint8_t *plain = malloc(AACS_UNIT);
  char hex[41];
  int i;

  for (i = 0; i < 16; i++) {
    uk[i] = (uint8_t)(0x10 + i);
    wrong[i] = (uint8_t)i;
  }
  make_unit(u, 0);
  memcpy(plain, u, AACS_UNIT);
  sha1_hex(u, AACS_UNIT, hex);
  CHECK_STR(hex, "bfe4626a307324da182b9c4717f4286848eb9494");
  // openssl enc -aes-128-ecb on the first 16 bytes, XORed with them, then
  // -aes-128-cbc with the AACS IV over the rest.
  aacs_encrypt_unit(uk, u);
  sha1_hex(u, AACS_UNIT, hex);
  CHECK_STR(hex, "38da8335242e8219b9c5039873d584004af076aa");
  CHECK(!memcmp(u, plain, 16));  // the clear head
  CHECK(aacs_decrypt_unit(wrong, u) == -1);
  CHECK(aacs_decrypt_unit(uk, u) == 1);
  plain[0] &= 0x3F;  // marked decrypted
  CHECK(!memcmp(u, plain, AACS_UNIT));
  // Copy permission bits clear: not encrypted, left alone.
  CHECK(aacs_decrypt_unit(uk, u) == 0);
  CHECK(!memcmp(u, plain, AACS_UNIT));
  free(u);
  free(plain);
}

// Unit_Key_RO.inf as discs have it (and libaacs checks): over 2048 bytes,
// application type 1 and one BDMV folder in the header, then where the keys
// are, how many, each 48 bytes on.
#define INF_LEN 4096
#define INF_KEYS 0x30

static size_t make_inf(uint8_t *inf, const uint8_t vuk[16], const uint8_t uk[][16], unsigned n) {
  struct rip_aes128 aes;
  unsigned i;

  memset(inf, 0, INF_LEN);
  inf[3] = INF_KEYS;
  inf[16] = 1;
  inf[17] = 1;
  inf[INF_KEYS + 1] = (uint8_t)n;
  rip_aes128_init_enc(&aes, vuk);
  for (i = 0; i < n; i++) {
    rip_aes128_encrypt_block(&aes, uk[i], inf + INF_KEYS + 48 * (i + 1));
  }
  return INF_LEN;
}

static void test_keys(void) {
  uint8_t vuk[16];
  uint8_t uk[2][16];
  uint8_t got[4][16];
  uint8_t inf[INF_LEN];
  uint8_t id[20];
  struct aacs_keys k;
  char dir[64];
  char path[128];
  size_t len;
  FILE *f;
  int i;

  for (i = 0; i < 16; i++) {
    vuk[i] = (uint8_t)(0xA0 + i);
    uk[0][i] = (uint8_t)(0x10 + i);
    uk[1][i] = (uint8_t)(0x30 + i);
  }
  len = make_inf(inf, vuk, uk, 2);
  // openssl enc -aes-128-ecb of the first unit key with the VUK.
  {
    static const uint8_t enc[16] = {0x84, 0xdf, 0x98, 0x88, 0x44, 0x7c, 0xac, 0x8d,
                                    0x79, 0xea, 0x12, 0x39, 0x72, 0xa2, 0x0a, 0x73};
    CHECK(!memcmp(inf + INF_KEYS + 48, enc, 16));
  }
  CHECK(aacs_unit_keys(inf, len, vuk, got, 4) == 2);
  CHECK(!memcmp(got[0], uk[0], 16) && !memcmp(got[1], uk[1], 16));
  CHECK(aacs_unit_keys(inf, INF_KEYS + 40, vuk, got, 4) == -1);

  test_tmpdir(dir, sizeof(dir));
  snprintf(path, sizeof(path), "%s/KEYDB.cfg", dir);
  f = fopen(path, "w");
  CHECK(f != NULL);
  fprintf(f, "; comment\n");
  fprintf(f,
          "0x0123456789ABCDEF0123456789ABCDEF01234567 = MOVIE ONE | D | 2008-01-01 | V | "
          "0xA0A1A2A3A4A5A6A7A8A9AAABACADAEAF | M | 0x00\n");
  fprintf(f,
          "  0xfedcba9876543210fedcba9876543210fedcba98 = Two Keys | U | 1-0x101112131415161718191A1B1C1D1E1F ; "
          "2-0x303132333435363738393a3b3c3d3e3f | C | 1-0x00\n");
  fprintf(f, "0x1111111111111111111111111111111111111111 = No Keys | D | 2010-01-01\n");
  fclose(f);
  CHECK(aacs_keydb_count(path) == 3);
  rip_unhex("0123456789abcdef0123456789abcdef01234567", id, 20);
  CHECK(aacs_keydb_find(path, id, &k) == RIP_OK);
  CHECK(k.have_vuk && !memcmp(k.vuk, vuk, 16) && k.uk_count == 0);
  CHECK_STR(k.title, "MOVIE ONE");
  rip_unhex("FEDCBA9876543210FEDCBA9876543210FEDCBA98", id, 20);
  CHECK(aacs_keydb_find(path, id, &k) == RIP_OK);
  CHECK(!k.have_vuk && k.uk_count == 2 && !memcmp(k.uk[0], uk[0], 16) && !memcmp(k.uk[1], uk[1], 16));
  rip_unhex("1111111111111111111111111111111111111111", id, 20);
  CHECK(aacs_keydb_find(path, id, &k) == RIP_ENOENT);
  rip_unhex("2222222222222222222222222222222222222222", id, 20);
  CHECK(aacs_keydb_find(path, id, &k) == RIP_ENOENT);
  CHECK(aacs_keydb_find("/nonexistent/KEYDB.cfg", id, &k) == RIP_ERR);
  CHECK(aacs_keydb_count("/nonexistent/KEYDB.cfg") == -1);
  unlink(path);
  rmdir(dir);
}

// A Blu-ray movie disc with two streams, encrypted with one unit key under
// the VUK in a KEYDB.cfg, read whole through the reader in odd-sized reads.
static void test_reader(void) {
  enum { UNITS_A = 5, UNITS_B = 3 };
  uint8_t vuk[16];
  uint8_t uk[1][16];
  uint8_t inf[INF_LEN];
  size_t inf_len;
  uint8_t *a_plain = malloc(UNITS_A * AACS_UNIT);
  uint8_t *b_plain = malloc(UNITS_B * AACS_UNIT);
  uint8_t *a_enc = malloc(UNITS_A * AACS_UNIT);
  uint8_t *b_enc = malloc(UNITS_B * AACS_UNIT);
  struct udfb_file files[4];
  uint8_t *img;
  uint8_t *out;
  size_t len;
  char dir[64];
  char img_path[128];
  char keydb[128];
  char why[160];
  char id_hex[41];
  struct rip_reader base;
  struct rip_reader r;
  struct udf_fs *fs = malloc(sizeof(*fs));
  struct udf_entry *e = malloc(sizeof(*e));
  int fd;
  int i;
  FILE *f;

  for (i = 0; i < 16; i++) {
    vuk[i] = (uint8_t)(0xA0 + i);
    uk[0][i] = (uint8_t)(0x10 + i);
  }
  inf_len = make_inf(inf, vuk, uk, 1);
  for (i = 0; i < UNITS_A; i++) {
    make_unit(a_plain + i * AACS_UNIT, (unsigned)i);
  }
  for (i = 0; i < UNITS_B; i++) {
    make_unit(b_plain + i * AACS_UNIT, (unsigned)(100 + i));
  }
  memcpy(a_enc, a_plain, UNITS_A * AACS_UNIT);
  memcpy(b_enc, b_plain, UNITS_B * AACS_UNIT);
  for (i = 0; i < UNITS_A; i++) {
    aacs_encrypt_unit(uk[0], a_enc + i * AACS_UNIT);
  }
  for (i = 0; i < UNITS_B; i++) {
    aacs_encrypt_unit(uk[0], b_enc + i * AACS_UNIT);
  }
  files[0] = (struct udfb_file){"AACS/Unit_Key_RO.inf", inf, inf_len};
  files[1] = (struct udfb_file){"BDMV/index.bdmv", (const uint8_t *)"INDX0200", 8};
  files[2] = (struct udfb_file){"BDMV/STREAM/00001.m2ts", a_enc, UNITS_A * AACS_UNIT};
  files[3] = (struct udfb_file){"BDMV/STREAM/00002.m2ts", b_enc, UNITS_B * AACS_UNIT};
  img = udfb_build(files, 4, 1, "MOVIE", &len);
  CHECK(img != NULL);

  test_tmpdir(dir, sizeof(dir));
  snprintf(img_path, sizeof(img_path), "%s/disc.iso", dir);
  snprintf(keydb, sizeof(keydb), "%s/KEYDB.cfg", dir);
  CHECK(rip_write_file(img_path, img, len) == 0);
  fd = open(img_path, O_RDONLY);
  CHECK(rip_reader_open_fd(&base, fd, 2048, len, 1) == RIP_OK);

  // No KEYDB.cfg, then one without this disc: the disc ID, and why.
  CHECK(rip_reader_open_aacs(&r, &base, keydb, why, sizeof(why)) == RIP_ENOENT);
  CHECK(strstr(why, "no KEYDB.cfg") != NULL);
  {
    struct rip_sha1 h;
    uint8_t id[20];
    rip_sha1_init(&h);
    rip_sha1_update(&h, inf, inf_len);
    rip_sha1_final(&h, id);
    rip_hex(id, 20, id_hex);
  }
  f = fopen(keydb, "w");
  fprintf(f, "0x2222222222222222222222222222222222222222 = OTHER | V | 0x00000000000000000000000000000000\n");
  fclose(f);
  CHECK(rip_reader_open_aacs(&r, &base, keydb, why, sizeof(why)) == RIP_ENOENT);
  CHECK(strstr(why, "has no key") != NULL && strstr(why, id_hex) != NULL);
  f = fopen(keydb, "a");
  fprintf(f, "0x%s = THE MOVIE | V | 0xA0A1A2A3A4A5A6A7A8A9AAABACADAEAF\n", id_hex);
  fclose(f);
  CHECK(rip_reader_open_aacs(&r, &base, keydb, why, sizeof(why)) == RIP_OK);

  // Read it all, a few sectors at a time, so reads start and end mid-unit.
  out = malloc(len);
  {
    size_t at = 0;
    const uint32_t steps[] = {1, 2, 5, 7, 3};
    unsigned s = 0;
    while (at < len / 2048) {
      uint32_t n = steps[s++ % 5];
      if (at + n > len / 2048) {
        n = (uint32_t)(len / 2048 - at);
      }
      CHECK(rip_reader_read(&r, at, n, out + at * 2048) == RIP_OK);
      at += n;
    }
  }
  for (i = 0; i < UNITS_A; i++) {
    a_plain[i * AACS_UNIT] &= 0x3F;
  }
  for (i = 0; i < UNITS_B; i++) {
    b_plain[i * AACS_UNIT] &= 0x3F;
  }
  CHECK(udf_mount(fs, &base) == RIP_OK);
  CHECK(udf_lookup(fs, "BDMV/STREAM/00001.m2ts", e) == RIP_OK);
  CHECK(!memcmp(out + (size_t)e->extents[0].lba * 2048, a_plain, UNITS_A * AACS_UNIT));
  CHECK(udf_lookup(fs, "BDMV/STREAM/00002.m2ts", e) == RIP_OK);
  CHECK(!memcmp(out + (size_t)e->extents[0].lba * 2048, b_plain, UNITS_B * AACS_UNIT));
  // Everything else as it was.
  CHECK(udf_lookup(fs, "AACS/Unit_Key_RO.inf", e) == RIP_OK);
  CHECK(!memcmp(out + (size_t)e->extents[0].lba * 2048, inf, inf_len));
  CHECK(!memcmp(out, img, 32 * 2048));
  rip_reader_close(&r);

  // A wrong VUK: the unit keys come out wrong, and reading the streams fails.
  f = fopen(keydb, "w");
  fprintf(f, "0x%s = THE MOVIE | V | 0x000102030405060708090A0B0C0D0E0F\n", id_hex);
  fclose(f);
  CHECK(rip_reader_open_aacs(&r, &base, keydb, why, sizeof(why)) == RIP_OK);
  CHECK(udf_lookup(fs, "BDMV/STREAM/00001.m2ts", e) == RIP_OK);
  CHECK(rip_reader_read(&r, e->extents[0].lba, 3, out) == RIP_ERR);
  rip_reader_close(&r);

  // A disc with no AACS folder.
  {
    struct udfb_file plain[] = {{"BDMV/index.bdmv", (const uint8_t *)"INDX0200", 8}};
    uint8_t *img2 = udfb_build(plain, 1, 1, "MOVIE", &len);
    struct rip_reader b2;
    int fd2;
    CHECK(rip_write_file(img_path, img2, len) == 0);
    fd2 = open(img_path, O_RDONLY);
    CHECK(rip_reader_open_fd(&b2, fd2, 2048, len, 1) == RIP_OK);
    CHECK(rip_reader_open_aacs(&r, &b2, keydb, why, sizeof(why)) == RIP_ENOTSUP);
    rip_reader_close(&b2);
    free(img2);
  }

  rip_reader_close(&base);
  unlink(img_path);
  unlink(keydb);
  rmdir(dir);
  free(out);
  free(img);
  free(fs);
  free(e);
  free(a_plain);
  free(b_plain);
  free(a_enc);
  free(b_enc);
}

static void remove_dir(const char *dir) {
  DIR *d = opendir(dir);
  struct dirent *e;

  while (d && (e = readdir(d)) != NULL) {
    char path[512];
    if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) {
      continue;
    }
    snprintf(path, sizeof(path), "%s/%s", dir, e->d_name);
    if (unlink(path) != 0) {
      remove_dir(path);
    }
  }
  if (d) {
    closedir(d);
  }
  rmdir(dir);
}

// The drive test on a 4K disc: AACS 2 asking for bus encryption, the normal
// read garbled by it, OmniDrive's raw read with the unit's head in the clear.
static void test_drive_check(void) {
  uint8_t uk[16];
  uint8_t cert[64];
  uint8_t *stream = malloc(2 * AACS_UNIT);
  struct udfb_file files[3];
  struct fake_drive fake;
  struct rip_transport t;
  struct diag_summary sum;
  struct rip_reader base;
  struct udf_fs *fs = malloc(sizeof(*fs));
  struct udf_entry *e = malloc(sizeof(*e));
  uint8_t *img;
  size_t len;
  char dir[64];
  char path[128];
  int fd;
  int i;

  for (i = 0; i < 16; i++) {
    uk[i] = (uint8_t)(0x10 + i);
  }
  make_unit(stream, 1);
  make_unit(stream + AACS_UNIT, 2);
  aacs_encrypt_unit(uk, stream);
  aacs_encrypt_unit(uk, stream + AACS_UNIT);
  memset(cert, 0, sizeof(cert));
  cert[0] = 0x10;  // AACS 2
  cert[1] = 0x80;  // bus encryption enabled
  files[0] = (struct udfb_file){"AACS/Content000.cer", cert, sizeof(cert)};
  files[1] = (struct udfb_file){"BDMV/index.bdmv", (const uint8_t *)"INDX0300", 8};
  files[2] = (struct udfb_file){"BDMV/STREAM/00001.m2ts", stream, 2 * AACS_UNIT};
  img = udfb_build(files, 3, 1, "MOVIE4K", &len);
  test_tmpdir(dir, sizeof(dir));
  snprintf(path, sizeof(path), "%s/disc.iso", dir);
  CHECK(rip_write_file(path, img, len) == 0);
  fd = open(path, O_RDONLY);
  CHECK(rip_reader_open_fd(&base, fd, 2048, len, 1) == RIP_OK);
  CHECK(udf_mount(fs, &base) == RIP_OK && udf_lookup(fs, "BDMV/STREAM/00001.m2ts", e) == RIP_OK);
  rip_reader_close(&base);
  unlink(path);

  memset(&fake, 0, sizeof(fake));
  fake.image = img;
  fake.image_len = len;
  fake.profile = MMC_PROFILE_BD_ROM;
  fake.dvd = 1;
  fake.bd = 1;
  fake.fail_lba = -1;
  fake.omnidrive = 1;
  fake.nintendo = 3;
  fake.bus_from = e->extents[0].lba;
  fake.bus_to = e->extents[0].lba + 6;
  fake_drive_transport(&fake, &t);
  snprintf(path, sizeof(path), "%s/diag", dir);
  CHECK(diag_run(&t, path, NULL, NULL, NULL, &sum) == RIP_OK);
  CHECK(strstr(sum.findings, "Blu-ray movie: AACS 2 (4K), bus encryption required") != NULL);
  CHECK(strstr(sum.findings, "Movie stream, normal read: scrambled from the first byte (bus encryption)") != NULL);
  CHECK(strstr(sum.findings, "Movie stream, OmniDrive raw read: AACS data with the unit's head in the clear") != NULL);
  CHECK(strstr(sum.findings, "EDC mismatch") == NULL);

  // Without bus encryption, the normal read shows it too.
  fake.bus_to = 0;
  CHECK(diag_run(&t, path, NULL, NULL, NULL, &sum) == RIP_OK);
  CHECK(strstr(sum.findings, "Movie stream, normal read: AACS data with the unit's head in the clear") != NULL);

  remove_dir(dir);
  free(img);
  free(fs);
  free(e);
  free(stream);
}

void test_aacs(void) {
  test_units();
  test_keys();
  test_reader();
  test_drive_check();
}
