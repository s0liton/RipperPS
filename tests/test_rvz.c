// RVZ images (src/core/rvz.c): a small GameCube disc with files, an FST and
// generator padding in the gaps, written and read back by a minimal reader
// here, and by Dolphin's own dolphin-tool when it is installed.
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <zstd.h>

#include "ripper/common.h"
#include "ripper/hash.h"
#include "ripper/rvz.h"
#include "ripper/util.h"
#include "test.h"

#define CHUNK 0x20000
#define BLOCK 0x8000
#define GC_SIZE 0x168000  // 11.25 chunks: the last one is short
#define WII_SIZE 0x80000

// The padding generator, written from the description in Dolphin's
// docs/WiaAndRvz.md rather than shared with the writer.
struct gen {
  uint32_t b[521];
  size_t pos;
};

static void gen_advance(struct gen *g) {
  size_t i;

  for (i = 0; i < 32; i++) {
    g->b[i] ^= g->b[i + 521 - 32];
  }
  for (i = 32; i < 521; i++) {
    g->b[i] ^= g->b[i - 32];
  }
}

static void gen_seed(struct gen *g, const uint32_t seed[17]) {
  size_t i;

  memcpy(g->b, seed, 17 * sizeof(uint32_t));
  for (i = 17; i < 521; i++) {
    g->b[i] = (g->b[i - 17] << 23) ^ (g->b[i - 16] >> 9) ^ g->b[i - 1];
  }
  for (i = 0; i < 4; i++) {
    gen_advance(g);
  }
  g->pos = 0;
}

static uint8_t gen_byte(struct gen *g) {
  static const int shift[4] = {24, 18, 8, 0};  // 18, not 16, as the discs have it
  uint8_t v = (uint8_t)(g->b[g->pos / 4] >> shift[g->pos % 4]);

  if (++g->pos == 521 * 4) {
    gen_advance(g);
    g->pos = 0;
  }
  return v;
}

// Each 32 KiB block has its own seed. Discs derive it from the disc ID and
// the block; this stand-in does too. The writer finds seeds from the data, so
// the derivation itself doesn't matter here.
static void block_seed(const uint8_t *id, uint32_t block, uint32_t seed[17]) {
  uint32_t x = rip_be32(id) * 0x260BCD5u ^ block;
  int i, j;

  for (i = 0; i < 17; i++) {
    uint32_t v = 0;
    for (j = 0; j < 32; j++) {
      x = x * 0x5D588B65u + 1;
      v = (v >> 1) | (x & 0x80000000u);
    }
    seed[i] = v;
  }
}

static void fill_junk(uint8_t *img, size_t size) {
  uint32_t seed[17];
  struct gen g;
  size_t off, i;

  for (off = 0; off < size; off += BLOCK) {
    block_seed(img, (uint32_t)(off / BLOCK), seed);
    gen_seed(&g, seed);
    for (i = 0; i < BLOCK && off + i < size; i++) {
      img[off + i] = gen_byte(&g);
    }
  }
}

static void fill_random(uint8_t *p, size_t n, uint32_t x) {
  size_t i;

  for (i = 0; i < n; i++) {
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    p[i] = (uint8_t)x;
  }
}

static void fst_entry(uint8_t *p, uint32_t name, uint32_t off, uint32_t size) {
  rip_put_be32(p, name);
  rip_put_be32(p + 4, off);
  rip_put_be32(p + 8, size);
}

