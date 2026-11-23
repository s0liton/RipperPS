#include "ripper/rvz.h"

#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include <zstd.h>

#include "ripper/common.h"
#include "ripper/hash.h"
#include "ripper/log.h"
#include "ripper/util.h"

// Layout from Dolphin's WIABlob.cpp and docs/WiaAndRvz.md. Everything in the
// file is big-endian.
#define CHUNK 0x20000  // Dolphin's default RVZ chunk size
#define BLOCK 0x8000   // the padding generator restarts every 32 KiB
#define HEADER1_BYTES 0x48
#define HEADER2_BYTES 0xDC
#define DISC_HEAD_BYTES 0x80  // kept in header 2, not in the raw data
#define PART_ENTRY_BYTES 0x30
#define RAW_ENTRY_BYTES 0x18
#define GROUP_ENTRY_BYTES 0x0C
#define COMPRESSION_ZSTD 5
#define RVZ_VERSION 0x01000000u
#define RVZ_VERSION_COMPATIBLE 0x00030000u  // what Dolphin's writer puts
#define GROUP_COMPRESSED 0x80000000u
#define PACKED_JUNK 0x80000000u

#define GC_MAGIC 0xC2339F3Du   // at 0x1C
#define WII_MAGIC 0x5D1C9EA3u  // at 0x18
#define DISC_GC 1
#define DISC_WII 2
#define FST_MAX (4u << 20)  // real ones are well under 1 MiB
#define REUSE_MAX 512       // 256 byte values, for full and short chunks

// The padding's generator: lagged Fibonacci, f = xor, j = 32, k = 521.
#define LFG_K 521
#define LFG_J 32
#define SEED_WORDS 17
#define SEED_BYTES (SEED_WORDS * 4)

struct lfg {
  uint32_t b[LFG_K];  // each word is the next 4 output bytes, big-endian
  size_t pos;         // output byte position in b
};

struct junk {
  size_t start, end;  // within the chunk
  uint32_t seed[SEED_WORDS];
};

struct file_range {
  uint64_t start, end;
};

struct reuse {
  uint32_t size;
  uint8_t value;
  uint8_t entry[GROUP_ENTRY_BYTES];
};

struct rvz {
  const char *iso_path;
  const char *out_path;
  int in_fd;
  int out_fd;
  uint64_t iso_size;
  uint8_t disc_head[DISC_HEAD_BYTES];
  uint32_t disc_type;
  int level;
  ZSTD_CCtx *zstd;
  uint8_t *in;  // one chunk of the image
  uint8_t *packed;
  size_t packed_cap;
  uint8_t *comp;
  size_t comp_cap;
  struct junk *junk;
  size_t junk_count, junk_cap;
  struct file_range *files;  // sorted by end, the order Dolphin looks them up in
  size_t file_count;
  uint8_t *groups;
  uint32_t group_count;
  struct reuse reuse[REUSE_MAX];
  size_t reuse_count;
  uint64_t reserved;  // bytes kept at the start for the tables
  uint64_t written;   // end of the file so far
};

static void put_be64(uint8_t *p, uint64_t v) {
  rip_put_be32(p, (uint32_t)(v >> 32));
  rip_put_be32(p + 4, (uint32_t)v);
}

static void lfg_forward(struct lfg *g) {
  size_t i;

  for (i = 0; i < LFG_J; i++) {
    g->b[i] ^= g->b[i + LFG_K - LFG_J];
  }
  for (i = LFG_J; i < LFG_K; i++) {
    g->b[i] ^= g->b[i - LFG_J];
  }
}

// Undoes lfg_forward for words start..end-1.
static void lfg_backward(struct lfg *g, size_t start, size_t end) {
  size_t stop = start > LFG_J ? start : LFG_J;
  size_t i;

  for (i = end < LFG_K ? end : LFG_K; i > stop; i--) {
    g->b[i - 1] ^= g->b[i - 1 - LFG_J];
  }
  for (i = end < LFG_J ? end : LFG_J; i > start; i--) {
    g->b[i - 1] ^= g->b[i - 1 + LFG_K - LFG_J];
  }
}

