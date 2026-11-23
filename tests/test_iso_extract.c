// A PS3 disc's tree extracted (src/core/iso9660.c iso_extract): Joliet names,
// a file in two extents, and a name that would climb out of the folder.
#include <dirent.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "ripper/common.h"
#include "ripper/iso9660.h"
#include "ripper/util.h"
#include "test.h"

#define SECTORS 40

static void both32(uint8_t *p, uint32_t v) {
  p[0] = (uint8_t)v;
  p[1] = (uint8_t)(v >> 8);
  p[2] = (uint8_t)(v >> 16);
  p[3] = (uint8_t)(v >> 24);
  p[4] = (uint8_t)(v >> 24);
  p[5] = (uint8_t)(v >> 16);
  p[6] = (uint8_t)(v >> 8);
  p[7] = (uint8_t)v;
}

// A directory record; name is raw bytes (UCS-2 for Joliet).
static size_t rec(uint8_t *p, uint32_t lba, uint32_t size, uint8_t flags, const uint8_t *name, uint8_t name_len) {
  size_t len = 33u + name_len + ((33u + name_len) & 1);
  memset(p, 0, len);
  p[0] = (uint8_t)len;
  both32(p + 2, lba);
  both32(p + 10, size);
  p[25] = flags;
  p[28] = 1;
  p[32] = name_len;
  memcpy(p + 33, name, name_len);
  return len;
}

static uint8_t ucs2[128];
static uint8_t *u(const char *s, uint8_t *len) {
  uint8_t n = 0;
  for (; *s; s++) {
    ucs2[n++] = 0;
    ucs2[n++] = (uint8_t)*s;
  }
  *len = n;
  return ucs2;
}

static size_t dots(uint8_t *p, uint32_t lba) {
  static const uint8_t dot = 0, dotdot = 1;
  size_t o = rec(p, lba, 2048, 2, &dot, 1);
  return o + rec(p + o, lba, 2048, 2, &dotdot, 1);
}