// Padding everywhere, then the header, apploader, FST and four files over it:
// one of random bytes ending mid-word, one of zeros and one of 0xAA (whole
// chunks each), one ending mid-block with zeros after it.
static uint8_t *make_gc(void) {
  static const char names[] = "a.bin\0zero.bin\0fill.bin\0tail.bin\0";
  uint8_t *img = malloc(GC_SIZE);
  uint8_t *fst;
  uint32_t fst_size = 5 * 12 + sizeof(names);

  memcpy(img, "GRVE01", 6);  // the seed needs the ID before the padding
  fill_junk(img, GC_SIZE);
  memset(img, 0, 0x2440);
  memcpy(img, "GRVE01", 6);
  rip_put_be32(img + 0x1C, 0xC2339F3Du);
  memcpy(img + 0x20, "RIPPER RVZ TEST", 15);
  rip_put_be32(img + 0x420, 0x2440);
  rip_put_be32(img + 0x424, 0x10000);
  rip_put_be32(img + 0x428, fst_size);
  rip_put_be32(img + 0x42C, fst_size);
  fill_random(img + 0x2440, 0xBC0, 1);

  fst = img + 0x10000;
  fst_entry(fst, 0x01000000, 0, 5);
  fst_entry(fst + 12, 0, 0x20000, 0x5123);
  fst_entry(fst + 24, 6, 0x40000, 0x40000);
  fst_entry(fst + 36, 15, 0x80000, 0x40000);
  fst_entry(fst + 48, 24, 0xC4000, 0x1001);
  memcpy(fst + 60, names, sizeof(names));

  fill_random(img + 0x20000, 0x5123, 2);
  memset(img + 0x40000, 0, 0x40000);
  memset(img + 0x80000, 0xAA, 0x40000);
  fill_random(img + 0xC4000, 0x1001, 3);
  memset(img + 0xC5001, 0, 0x1F);
  return img;
}

struct rvz_info {
  uint32_t disc_type;
  uint32_t groups;
  uint32_t packed_groups;  // groups with RVZ packing
  uint32_t junk_runs;      // padding runs stored as seeds
  uint8_t *group_table;
};

// Expands RVZ packing into out, which starts offset bytes into the image.
static int unpack(const uint8_t *in, size_t n, uint8_t *out, size_t out_len, uint64_t offset, uint32_t *runs) {
  size_t i = 0, o = 0;

  while (i < n) {
    uint32_t size;
    if (n - i < 4) {
      return -1;
    }
    size = rip_be32(in + i) & 0x7FFFFFFFu;
    if (o + size > out_len) {
      return -1;
    }
    if (in[i] & 0x80) {
      uint32_t seed[17];
      struct gen g;
      size_t k;
      if (n - i < 4 + 68) {
        return -1;
      }
      for (k = 0; k < 17; k++) {
        seed[k] = rip_be32(in + i + 4 + k * 4);
      }
      gen_seed(&g, seed);
      for (k = 0; k < (offset + o) % BLOCK; k++) {
        gen_byte(&g);
      }
      for (k = 0; k < size; k++) {
        out[o + k] = gen_byte(&g);
      }
      i += 4 + 68;
      (*runs)++;
    } else {
      if (n - i - 4 < size) {
        return -1;
      }
      memcpy(out + o, in + i + 4, size);
      i += 4 + size;
    }
    o += size;
  }
  return o == out_len ? 0 : -1;
}

static int sha1_is(const uint8_t *data, size_t n, const uint8_t *want) {
  struct rip_sha1 s;
  uint8_t got[20];

  rip_sha1_init(&s);
  rip_sha1_update(&s, data, n);
  rip_sha1_final(&s, got);
  return memcmp(got, want, 20) == 0;
}

static uint64_t be64(const uint8_t *p) {
  return ((uint64_t)rip_be32(p) << 32) | rip_be32(p + 4);
}