// Expands the 17 seed words in b. With check set, the rest of b must already
// hold what the seed expands to, or it fails.
static int lfg_init(struct lfg *g, int check) {
  size_t i;

  for (i = SEED_WORDS; i < LFG_K; i++) {
    uint32_t calc = (g->b[i - 17] << 23) ^ (g->b[i - 16] >> 9) ^ g->b[i - 1];
    if (check) {
      uint32_t actual = (g->b[i] & 0xFF00FFFFu) | ((g->b[i] << 2) & 0x00FC0000u);
      if ((calc & 0xFFFCFFFFu) != actual) {
        return 0;
      }
    }
    g->b[i] = calc;
  }
  // The discs' generator outputs its third byte shifted by 18 instead of 16.
  for (i = 0; i < LFG_K; i++) {
    g->b[i] = (g->b[i] & 0xFF00FFFFu) | ((g->b[i] >> 2) & 0x00FF0000u);
  }
  for (i = 0; i < 4; i++) {
    lfg_forward(g);
  }
  return 1;
}

// b holds the first output words; works back to the seed.
static int lfg_reinit(struct lfg *g, uint32_t seed[SEED_WORDS]) {
  size_t i;

  for (i = 0; i < 4; i++) {
    lfg_backward(g, 0, LFG_K);
  }
  // Bits lost to the shift by 18 come back from the words after.
  for (i = 0; i < SEED_WORDS; i++) {
    g->b[i] =
        (g->b[i] & 0xFF00FFFFu) | ((g->b[i] << 2) & 0x00FC0000u) | (((g->b[i + 16] ^ g->b[i + 15]) << 9) & 0x00030000u);
    seed[i] = g->b[i];
  }
  return lfg_init(g, 1);
}

static uint8_t lfg_byte(struct lfg *g) {
  uint8_t v = (uint8_t)(g->b[g->pos >> 2] >> (24 - 8 * (g->pos & 3)));

  if (++g->pos == LFG_K * 4) {
    lfg_forward(g);
    g->pos = 0;
  }
  return v;
}

// data is the image from offset bytes into a 32 KiB block. When it starts
// with padding, gets the block's seed and returns how many bytes it covers.
static size_t junk_seed(const uint8_t *data, size_t size, size_t offset, uint32_t seed[SEED_WORDS]) {
  struct lfg g;
  size_t skip = (4 - (offset & 3)) & 3;  // whole words only
  size_t word_off = (offset + skip) / 4;
  size_t mod = word_off % LFG_K;
  size_t div = word_off / LFG_K;
  size_t i;
  size_t n = 0;

  if (size < skip + LFG_K * 4) {
    return 0;
  }
  // A quick test first: in padding, bits 22-23 of each word match bits 24-25.
  for (i = 0; i < LFG_K; i++) {
    uint32_t x = rip_be32(data + skip + i * 4);
    if ((x & 0x00C00000u) != ((x >> 2) & 0x00C00000u)) {
      return 0;
    }
  }
  for (i = 0; i < LFG_K; i++) {
    g.b[(mod + i) % LFG_K] = rip_be32(data + skip + i * 4);
  }
  lfg_backward(&g, 0, mod);
  for (i = 0; i < div; i++) {
    lfg_backward(&g, 0, LFG_K);
  }
  if (!lfg_reinit(&g, seed)) {
    return 0;
  }
  for (i = 0; i < div; i++) {
    lfg_forward(&g);
  }
  g.pos = offset % (LFG_K * 4);
  while (n < size && lfg_byte(&g) == data[n]) {
    n++;
  }
  return n;
}

static int cmp_file_end(const void *a, const void *b) {
  const struct file_range *x = a, *y = b;
  if (x->end != y->end) {
    return x->end < y->end ? -1 : 1;
  }
  return x->start < y->start ? -1 : x->start > y->start;
}

