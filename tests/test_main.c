#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "iso_builder.h"
#include "ripper/aes.h"
#include "ripper/common.h"
#include "ripper/disc.h"
#include "ripper/hash.h"
#include "ripper/iso9660.h"
#include "ripper/log.h"
#include "ripper/ps3.h"
#include "ripper/reader.h"
#include "ripper/sfo.h"
#include "ripper/util.h"
#include "test.h"

int test_failures;

void test_tmpdir(char *dir, size_t len) {
  snprintf(dir, len, "./test-tmp-XXXXXX");
  if (!mkdtemp(dir)) {
    perror("mkdtemp");
    exit(1);
  }
}

unsigned char *test_read_file(const char *path, size_t *len) {
  FILE *f = fopen(path, "rb");
  unsigned char *buf;
  long n;

  *len = 0;
  if (!f) {
    return NULL;
  }
  fseek(f, 0, SEEK_END);
  n = ftell(f);
  fseek(f, 0, SEEK_SET);
  buf = malloc(n > 0 ? (size_t)n : 1);
  if (buf && fread(buf, 1, (size_t)n, f) != (size_t)n) {
    free(buf);
    buf = NULL;
  }
  fclose(f);
  if (buf) {
    *len = (size_t)n;
  }
  return buf;
}

static void unhex(const char *hex, uint8_t *out, size_t len) {
  if (rip_unhex(hex, out, len) != 0) {
    fprintf(stderr, "bad test vector %s\n", hex);
    exit(1);
  }
}

static void test_aes(void) {
  struct rip_aes128 enc;
  struct rip_aes128 dec;
  uint8_t key[16], pt[16], ct[16], out[16], iv[16];
  uint8_t buf[32];

  // FIPS-197 appendix C.1
  unhex("000102030405060708090a0b0c0d0e0f", key, 16);
  unhex("00112233445566778899aabbccddeeff", pt, 16);
  unhex("69c4e0d86a7b0430d8cdb78070b4c55a", ct, 16);
  rip_aes128_init_enc(&enc, key);
  rip_aes128_init_dec(&dec, key);
  rip_aes128_encrypt_block(&enc, pt, out);
  CHECK_MEM(out, ct, 16);
  rip_aes128_decrypt_block(&dec, ct, out);
  CHECK_MEM(out, pt, 16);

  // NIST SP 800-38A F.2.1 / F.2.2 (first two blocks)
  unhex("2b7e151628aed2a6abf7158809cf4f3c", key, 16);
  unhex("6bc1bee22e409f96e93d7e117393172aae2d8a571e03ac9c9eb76fac45af8e51", buf, 32);
  rip_aes128_init_enc(&enc, key);
  rip_aes128_init_dec(&dec, key);
  unhex("000102030405060708090a0b0c0d0e0f", iv, 16);
  rip_aes128_cbc_encrypt(&enc, iv, buf, 32);
  {
    uint8_t want[32];
    unhex("7649abac8119b246cee98e9b12e9197d5086cb9b507219ee95db113a917678b2", want, 32);
    CHECK_MEM(buf, want, 32);
  }
  unhex("000102030405060708090a0b0c0d0e0f", iv, 16);
  rip_aes128_cbc_decrypt(&dec, iv, buf, 32);
  {
    uint8_t want[32];
    unhex("6bc1bee22e409f96e93d7e117393172aae2d8a571e03ac9c9eb76fac45af8e51", want, 32);
    CHECK_MEM(buf, want, 32);
  }
}

static void test_hashes(void) {
  struct rip_hash_ctx ctx;
  struct rip_hashes h;
  uint8_t want_md5[16], want_sha1[20];
  static uint8_t big[100000];
  struct rip_hashes whole, split;
  size_t i;

  CHECK(rip_crc32(0, "123456789", 9) == 0xCBF43926u);

  rip_hash_init(&ctx);
  rip_hash_update(&ctx, "abc", 3);
  rip_hash_final(&ctx, &h);
  unhex("900150983cd24fb0d6963f7d28e17f72", want_md5, 16);
  unhex("a9993e364706816aba3e25717850c26c9cd0d89d", want_sha1, 20);
  CHECK_MEM(h.md5, want_md5, 16);
  CHECK_MEM(h.sha1, want_sha1, 20);
  CHECK(h.size == 3);

  // Feeding odd-sized pieces must match one call.
  for (i = 0; i < sizeof(big); i++) {
    big[i] = (uint8_t)(i * 31 + 7);
  }
  rip_hash_init(&ctx);
  rip_hash_update(&ctx, big, sizeof(big));
  rip_hash_final(&ctx, &whole);
  rip_hash_init(&ctx);
  for (i = 0; i < sizeof(big);) {
    size_t n = (i % 97) + 1;
    if (i + n > sizeof(big)) {
      n = sizeof(big) - i;
    }
    rip_hash_update(&ctx, big + i, n);
    i += n;
  }
  rip_hash_final(&ctx, &split);
  CHECK(whole.crc32 == split.crc32);
  CHECK_MEM(whole.md5, split.md5, 16);
  CHECK_MEM(whole.sha1, split.sha1, 20);
}

