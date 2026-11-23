// Images made from a disc's: an Xbox game partition alone (src/core/xiso.c)
// and a GameCube disc without its unused blocks (src/core/ciso.c).
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "ripper/ciso.h"
#include "ripper/common.h"
#include "ripper/sfo.h"
#include "ripper/util.h"
#include "ripper/xiso.h"
#include "test.h"

#define XGD3 0x2080000ull

static void le16(uint8_t *p, uint16_t v) {
  p[0] = (uint8_t)v;
  p[1] = (uint8_t)(v >> 8);
}

static void le32(uint8_t *p, uint32_t v) {
  le16(p, (uint16_t)v);
  le16(p + 2, (uint16_t)(v >> 16));
}

static void be32(uint8_t *p, uint32_t v) {
  p[0] = (uint8_t)(v >> 24);
  p[1] = (uint8_t)(v >> 16);
  p[2] = (uint8_t)(v >> 8);
  p[3] = (uint8_t)v;
}

// One XDVDFS entry at t + at (bytes): its tree links in 4-byte steps.
static size_t xentry(uint8_t *t, size_t at, uint16_t left, uint16_t right, uint32_t sector, uint32_t size,
                     uint8_t attrs, const char *name) {
  size_t n = strlen(name);
  le16(t + at, left);
  le16(t + at + 2, right);
  le32(t + at + 4, sector);
  le32(t + at + 8, size);
  t[at + 12] = attrs;
  t[at + 13] = (uint8_t)n;
  memcpy(t + at + 14, name, n);
  return (14 + n + 3) / 4 * 4;
}

static void put(int fd, uint64_t at, const void *p, size_t n) {
  CHECK(rip_seek_write(fd, at, p, n) == 0);
}

// A game partition at base: the volume descriptor, a root with a file and a
// folder, the folder with one file. The last byte used is the folder's
// file's: sector 70, 3000 bytes, so 72 sectors in all.
static void make_partition(int fd, uint64_t base) {
  uint8_t s[2048];
  size_t at;

  memset(s, 0, sizeof(s));
  memcpy(s, "MICROSOFT*XBOX*MEDIA", 20);
  le32(s + 0x14, 40);
  le32(s + 0x18, 2048);
  memcpy(s + 0x7EC, "MICROSOFT*XBOX*MEDIA", 20);
  put(fd, base + 32 * 2048, s, sizeof(s));

  memset(s, 0xFF, sizeof(s));
  at = xentry(s, 0, 0, 5, 50, 5000, 0x20, "a.xbe");
  CHECK(at == 20);
  xentry(s, at, 0, 0, 60, 2048, 0x10, "sub");
  put(fd, base + 40 * 2048, s, sizeof(s));

  memset(s, 0xFF, sizeof(s));
  xentry(s, 0, 0, 0, 70, 3000, 0x20, "b.bin");
  put(fd, base + 60 * 2048, s, sizeof(s));

  memset(s, 0xA5, sizeof(s));
  put(fd, base + 70 * 2048, s, sizeof(s));
  put(fd, base + 71 * 2048, s, 3000 - 2048);
}

static void test_xiso(void) {
  char dir[64];
  char full[128];
  char plain[128];
  char out[128];
  uint64_t offset = 1;
  uint64_t used = 0;
  unsigned char *data;
  size_t len = 0;
  int fd;

  test_tmpdir(dir, sizeof(dir));
  snprintf(full, sizeof(full), "%s/full.iso", dir);
  snprintf(plain, sizeof(plain), "%s/plain.iso", dir);
  snprintf(out, sizeof(out), "%s/out.iso", dir);

  // Redump's layout of an XGD3 disc: the partition after the video one,
  // and padding past the last file.
  fd = open(full, O_RDWR | O_CREAT | O_TRUNC, 0644);
  CHECK(fd >= 0);
  CHECK(ftruncate(fd, (off_t)(XGD3 + 200 * 2048)) == 0);
  make_partition(fd, XGD3);
  CHECK(xiso_partition(fd, &offset) == RIP_OK);
  CHECK(offset == XGD3);
  CHECK(xiso_used(fd, offset, &used) == RIP_OK);
  CHECK(used == 72 * 2048);
  close(fd);

  CHECK(xiso_from_iso(full, out, NULL, NULL) == RIP_OK);
  data = test_read_file(out, &len);
  CHECK(data && len == 72 * 2048);
  CHECK(data && !memcmp(data + 32 * 2048, "MICROSOFT*XBOX*MEDIA", 20));
  CHECK(data && data[70 * 2048] == 0xA5 && data[71 * 2048 + 3000 - 2049] == 0xA5);
  free(data);

  // An image that's already the partition (a burned copy): cut the same way.
  fd = open(plain, O_RDWR | O_CREAT | O_TRUNC, 0644);
  CHECK(fd >= 0);
  CHECK(ftruncate(fd, 300 * 2048) == 0);
  make_partition(fd, 0);
  CHECK(xiso_partition(fd, &offset) == RIP_OK);
  CHECK(offset == 0);
  close(fd);
  CHECK(xiso_from_iso(plain, out, NULL, NULL) == RIP_OK);
  data = test_read_file(out, &len);
  CHECK(data && len == 72 * 2048);
  free(data);

  // A tree whose links go round in a circle is refused, not followed forever.
  fd = open(plain, O_RDWR);
  {
    uint8_t s[2048];
    // The second entry's right link points back at itself.
    memset(s, 0xFF, sizeof(s));
    xentry(s, 0, 4, 0, 50, 10, 0x20, "a");
    xentry(s, 16, 0, 4, 51, 10, 0x20, "b");
    put(fd, 40 * 2048, s, sizeof(s));
  }
  CHECK(xiso_used(fd, 0, &used) == RIP_ERR);
  close(fd);

  // Not an Xbox disc.
  fd = open(full, O_RDWR | O_TRUNC);
  CHECK(ftruncate(fd, 100 * 2048) == 0);
  CHECK(xiso_partition(fd, &offset) == RIP_ENOENT);
  close(fd);

  unlink(full);
  unlink(plain);
  unlink(out);
  rmdir(dir);
}