// The file holding offset, if any (FileSystemGCWii::FindFileInfo).
static const struct file_range *find_file(const struct rvz *r, uint64_t offset) {
  size_t lo = 0, hi = r->file_count;

  while (lo < hi) {
    size_t mid = lo + (hi - lo) / 2;
    if (r->files[mid].end <= offset) {
      lo = mid + 1;
    } else {
      hi = mid;
    }
  }
  return lo < r->file_count && r->files[lo].start <= offset ? &r->files[lo] : NULL;
}

// GameCube: the FST's file extents, so the padding right after a file that
// ends mid-block is found too. Without them the image is still written, only
// bigger.
static int load_files(struct rvz *r, const uint8_t *head) {
  uint32_t fst_off = rip_be32(head + 0x424);
  uint32_t fst_size = rip_be32(head + 0x428);
  uint8_t *fst;
  uint32_t count;
  uint32_t i;
  size_t n = 0;

  if (fst_size < 12 || fst_size > FST_MAX || (uint64_t)fst_off + fst_size > r->iso_size) {
    RIP_WARN("RVZ: %s has no usable FST (%u bytes at 0x%x)", r->iso_path, fst_size, fst_off);
    return RIP_OK;
  }
  fst = malloc(fst_size);
  if (!fst) {
    return RIP_ENOMEM;
  }
  if (rip_seek_read(r->in_fd, fst_off, fst, fst_size) != 0) {
    RIP_ERROR("RVZ: reading the FST of %s failed: %s", r->iso_path, strerror(errno));
    free(fst);
    return RIP_ERR;
  }
  count = rip_be32(fst + 8);
  if (fst[0] == 0 || (uint64_t)count * 12 > fst_size || fst[fst_size - 1] != 0) {
    RIP_WARN("RVZ: %s has a broken FST", r->iso_path);
    free(fst);
    return RIP_OK;
  }
  r->files = malloc((size_t)count * sizeof(*r->files) + 1);
  if (!r->files) {
    free(fst);
    return RIP_ENOMEM;
  }
  for (i = 1; i < count; i++) {
    const uint8_t *e = fst + (size_t)i * 12;
    uint32_t size = rip_be32(e + 8);
    if (e[0] == 0 && size != 0) {  // a file, not a directory
      r->files[n].start = rip_be32(e + 4);
      r->files[n].end = r->files[n].start + size;
      n++;
    }
  }
  free(fst);
  qsort(r->files, n, sizeof(*r->files), cmp_file_end);
  r->file_count = n;
  return RIP_OK;
}

static int add_junk(struct rvz *r, size_t start, size_t end, const uint32_t seed[SEED_WORDS]) {
  if (r->junk_count == r->junk_cap) {
    size_t cap = r->junk_cap ? r->junk_cap * 2 : 64;
    struct junk *j = realloc(r->junk, cap * sizeof(*j));
    if (!j) {
      return RIP_ENOMEM;
    }
    r->junk = j;
    r->junk_cap = cap;
  }
  r->junk[r->junk_count].start = start;
  r->junk[r->junk_count].end = end;
  memcpy(r->junk[r->junk_count].seed, seed, sizeof(r->junk[0].seed));
  r->junk_count++;
  return RIP_OK;
}

// Finds the padding runs in a chunk at image offset base, block by block, as
// Dolphin's RVZPack does: past any zeros, then from the end of each file.
static int find_junk(struct rvz *r, size_t len, uint64_t base) {
  const uint8_t *in = r->in;
  size_t pos = 0;
  uint64_t off = base;

  r->junk_count = 0;
  while (pos < len) {
    uint32_t seed[SEED_WORDS];
    const struct file_range *f;
    size_t n;
    size_t got;

    while (pos < len && in[pos] == 0) {
      pos++;
      off++;
    }
    n = (size_t)(BLOCK - off % BLOCK);
    if (n > len - pos) {
      n = len - pos;
    }
    got = junk_seed(in + pos, n, (size_t)(off % BLOCK), seed);
    if (got && add_junk(r, pos, pos + got, seed) != RIP_OK) {
      return RIP_ENOMEM;
    }
    f = find_file(r, off + got);
    if (f && f->end < off + n) {
      pos += (size_t)(f->end - off);
      off = f->end;
      continue;
    }
    pos += n;
    off += n;
  }
  return RIP_OK;
}