static void test_names(void) {
  char a[64] = "Fallout: New Vegas (BLUS-30500)";
  char b[64] = "What? <A|B> 12:30. ";
  char c[8] = "  ...";

  rip_sanitize_name(a, sizeof(a));
  CHECK_STR(a, "Fallout - New Vegas (BLUS-30500)");
  rip_sanitize_name(b, sizeof(b));
  CHECK_STR(b, "What_ _A_B_ 12-30");
  rip_sanitize_name(c, sizeof(c));
  CHECK_STR(c, "disc");
}

static void test_sha256(void) {
  struct rip_sha256 c;
  uint8_t out[32];
  uint8_t want[32];
  static uint8_t million[1000000];

  // FIPS 180-2 examples: "abc", and a million 'a'.
  rip_sha256_init(&c);
  rip_sha256_update(&c, "abc", 3);
  rip_sha256_final(&c, out);
  unhex("ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad", want, 32);
  CHECK_MEM(out, want, 32);
  memset(million, 'a', sizeof(million));
  rip_sha256_init(&c);
  rip_sha256_update(&c, million, 333333);
  rip_sha256_update(&c, million + 333333, sizeof(million) - 333333);
  rip_sha256_final(&c, out);
  unhex("cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0", want, 32);
  CHECK_MEM(out, want, 32);
}

static void test_system_cnf(void) {
  const char ps2[] = "BOOT2 = cdrom0:\\SLUS_209.46;1\r\nVER = 1.00\r\nVMODE = NTSC\r\n";
  const char ps1[] = "BOOT = cdrom:\\SCUS_944.91;1\nTCB = 4\n";
  char serial[32];
  int is_ps2 = 0;

  CHECK(rip_parse_system_cnf(ps2, strlen(ps2), &is_ps2, serial, sizeof(serial)) == RIP_OK);
  CHECK(is_ps2);
  CHECK_STR(serial, "SLUS-20946");
  CHECK(rip_parse_system_cnf(ps1, strlen(ps1), &is_ps2, serial, sizeof(serial)) == RIP_OK);
  CHECK(!is_ps2);
  CHECK_STR(serial, "SCUS-94491");
  CHECK(rip_parse_system_cnf("VER = 1\n", 8, &is_ps2, serial, sizeof(serial)) == RIP_ENOENT);
}

// Minimal PARAM.SFO with one string entry, as built by the tests.
size_t test_make_sfo(uint8_t *out, const char *key, const char *value) {
  size_t key_len = strlen(key) + 1;
  size_t key_table = 0x14 + 0x10;
  size_t data_table = (key_table + key_len + 3) & ~(size_t)3;
  size_t value_len = strlen(value) + 1;

  memset(out, 0, data_table + 16);
  memcpy(out, "\0PSF", 4);
  rip_put_le32(out + 4, 0x101);
  rip_put_le32(out + 8, (uint32_t)key_table);
  rip_put_le32(out + 12, (uint32_t)data_table);
  rip_put_le32(out + 16, 1);
  rip_put_le16(out + 0x14, 0);
  rip_put_le16(out + 0x16, 0x0204);
  rip_put_le32(out + 0x18, (uint32_t)value_len);
  rip_put_le32(out + 0x1C, 16);
  rip_put_le32(out + 0x20, 0);
  memcpy(out + key_table, key, key_len);
  memcpy(out + data_table, value, value_len);
  return data_table + 16;
}

static void test_sfo(void) {
  uint8_t sfo[128];
  size_t len = test_make_sfo(sfo, "TITLE_ID", "BLUS30001");
  char out[32];
  char formatted[32];

  CHECK(sfo_valid(sfo, len));
  CHECK(sfo_get_string(sfo, len, "TITLE_ID", out, sizeof(out)) == RIP_OK);
  CHECK_STR(out, "BLUS30001");
  rip_format_title_id(out, formatted, sizeof(formatted));
  CHECK_STR(formatted, "BLUS-30001");
  rip_format_title_id("SLUS-20946", formatted, sizeof(formatted));
  CHECK_STR(formatted, "SLUS-20946");
  CHECK(sfo_get_string(sfo, len, "CATEGORY", out, sizeof(out)) == RIP_ENOENT);
}