// A minimal reader: checks the headers and their hashes, returns the image.
static uint8_t *read_rvz(const char *path, uint64_t *iso_size, struct rvz_info *info) {
  size_t len;
  uint8_t *f = test_read_file(path, &len);
  const uint8_t *h1 = f;
  const uint8_t *h2 = f + 0x48;
  uint8_t raw[0x18];
  uint8_t *img = NULL;
  uint32_t i, groups;
  size_t got;

  memset(info, 0, sizeof(*info));
  if (!f || len < 0x48 + 0xDC || memcmp(h1, "RVZ\x01", 4) != 0) {
    free(f);
    return NULL;
  }
  CHECK(rip_be32(h1 + 4) == 0x01000000u);
  CHECK(rip_be32(h1 + 0xC) == 0xDC);
  CHECK(sha1_is(h1, 0x34, h1 + 0x34));
  CHECK(sha1_is(h2, 0xDC, h1 + 0x10));
  CHECK(be64(h1 + 0x2C) == len);
  *iso_size = be64(h1 + 0x24);
  info->disc_type = rip_be32(h2);
  CHECK(rip_be32(h2 + 4) == 5);  // zstd
  CHECK(rip_be32(h2 + 0xC) == CHUNK);
  CHECK(rip_be32(h2 + 0x90) == 0);  // no Wii partition entries
  CHECK(sha1_is((const uint8_t *)"", 0, h2 + 0xA0));
  CHECK(rip_be32(h2 + 0xB4) == 1);
  got = ZSTD_decompress(raw, sizeof(raw), f + be64(h2 + 0xB8), rip_be32(h2 + 0xC0));
  CHECK(got == sizeof(raw));
  CHECK(be64(raw) == 0x80 && be64(raw + 8) == *iso_size - 0x80 && rip_be32(raw + 16) == 0);
  groups = rip_be32(h2 + 0xC4);
  CHECK(rip_be32(raw + 20) == groups && groups == (*iso_size + CHUNK - 1) / CHUNK);
  info->groups = groups;
  info->group_table = malloc((size_t)groups * 12);
  got = ZSTD_decompress(info->group_table, (size_t)groups * 12, f + be64(h2 + 0xC8), rip_be32(h2 + 0xD0));
  CHECK(got == (size_t)groups * 12);

  img = malloc(*iso_size);
  for (i = 0; i < groups && got == (size_t)groups * 12; i++) {
    const uint8_t *e = info->group_table + (size_t)i * 12;
    uint64_t start = (uint64_t)i * CHUNK;
    size_t n = *iso_size - start < CHUNK ? (size_t)(*iso_size - start) : CHUNK;
    uint32_t size = rip_be32(e + 4) & 0x7FFFFFFFu;
    uint32_t packed = rip_be32(e + 8);
    const uint8_t *data = f + (uint64_t)rip_be32(e) * 4;
    uint8_t *buf = NULL;

    if (size == 0) {
      memset(img + start, 0, n);
      continue;
    }
    CHECK((uint64_t)rip_be32(e) * 4 + size <= len);
    if (rip_be32(e + 4) & 0x80000000u) {
      size_t want = packed ? packed : n;
      buf = malloc(want);
      CHECK(ZSTD_decompress(buf, want, data, size) == want);
      data = buf;
      size = (uint32_t)want;
    }
    if (packed) {
      CHECK(size == packed);
      CHECK(unpack(data, size, img + start, n, start, &info->junk_runs) == 0);
      info->packed_groups++;
    } else {
      CHECK(size == n);
      memcpy(img + start, data, n);
    }
    free(buf);
  }
  CHECK(memcmp(img, h2 + 0x10, 0x80) == 0);
  free(f);
  return img;
}

static int write_file(const char *path, const uint8_t *data, size_t len) {
  int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
  int ok = fd >= 0 && rip_write_all(fd, data, len) == 0;

  if (fd >= 0) {
    close(fd);
  }
  return ok ? 0 : -1;
}

struct progress {
  int calls;
  uint64_t done, total;
  int stop_after;
};

static int on_progress(void *ud, uint64_t done, uint64_t total) {
  struct progress *p = ud;

  p->calls++;
  p->done = done;
  p->total = total;
  return p->stop_after && p->calls >= p->stop_after;
}

static int have_dolphin_tool(void) {
  return system("command -v dolphin-tool >/dev/null 2>&1") == 0;
}

// Dolphin's own reader: back to an ISO, compared byte for byte.
static void dolphin_round_trip(const char *dir, const char *rvz, const uint8_t *img, size_t size) {
  char cmd[600];
  char iso[300];
  uint8_t *back;
  size_t len;

  snprintf(iso, sizeof(iso), "%s/dolphin.iso", dir);
  snprintf(cmd, sizeof(cmd), "dolphin-tool convert -i '%s' -o '%s' -f iso >/dev/null 2>&1", rvz, iso);
  CHECK(system(cmd) == 0);
  back = test_read_file(iso, &len);
  CHECK(back && len == size && memcmp(back, img, size) == 0);
  free(back);
  unlink(iso);
}