// RVZ packing: runs of data as a length and the bytes, padding as a length
// with the top bit set and the seed. Returns 0 when the chunk has no padding
// worth packing, and it goes in as it is.
static size_t pack(struct rvz *r, size_t len) {
  uint8_t *out = r->packed;
  size_t cur = 0;
  size_t o = 0;
  size_t j = 0;

  while (cur < len) {
    size_t js = len;
    size_t je = len;
    const struct junk *k = NULL;

    if (len - cur > SEED_BYTES) {
      while (j < r->junk_count && r->junk[j].end <= cur + SEED_BYTES) {
        j++;
      }
      if (j < r->junk_count && r->junk[j].start + SEED_BYTES < len) {
        k = &r->junk[j];
        js = k->start > cur ? k->start : cur;
        je = k->end < len ? k->end : len;
      }
    }
    if (cur == 0 && !k) {
      return 0;
    }
    if (js > cur) {
      rip_put_be32(out + o, (uint32_t)(js - cur));
      memcpy(out + o + 4, r->in + cur, js - cur);
      o += 4 + js - cur;
      cur = js;
    }
    if (je > cur) {
      size_t i;
      rip_put_be32(out + o, (uint32_t)(je - cur) | PACKED_JUNK);
      for (i = 0; i < SEED_WORDS; i++) {
        rip_put_be32(out + o + 4 + i * 4, k->seed[i]);
      }
      o += 4 + SEED_BYTES;
      cur = je;
    }
  }
  return o;
}

static int write_out(struct rvz *r, const void *p, size_t n) {
  if (rip_write_all(r->out_fd, p, n) != 0) {
    RIP_ERROR("RVZ: writing %s failed: %s", r->out_path, strerror(errno));
    return RIP_ERR;
  }
  r->written += n;
  return RIP_OK;
}

static int pad4(struct rvz *r) {
  static const uint8_t zero[4];
  return write_out(r, zero, (size_t)(-r->written & 3));
}

static int grow(uint8_t **buf, size_t *cap, size_t need) {
  uint8_t *p;

  if (need <= *cap) {
    return RIP_OK;
  }
  p = realloc(*buf, need);
  if (!p) {
    return RIP_ENOMEM;
  }
  *buf = p;
  *cap = need;
  return RIP_OK;
}

static int compress(struct rvz *r, const uint8_t *src, size_t len, size_t *out_len) {
  size_t n;

  if (grow(&r->comp, &r->comp_cap, ZSTD_compressBound(len)) != RIP_OK) {
    return RIP_ENOMEM;
  }
  n = ZSTD_compress2(r->zstd, r->comp, r->comp_cap, src, len);
  if (ZSTD_isError(n)) {
    RIP_ERROR("RVZ: zstd failed: %s", ZSTD_getErrorName(n));
    return RIP_ERR;
  }
  *out_len = n;
  return RIP_OK;
}

static int all_same(const uint8_t *p, size_t n) {
  size_t i;

  for (i = 1; i < n; i++) {
    if (p[i] != p[0]) {
      return 0;
    }
  }
  return 1;
}