static void test_iso9660(void) {
  const uint8_t a[] = "hello";
  static uint8_t b[5000];
  struct isob_file files[] = {
      {"README.TXT", a, 5, 0},
      {"DATA/BIG.BIN", b, sizeof(b), 0},
  };
  struct rip_reader r;
  struct iso_fs fs;
  struct iso_entry e;
  uint8_t *data;
  size_t len;
  size_t img_len;
  uint8_t *img;
  char dir[64];
  char path[128];
  FILE *f;

  memset(b, 0xAB, sizeof(b));
  img = isob_build(files, 2, 40, "TESTVOL", &img_len);
  CHECK(img != NULL);
  test_tmpdir(dir, sizeof(dir));
  snprintf(path, sizeof(path), "%s/t.iso", dir);
  f = fopen(path, "wb");
  fwrite(img, 1, img_len, f);
  fclose(f);
  free(img);

  {
    int fd = open(path, O_RDONLY);
    CHECK(rip_reader_open_fd(&r, fd, 2048, img_len, 1) == RIP_OK);
  }
  CHECK(iso_mount(&fs, &r) == RIP_OK);
  CHECK_STR(fs.volume_id, "TESTVOL");
  CHECK(iso_lookup(&fs, "readme.txt", &e) == RIP_OK);
  CHECK(e.size == 5);
  CHECK(iso_read_file(&fs, &e, 1024, &data, &len) == RIP_OK);
  CHECK(len == 5 && memcmp(data, "hello", 5) == 0);
  free(data);
  CHECK(iso_lookup(&fs, "DATA", &e) == RIP_OK && e.is_dir);
  CHECK(iso_lookup(&fs, "data/big.bin", &e) == RIP_OK && e.size == sizeof(b));
  CHECK(iso_lookup(&fs, "DATA/NOPE.BIN", &e) == RIP_ENOENT);
  rip_reader_close(&r);
  remove(path);
  remove(dir);
}

static void test_ps3_layout(void) {
  uint8_t s0[2048];
  struct ps3_layout l;
  uint8_t key[16];
  uint8_t buf[4 * 2048];
  uint8_t orig[4 * 2048];
  size_t i;

  memset(s0, 0, sizeof(s0));
  rip_put_be32(s0, 2);
  rip_put_be32(s0 + 8, 0);
  rip_put_be32(s0 + 12, 31);
  rip_put_be32(s0 + 16, 48);
  rip_put_be32(s0 + 20, 63);
  CHECK(ps3_parse_layout(s0, &l) == RIP_OK);
  CHECK(l.count == 3);
  CHECK(l.total_sectors == 64);
  CHECK(!ps3_sector_encrypted(&l, 31));
  CHECK(ps3_sector_encrypted(&l, 32));
  CHECK(ps3_sector_encrypted(&l, 47));
  CHECK(!ps3_sector_encrypted(&l, 48));

  // Sectors 30..33: two plain, two encrypted.
  for (i = 0; i < sizeof(buf); i++) {
    buf[i] = (uint8_t)(i ^ (i >> 8));
  }
  memcpy(orig, buf, sizeof(buf));
  memset(key, 0x5A, sizeof(key));
  ps3_crypt_sectors(&l, key, 0, 30, buf, 4);
  CHECK_MEM(buf, orig, 2 * 2048);
  CHECK(memcmp(buf + 2 * 2048, orig + 2 * 2048, 2 * 2048) != 0);
  ps3_crypt_sectors(&l, key, 1, 30, buf, 4);
  CHECK_MEM(buf, orig, sizeof(buf));

  // Overlapping regions are rejected.
  rip_put_be32(s0 + 16, 20);
  CHECK(ps3_parse_layout(s0, &l) == RIP_ERR);
}

static void quiet_sink(enum rip_log_level level, const char *msg, void *user) {
  (void)user;
  if (level >= RIP_LOG_WARN) {
    fprintf(stderr, "  log: %s\n", msg);
  }
}

int main(void) {
  static const struct {
    const char *name;
    void (*fn)(void);
  } tests[] = {
      {"aes", test_aes},
      {"hashes", test_hashes},
      {"system_cnf", test_system_cnf},
      {"names", test_names},
      {"sha256", test_sha256},
      {"sfo", test_sfo},
      {"iso9660", test_iso9660},
      {"ps3_layout", test_ps3_layout},
      {"rip", test_rip_all},
      {"formats", test_formats},
      {"firmware", test_firmware},
      {"identify", test_identify},
      {"langs", test_langs},
      {"iso_extract", test_iso_extract},
      {"split", test_split},
      {"game_images", test_game_images},
      {"wbfs", test_wbfs},
      {"rvz", test_rvz},
      {"aacs", test_aacs},
      {"fpkg", test_fpkg},
  };
  size_t i;

  rip_log_set_sink(quiet_sink, NULL);
  for (i = 0; i < sizeof(tests) / sizeof(tests[0]); i++) {
    int before = test_failures;
    tests[i].fn();
    printf("%-12s %s\n", tests[i].name, test_failures == before ? "ok" : "FAILED");
  }
  if (test_failures) {
    printf("%d check(s) failed\n", test_failures);
    return 1;
  }
  printf("all tests passed\n");
  return 0;
}