static void test_gc(const char *dir) {
  uint8_t *img = make_gc();
  char iso[300], rvz[300];
  struct progress p = {0, 0, 0, 0};
  struct rvz_info info;
  uint64_t size = 0;
  uint8_t *back;
  size_t rvz_len;
  uint8_t *file;

  snprintf(iso, sizeof(iso), "%s/gc.iso", dir);
  snprintf(rvz, sizeof(rvz), "%s/gc.rvz", dir);
  CHECK(write_file(iso, img, GC_SIZE) == 0);
  CHECK(rvz_from_iso(iso, rvz, 5, on_progress, &p) == RIP_OK);
  CHECK(p.calls == 12 && p.done == GC_SIZE && p.total == GC_SIZE);

  back = read_rvz(rvz, &size, &info);
  CHECK(back && size == GC_SIZE && memcmp(back, img, GC_SIZE) == 0);
  CHECK(info.disc_type == 1);
  CHECK(info.groups == 12);
  if (info.group_table) {
    const uint8_t *t = info.group_table;
    // zero.bin: two chunks of nothing; fill.bin: one chunk stored, then shared.
    CHECK(rip_be32(t + 2 * 12 + 4) == 0 && rip_be32(t + 3 * 12 + 4) == 0);
    CHECK(rip_be32(t + 4 * 12 + 4) != 0 && memcmp(t + 4 * 12, t + 5 * 12, 12) == 0);
    // The padding went in as seeds: chunks of nothing else are tiny.
    CHECK(rip_be32(t + 8 * 12 + 8) != 0 && (rip_be32(t + 8 * 12 + 4) & 0x7FFFFFFFu) < 512);
    CHECK(rip_be32(t + 11 * 12 + 8) != 0);
  }
  // Every chunk with padding is packed: 0, 1 (after a.bin), 6 (after tail.bin,
  // past its zeros), 7 to 11; and the file is a fraction of the image.
  CHECK(info.packed_groups == 8);
  CHECK(info.junk_runs >= 25);
  file = test_read_file(rvz, &rvz_len);
  CHECK(file && rvz_len < GC_SIZE / 8);
  free(file);
  if (have_dolphin_tool()) {
    dolphin_round_trip(dir, rvz, img, GC_SIZE);
  }
  free(info.group_table);
  free(back);

  // Stopped from the progress callback: nothing left behind.
  p.calls = 0;
  p.stop_after = 3;
  CHECK(rvz_from_iso(iso, rvz, 5, on_progress, &p) == RIP_ECANCEL);
  CHECK(p.calls == 3);
  CHECK(!rip_file_exists(rvz));
  unlink(iso);
  free(img);
}

// Wii: everything stays raw data, the encrypted partitions as they are.
static void test_wii(const char *dir) {
  uint8_t *img = malloc(WII_SIZE);
  char iso[300], rvz[300];
  struct rvz_info info;
  uint64_t size = 0;
  uint8_t *back;

  fill_random(img, WII_SIZE, 4);
  memset(img, 0, 0x100);
  memcpy(img, "RRVE01", 6);
  rip_put_be32(img + 0x18, 0x5D1C9EA3u);
  // One partition at 0x50000, its contents random like encrypted data.
  memset(img + 0x40000, 0, 0x20);
  rip_put_be32(img + 0x40000, 1);
  rip_put_be32(img + 0x40004, 0x40020 >> 2);
  memset(img + 0x40020, 0, 8);
  rip_put_be32(img + 0x40020, 0x50000 >> 2);
  snprintf(iso, sizeof(iso), "%s/wii.iso", dir);
  snprintf(rvz, sizeof(rvz), "%s/wii.rvz", dir);
  CHECK(write_file(iso, img, WII_SIZE) == 0);
  CHECK(rvz_from_iso(iso, rvz, 3, NULL, NULL) == RIP_OK);
  back = read_rvz(rvz, &size, &info);
  CHECK(back && size == WII_SIZE && memcmp(back, img, WII_SIZE) == 0);
  CHECK(info.disc_type == 2 && info.packed_groups == 0);
  if (have_dolphin_tool()) {
    dolphin_round_trip(dir, rvz, img, WII_SIZE);
  }
  free(info.group_table);
  free(back);

  // Neither a GameCube nor a Wii disc: refused, no file made.
  unlink(rvz);
  memset(img, 0, 0x100);
  CHECK(write_file(iso, img, WII_SIZE) == 0);
  CHECK(rvz_from_iso(iso, rvz, 3, NULL, NULL) == RIP_ENOTSUP);
  CHECK(!rip_file_exists(rvz));
  unlink(iso);
  free(img);
}

void test_rvz(void) {
  char dir[64];

  test_tmpdir(dir, sizeof(dir));
  test_gc(dir);
  test_wii(dir);
  rmdir(dir);
}