// One chunk, already read into r->in, becomes group g.
static int write_group(struct rvz *r, uint32_t g, size_t len, uint64_t base) {
  uint8_t *entry = r->groups + (size_t)g * GROUP_ENTRY_BYTES;
  const uint8_t *data = r->in;
  size_t data_len = len;
  size_t packed_len;
  size_t comp_len;
  uint32_t size_field;
  struct reuse *same = NULL;
  int rc;

  // A chunk of one repeated byte is stored once and shared, all zeros as nothing.
  if (all_same(r->in, len)) {
    size_t i;
    for (i = 0; i < r->reuse_count; i++) {
      if (r->reuse[i].size == len && r->reuse[i].value == r->in[0]) {
        memcpy(entry, r->reuse[i].entry, GROUP_ENTRY_BYTES);
        return RIP_OK;
      }
    }
    if (r->reuse_count < REUSE_MAX) {
      same = &r->reuse[r->reuse_count++];
      same->size = (uint32_t)len;
      same->value = r->in[0];
    }
    if (r->in[0] == 0) {
      rip_put_be32(entry, (uint32_t)(r->written >> 2));
      rip_put_be32(entry + 4, 0);
      rip_put_be32(entry + 8, 0);
      if (same) {
        memcpy(same->entry, entry, GROUP_ENTRY_BYTES);
      }
      return RIP_OK;
    }
  }

  if ((rc = find_junk(r, len, base)) != RIP_OK) {
    return rc;
  }
  if (grow(&r->packed, &r->packed_cap, len + (r->junk_count + 1) * (8 + SEED_BYTES)) != RIP_OK) {
    return RIP_ENOMEM;
  }
  packed_len = pack(r, len);
  if (packed_len) {
    data = r->packed;
    data_len = packed_len;
  }
  if ((rc = compress(r, data, data_len, &comp_len)) != RIP_OK) {
    return rc;
  }
  // Stored as it is when zstd doesn't make it smaller, as Dolphin does.
  size_field = (uint32_t)data_len;
  if (comp_len < data_len) {
    data = r->comp;
    data_len = comp_len;
    size_field = (uint32_t)comp_len | GROUP_COMPRESSED;
  }
  if (r->written >> 2 > UINT32_MAX) {
    RIP_ERROR("RVZ: %s grew past 16 GiB", r->out_path);
    return RIP_ERR;
  }
  rip_put_be32(entry, (uint32_t)(r->written >> 2));
  rip_put_be32(entry + 4, size_field);
  rip_put_be32(entry + 8, (uint32_t)packed_len);
  if (same) {
    memcpy(same->entry, entry, GROUP_ENTRY_BYTES);
  }
  if ((rc = write_out(r, data, data_len)) != RIP_OK) {
    return rc;
  }
  return pad4(r);
}

// Writes a table at *pos, inside the space kept at the start, or at the end
// of the file when it doesn't fit there.
static int write_table(struct rvz *r, uint64_t *pos, const uint8_t *p, size_t n, uint64_t *where) {
  static const uint8_t zero[4];
  size_t pad = (size_t)(-(uint64_t)n & 3);

  if (*pos <= r->reserved && *pos + n > r->reserved) {
    *pos = r->written;
  }
  *where = *pos;
  if (rip_seek_write(r->out_fd, *pos, p, n) != 0 || rip_write_all(r->out_fd, zero, pad) != 0) {
    RIP_ERROR("RVZ: writing %s failed: %s", r->out_path, strerror(errno));
    return RIP_ERR;
  }
  *pos += n + pad;
  if (*pos > r->written) {
    r->written = *pos;
  }
  return RIP_OK;
}