void test_iso_extract(void) {
  uint8_t *img = calloc(SECTORS, 2048);
  uint8_t *p;
  uint8_t n;
  size_t o;
  char dir[64];
  char path[300];
  char img_path[300];
  struct rip_reader r;
  struct iso_fs fs;
  uint8_t *got;
  size_t len;
  int fd;
  int i;

  // Descriptors: primary at 16, Joliet at 17, the terminator at 18.
  p = img + 16 * 2048;
  p[0] = 1;
  memcpy(p + 1, "CD001", 5);
  p[6] = 1;
  memcpy(p + 40, "PS3VOLUME", 9);
  both32(p + 80, SECTORS);
  {
    static const uint8_t root = 0;
    rec(p + 156, 19, 2048, 2, &root, 1);
  }
  p = img + 17 * 2048;
  p[0] = 2;
  memcpy(p + 1, "CD001", 5);
  p[6] = 1;
  p[88] = 0x25;
  p[89] = 0x2F;
  p[90] = 0x45;
  {
    static const uint8_t root = 0;
    rec(p + 156, 20, 2048, 2, &root, 1);
  }
  p = img + 18 * 2048;
  p[0] = 0xFF;
  memcpy(p + 1, "CD001", 5);

  // The primary tree (8.3 names), only so the image is whole: Joliet is used.
  p = img + 19 * 2048;
  o = dots(p, 19);
  rec(p + o, 30, 5, 0, (const uint8_t *)"PS3_DISC.SFB;1", 14);

  // Joliet: the root, then PS3_GAME/ with a long name and a two-extent file.
  p = img + 20 * 2048;
  o = dots(p, 20);
  {
    const uint8_t *nm = u("PS3_DISC.SFB", &n);
    o += rec(p + o, 30, 5, 0, nm, n);
  }
  {
    const uint8_t *nm = u("PS3_GAME", &n);
    o += rec(p + o, 21, 2048, 2, nm, n);
  }
  {
    const uint8_t *nm = u("..", &n);
    rec(p + o, 32, 3, 0, nm, n);  // a file named "..": not written
  }
  p = img + 21 * 2048;
  o = dots(p, 21);
  {
    const uint8_t *nm = u("Long File Name.dat", &n);
    o += rec(p + o, 31, 11, 0, nm, n);
  }
  {
    const uint8_t *nm = u("big.bin;1", &n);
    o += rec(p + o, 33, 2048, 0x80, nm, n);  // first extent: a whole sector
  }
  {
    const uint8_t *nm = u("big.bin;1", &n);
    rec(p + o, 35, 4, 0, nm, n);  // the rest
  }

  memcpy(img + 30 * 2048, "SFB!!", 5);
  memcpy(img + 31 * 2048, "long name!!", 11);
  memset(img + 33 * 2048, 'A', 2048);
  memcpy(img + 35 * 2048, "TAIL", 4);

  test_tmpdir(dir, sizeof(dir));
  snprintf(img_path, sizeof(img_path), "%s/ps3.iso", dir);
  fd = open(img_path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
  CHECK(fd >= 0 && rip_write_all(fd, img, (size_t)SECTORS * 2048) == 0);
  close(fd);
  fd = open(img_path, O_RDONLY);
  CHECK(rip_reader_open_fd(&r, fd, 2048, (uint64_t)SECTORS * 2048, 1) == RIP_OK);
  CHECK(iso_mount(&fs, &r) == RIP_OK);
  CHECK(fs.joliet);
  CHECK_STR(fs.volume_id, "PS3VOLUME");
  snprintf(path, sizeof(path), "%s/out", dir);
  CHECK(iso_extract(&fs, path, NULL, NULL) == RIP_OK);
  rip_reader_close(&r);

  snprintf(path, sizeof(path), "%s/out/PS3_DISC.SFB", dir);
  got = test_read_file(path, &len);
  CHECK(got && len == 5 && !memcmp(got, "SFB!!", 5));
  free(got);
  snprintf(path, sizeof(path), "%s/out/PS3_GAME/Long File Name.dat", dir);
  got = test_read_file(path, &len);
  CHECK(got && len == 11 && !memcmp(got, "long name!!", 11));
  free(got);
  snprintf(path, sizeof(path), "%s/out/PS3_GAME/big.bin", dir);
  got = test_read_file(path, &len);
  CHECK(got && len == 2052);
  for (i = 0; got && i < 2048; i++) {
    if (got[i] != 'A') {
      break;
    }
  }
  CHECK(i == 2048 && got && !memcmp(got + 2048, "TAIL", 4));
  free(got);
  // Only the disc's two names in the folder: the ".." file went nowhere.
  {
    DIR *d;
    struct dirent *e;
    int count = 0;
    snprintf(path, sizeof(path), "%s/out", dir);
    d = opendir(path);
    while (d && (e = readdir(d)) != NULL) {
      count += strcmp(e->d_name, ".") != 0 && strcmp(e->d_name, "..") != 0;
    }
    if (d) {
      closedir(d);
    }
    CHECK(count == 2);
  }
  free(img);
}

// A file in FAT32-sized parts (rip_split_file), here 1000-byte ones.
void test_split(void) {
  char dir[64];
  char path[400];
  char prefix[300];
  uint8_t data[2500];
  uint8_t *got;
  size_t len;
  int fd;
  size_t i;

  for (i = 0; i < sizeof(data); i++) {
    data[i] = (uint8_t)(i * 7);
  }
  test_tmpdir(dir, sizeof(dir));
  snprintf(path, sizeof(path), "%s/Game.iso", dir);
  snprintf(prefix, sizeof(prefix), "%s/Game.iso", dir);
  fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
  CHECK(fd >= 0 && rip_write_all(fd, data, sizeof(data)) == 0);
  close(fd);
  CHECK(rip_split_file(path, 1000, prefix) == RIP_OK);
  CHECK(!rip_file_exists(path));
  for (i = 0; i < 3; i++) {
    char part[320];
    snprintf(part, sizeof(part), "%s.%zu", prefix, i);
    got = test_read_file(part, &len);
    CHECK(got && len == (i < 2 ? 1000u : 500u) && !memcmp(got, data + i * 1000, len));
    free(got);
  }
  snprintf(path, sizeof(path), "%s.3", prefix);
  CHECK(!rip_file_exists(path));
  // Small enough already: refused, the file left alone.
  snprintf(path, sizeof(path), "%s/Small.iso", dir);
  fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
  CHECK(fd >= 0 && rip_write_all(fd, data, 10) == 0);
  close(fd);
  CHECK(rip_split_file(path, 1000, path) != RIP_OK && rip_file_exists(path));
}