// A GameCube image of five 2 MiB blocks: the header, apploader, executable
// and file table in the first, files in the second and fifth.
static void test_ciso(void) {
  char dir[64];
  char iso[128];
  char out[128];
  uint8_t map[CISO_MAP];
  uint8_t b[0x100];
  unsigned char *data;
  size_t len = 0;
  int fd;

  test_tmpdir(dir, sizeof(dir));
  snprintf(iso, sizeof(iso), "%s/gc.iso", dir);
  snprintf(out, sizeof(out), "%s/gc.ciso", dir);
  fd = open(iso, O_RDWR | O_CREAT | O_TRUNC, 0644);
  CHECK(fd >= 0);
  CHECK(ftruncate(fd, 5 * CISO_BLOCK) == 0);

  memset(b, 0, sizeof(b));
  memcpy(b, "GTEST01", 6);
  be32(b + 0x1C, 0xC2339F3D);
  put(fd, 0, b, 0x20);
  be32(b, 0x10000);      // executable
  be32(b + 4, 0x20000);  // file table
  be32(b + 8, 3 * 12 + 16);
  put(fd, 0x420, b, 12);
  memset(b, 0, sizeof(b));
  be32(b + 0x14, 0x1000);  // apploader code and trailer
  be32(b + 0x18, 0x10);
  put(fd, 0x2440, b, 0x20);
  memset(b, 0, sizeof(b));
  be32(b, 0x100);  // text 0 at 0x100, 0x1000 bytes
  be32(b + 0x90, 0x1000);
  put(fd, 0x10000, b, sizeof(b));
  {
    uint8_t fst[3 * 12 + 16];
    memset(fst, 0, sizeof(fst));
    fst[0] = 1;  // the root, three entries
    be32(fst + 8, 3);
    be32(fst + 12 + 4, 0x300000);  // a file in block 1
    be32(fst + 12 + 8, 0x1000);
    be32(fst + 24 + 4, 0x900000);  // and one in block 4
    be32(fst + 24 + 8, 0x1000);
    put(fd, 0x20000, fst, sizeof(fst));
  }
  memset(b, 0x5A, sizeof(b));
  put(fd, 0x900000, b, sizeof(b));

  CHECK(ciso_used_blocks(fd, 5 * CISO_BLOCK, map) == RIP_OK);
  CHECK(map[0] && map[1] && !map[2] && !map[3] && map[4]);
  close(fd);

  CHECK(ciso_from_iso(iso, out, NULL, NULL) == RIP_OK);
  data = test_read_file(out, &len);
  CHECK(data && len == 0x8000 + 3 * CISO_BLOCK);
  CHECK(data && !memcmp(data, "CISO", 4));
  CHECK(data && data[4] == 0 && data[5] == 0 && data[6] == 0x20 && data[7] == 0);
  CHECK(data && data[8] == 1 && data[9] == 1 && data[10] == 0 && data[11] == 0 && data[12] == 1);
  CHECK(data && data[0x8000] == 'G');
  // Block 4 is the third stored.
  CHECK(data && data[0x8000 + 2 * CISO_BLOCK + 0x100000] == 0x5A);
  free(data);

  // Not a GameCube disc.
  fd = open(iso, O_RDWR | O_TRUNC);
  CHECK(ftruncate(fd, CISO_BLOCK) == 0);
  CHECK(ciso_used_blocks(fd, CISO_BLOCK, map) == RIP_ERR);
  close(fd);

  unlink(iso);
  unlink(out);
  rmdir(dir);
}

// A dump with its update says the update's version: APP_VER changed in place.
static void test_sfo_set(void) {
  unsigned char sfo[256];
  char got[16];
  size_t len = test_make_sfo(sfo, "APP_VER", "01.00");

  CHECK(sfo_set_string(sfo, len, "APP_VER", "01.02") == RIP_OK);
  CHECK(sfo_get_string(sfo, len, "APP_VER", got, sizeof(got)) == RIP_OK);
  CHECK_STR(got, "01.02");
  CHECK(sfo_set_string(sfo, len, "APP_VER", "01.02.0000000000000000") == RIP_ERR);
  CHECK(sfo_set_string(sfo, len, "VERSION", "01.02") == RIP_ENOENT);
}

void test_game_images(void) {
  test_xiso();
  test_ciso();
  test_sfo_set();
}