static int finish(struct rvz *r) {
  uint8_t raw[RAW_ENTRY_BYTES];
  uint8_t h1[HEADER1_BYTES];
  uint8_t h2[HEADER2_BYTES];
  struct rip_sha1 sha;
  uint8_t *raw_comp;
  size_t raw_len;
  size_t group_len;
  uint64_t pos = HEADER1_BYTES + HEADER2_BYTES;
  uint64_t part_off, raw_off, group_off;
  int rc;

  // One raw data entry for the whole image past the disc header.
  put_be64(raw, DISC_HEAD_BYTES);
  put_be64(raw + 8, r->iso_size - DISC_HEAD_BYTES);
  rip_put_be32(raw + 16, 0);
  rip_put_be32(raw + 20, r->group_count);

  if ((rc = write_table(r, &pos, NULL, 0, &part_off)) != RIP_OK) {
    return rc;
  }
  if ((rc = compress(r, raw, sizeof(raw), &raw_len)) != RIP_OK) {
    return rc;
  }
  raw_comp = malloc(raw_len);
  if (!raw_comp) {
    return RIP_ENOMEM;
  }
  memcpy(raw_comp, r->comp, raw_len);
  rc = write_table(r, &pos, raw_comp, raw_len, &raw_off);
  free(raw_comp);
  if (rc != RIP_OK) {
    return rc;
  }
  if ((rc = compress(r, r->groups, (size_t)r->group_count * GROUP_ENTRY_BYTES, &group_len)) != RIP_OK) {
    return rc;
  }
  if ((rc = write_table(r, &pos, r->comp, group_len, &group_off)) != RIP_OK) {
    return rc;
  }

  memset(h2, 0, sizeof(h2));
  rip_put_be32(h2, r->disc_type);
  rip_put_be32(h2 + 0x04, COMPRESSION_ZSTD);
  rip_put_be32(h2 + 0x08, (uint32_t)r->level);
  rip_put_be32(h2 + 0x0C, CHUNK);
  memcpy(h2 + 0x10, r->disc_head, DISC_HEAD_BYTES);
  rip_put_be32(h2 + 0x90, 0);  // no partition entries
  rip_put_be32(h2 + 0x94, PART_ENTRY_BYTES);
  put_be64(h2 + 0x98, part_off);
  rip_sha1_init(&sha);
  rip_sha1_final(&sha, h2 + 0xA0);  // of the (empty) partition entries
  rip_put_be32(h2 + 0xB4, 1);
  put_be64(h2 + 0xB8, raw_off);
  rip_put_be32(h2 + 0xC0, (uint32_t)raw_len);
  rip_put_be32(h2 + 0xC4, r->group_count);
  put_be64(h2 + 0xC8, group_off);
  rip_put_be32(h2 + 0xD0, (uint32_t)group_len);

  memset(h1, 0, sizeof(h1));
  memcpy(h1, "RVZ\x01", 4);
  rip_put_be32(h1 + 0x04, RVZ_VERSION);
  rip_put_be32(h1 + 0x08, RVZ_VERSION_COMPATIBLE);
  rip_put_be32(h1 + 0x0C, HEADER2_BYTES);
  rip_sha1_init(&sha);
  rip_sha1_update(&sha, h2, sizeof(h2));
  rip_sha1_final(&sha, h1 + 0x10);
  put_be64(h1 + 0x24, r->iso_size);
  put_be64(h1 + 0x2C, r->written);
  rip_sha1_init(&sha);
  rip_sha1_update(&sha, h1, 0x34);
  rip_sha1_final(&sha, h1 + 0x34);

  if (rip_seek_write(r->out_fd, 0, h1, sizeof(h1)) != 0 || rip_write_all(r->out_fd, h2, sizeof(h2)) != 0 ||
      fsync(r->out_fd) != 0) {
    RIP_ERROR("RVZ: writing %s failed: %s", r->out_path, strerror(errno));
    return RIP_ERR;
  }
  return RIP_OK;
}

static int convert(struct rvz *r, iso_progress_fn progress, void *ud) {
  uint8_t head[0x440];
  uint64_t off;
  uint32_t g;
  int rc;

  if (r->iso_size < sizeof(head)) {
    RIP_ERROR("RVZ: %s is too small for a disc image", r->iso_path);
    return RIP_ENOTSUP;
  }
  if (rip_seek_read(r->in_fd, 0, head, sizeof(head)) != 0) {
    RIP_ERROR("RVZ: reading %s failed: %s", r->iso_path, strerror(errno));
    return RIP_ERR;
  }
  if (rip_be32(head + 0x1C) == GC_MAGIC) {
    r->disc_type = DISC_GC;
    if ((rc = load_files(r, head)) != RIP_OK) {
      return rc;
    }
  } else if (rip_be32(head + 0x18) == WII_MAGIC) {
    r->disc_type = DISC_WII;
  } else {
    RIP_ERROR("RVZ: %s is not a GameCube or Wii disc", r->iso_path);
    return RIP_ENOTSUP;
  }
  memcpy(r->disc_head, head, DISC_HEAD_BYTES);

  r->group_count = (uint32_t)((r->iso_size + CHUNK - 1) / CHUNK);
  r->groups = calloc(r->group_count, GROUP_ENTRY_BYTES);
  r->in = malloc(CHUNK);
  r->zstd = ZSTD_createCCtx();
  if (!r->groups || !r->in || !r->zstd) {
    return RIP_ENOMEM;
  }
  ZSTD_CCtx_setParameter(r->zstd, ZSTD_c_compressionLevel, r->level);
  ZSTD_CCtx_setParameter(r->zstd, ZSTD_c_contentSizeFlag, 0);

  r->out_fd = open(r->out_path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
  if (r->out_fd < 0) {
    RIP_ERROR("RVZ: cannot create %s: %s", r->out_path, strerror(errno));
    return RIP_ERR;
  }
  // Room for the headers and tables up front, as Dolphin sizes it: the group
  // table is guessed to compress to 9/16 or better.
  r->reserved =
      HEADER1_BYTES + HEADER2_BYTES + RAW_ENTRY_BYTES + 0x100 + (uint64_t)r->group_count * GROUP_ENTRY_BYTES * 9 / 16;
  r->reserved = (r->reserved + BLOCK - 1) / BLOCK * BLOCK;
  memset(r->in, 0, CHUNK);
  for (off = 0; off < r->reserved; off += CHUNK) {
    size_t n = r->reserved - off < CHUNK ? (size_t)(r->reserved - off) : CHUNK;
    if ((rc = write_out(r, r->in, n)) != RIP_OK) {
      return rc;
    }
  }

  // The raw data starts at 0x80 but its chunks are counted from 0, so chunk
  // g is image bytes g * CHUNK onwards, the disc header included.
  for (g = 0; g < r->group_count; g++) {
    uint64_t base = (uint64_t)g * CHUNK;
    size_t len = r->iso_size - base < CHUNK ? (size_t)(r->iso_size - base) : CHUNK;
    errno = 0;
    if (rip_seek_read(r->in_fd, base, r->in, len) != 0) {
      RIP_ERROR("RVZ: reading %s at %llu failed: %s", r->iso_path, (unsigned long long)base,
                errno ? strerror(errno) : "short read");
      return RIP_ERR;
    }
    if ((rc = write_group(r, g, len, base)) != RIP_OK) {
      return rc;
    }
    if (progress && progress(ud, base + len, r->iso_size)) {
      return RIP_ECANCEL;
    }
  }
  return finish(r);
}

int rvz_from_iso(const char *iso_path, const char *out_path, int zstd_level, iso_progress_fn progress, void *ud) {
  struct rvz *r;
  struct stat st;
  int rc;

  if (zstd_level < ZSTD_minCLevel() || zstd_level > ZSTD_maxCLevel()) {
    RIP_ERROR("RVZ: zstd level %d is out of range", zstd_level);
    return RIP_ERR;
  }
  r = calloc(1, sizeof(*r));
  if (!r) {
    return RIP_ENOMEM;
  }
  r->iso_path = iso_path;
  r->out_path = out_path;
  r->level = zstd_level;
  r->out_fd = -1;
  r->in_fd = open(iso_path, O_RDONLY);
  if (r->in_fd < 0 || fstat(r->in_fd, &st) != 0) {
    RIP_ERROR("RVZ: cannot open %s: %s", iso_path, strerror(errno));
    rc = RIP_ERR;
  } else {
    r->iso_size = (uint64_t)st.st_size;
    rc = convert(r, progress, ud);
  }
  if (rc == RIP_ENOMEM) {
    RIP_ERROR("RVZ: out of memory");
  }
  if (r->out_fd >= 0 && close(r->out_fd) != 0 && rc == RIP_OK) {
    RIP_ERROR("RVZ: closing %s failed: %s", out_path, strerror(errno));
    rc = RIP_ERR;
  }
  if (r->out_fd >= 0 && rc != RIP_OK) {
    unlink(out_path);
  }
  if (rc == RIP_OK) {
    RIP_INFO("RVZ: %s, %llu bytes from %llu", out_path, (unsigned long long)r->written,
             (unsigned long long)r->iso_size);
  }
  if (r->in_fd >= 0) {
    close(r->in_fd);
  }
  ZSTD_freeCCtx(r->zstd);
  free(r->in);
  free(r->packed);
  free(r->comp);
  free(r->junk);
  free(r->files);
  free(r->groups);
  free(r);
  return rc;
}
