#include "ripper/aacs.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "ripper/aes.h"
#include "ripper/common.h"
#include "ripper/hash.h"
#include "ripper/log.h"
#include "ripper/udf.h"
#include "ripper/util.h"

#define SECTOR 2048u
#define UNIT_SECTORS (AACS_UNIT / SECTOR)
#define TS_PACKET 192u
#define MAX_RANGES 1024
// Unit_Key_RO.inf files are a few KiB; this is far past any.
#define MAX_INF (1u << 20)

// The IV of every aligned unit's CBC (the AACS spec's).
static const uint8_t IV[16] = {0x0B, 0xA0, 0xF8, 0xDD, 0xFE, 0xA6, 0x1F, 0xB3,
                               0xD8, 0xDF, 0x9F, 0x56, 0x6A, 0x05, 0x0F, 0x78};

/* KEYDB.cfg */

static int hex_val(int c) {
  return c >= '0' && c <= '9'   ? c - '0'
         : c >= 'a' && c <= 'f' ? c - 'a' + 10
         : c >= 'A' && c <= 'F' ? c - 'A' + 10
                                : -1;
}

// "0x" and 2n hex digits at p into out. The end of them, or NULL.
static const char *hex_bytes(const char *p, uint8_t *out, size_t n) {
  size_t i;

  while (*p == ' ' || *p == '\t') {
    p++;
  }
  if (p[0] != '0' || (p[1] != 'x' && p[1] != 'X')) {
    return NULL;
  }
  p += 2;
  for (i = 0; i < n; i++) {
    int hi = hex_val((unsigned char)p[0]);
    int lo = hi >= 0 ? hex_val((unsigned char)p[1]) : -1;
    if (lo < 0) {
      return NULL;
    }
    out[i] = (uint8_t)(hi << 4 | lo);
    p += 2;
  }
  return hex_val((unsigned char)*p) >= 0 ? NULL : p;
}

static char *trim(char *p) {
  size_t n;

  while (*p == ' ' || *p == '\t') {
    p++;
  }
  n = strlen(p);
  while (n && (p[n - 1] == ' ' || p[n - 1] == '\t' || p[n - 1] == '\r' || p[n - 1] == '\n')) {
    p[--n] = '\0';
  }
  return p;
}

// One entry: "0x<disc ID> = <title> | D | <date> | V | 0x<VUK> | U | 1-0x<key> ; 2-0x<key>",
// a tag then its value, in any order. line is cut up.
static void parse_entry(char *line, struct aacs_keys *k) {
  char *eq = strchr(line, '=');
  char *field[64];
  unsigned n = 0;
  unsigned i;
  char *p;

  memset(k, 0, sizeof(*k));
  if (!eq) {
    return;
  }
  for (p = eq + 1; p && n < RIP_ARRAY_LEN(field);) {
    char *bar = strchr(p, '|');
    if (bar) {
      *bar = '\0';
    }
    field[n++] = trim(p);
    p = bar ? bar + 1 : NULL;
  }
  snprintf(k->title, sizeof(k->title), "%s", n ? field[0] : "");
  for (i = 1; i + 1 < n; i += 2) {
    const char *tag = field[i];
    char *value = field[i + 1];
    if (!strcasecmp(tag, "V")) {
      k->have_vuk = hex_bytes(value, k->vuk, 16) != NULL;
    } else if (!strcasecmp(tag, "U")) {
      // "1-0x<key> ; 2-0x<key>"
      for (p = value; p && k->uk_count < AACS_MAX_UNIT_KEYS;) {
        char *semi = strchr(p, ';');
        char *dash;
        if (semi) {
          *semi = '\0';
        }
        dash = strchr(p, '-');
        if (dash && hex_bytes(trim(dash + 1), k->uk[k->uk_count], 16)) {
          k->uk_count++;
        }
        p = semi ? semi + 1 : NULL;
      }
    }
  }
}

int aacs_keydb_find(const char *path, const uint8_t disc_id[20], struct aacs_keys *out) {
  FILE *f = fopen(path, "r");
  char want[43];
  char *line;
  int rc = RIP_ENOENT;
  size_t i;

  memset(out, 0, sizeof(*out));
  if (!f) {
    return RIP_ERR;
  }
  want[0] = '0';
  want[1] = 'x';
  for (i = 0; i < 20; i++) {
    snprintf(want + 2 + i * 2, 3, "%02x", disc_id[i]);
  }
  line = malloc(65536);
  while (line && fgets(line, 65536, f)) {
    char *p = line;
    while (*p == ' ' || *p == '\t') {
      p++;
    }
    if (!strncasecmp(p, want, 42) && (p[42] == ' ' || p[42] == '=')) {
      parse_entry(p, out);
      rc = out->have_vuk || out->uk_count ? RIP_OK : RIP_ENOENT;
      break;
    }
  }
  free(line);
  fclose(f);
  return rc;
}

long aacs_keydb_count(const char *path) {
  FILE *f = fopen(path, "r");
  char *line;
  long n = 0;

  if (!f) {
    return -1;
  }
  line = malloc(65536);
  while (line && fgets(line, 65536, f)) {
    const char *p = line;
    uint8_t id[20];
    while (*p == ' ' || *p == '\t') {
      p++;
    }
    if (hex_bytes(p, id, 20)) {
      n++;
    }
  }
  free(line);
  fclose(f);
  return n;
}

/* Keys and units */

int aacs_unit_keys(const uint8_t *inf, size_t len, const uint8_t vuk[16], uint8_t uk[][16], unsigned max) {
  struct rip_aes128 aes;
  uint32_t at;
  unsigned n;
  unsigned i;

  if (len < 4) {
    return -1;
  }
  at = rip_be32(inf);
  if ((uint64_t)at + 2 > len) {
    return -1;
  }
  n = rip_be16(inf + at);
  if ((uint64_t)at + 48ull * n + 16 > len) {
    return -1;
  }
  rip_aes128_init_dec(&aes, vuk);
  for (i = 0; i < n && i < max; i++) {
    rip_aes128_decrypt_block(&aes, inf + at + 48 * (i + 1), uk[i]);
  }
  return (int)(n < max ? n : max);
}

// The unit's own key: its clear first 16 bytes, encrypted with the unit key
// and XORed with themselves.
static void unit_key(const uint8_t uk[16], const uint8_t head[16], uint8_t key[16]) {
  struct rip_aes128 aes;
  int i;

  rip_aes128_init_enc(&aes, uk);
  rip_aes128_encrypt_block(&aes, head, key);
  for (i = 0; i < 16; i++) {
    key[i] ^= head[i];
  }
}

static int looks_like_ts(const uint8_t *unit) {
  unsigned i;

  for (i = 0; i < AACS_UNIT / TS_PACKET; i++) {
    if (unit[i * TS_PACKET + 4] != 0x47) {
      return 0;
    }
  }
  return 1;
}

int aacs_decrypt_unit(const uint8_t uk[16], uint8_t unit[AACS_UNIT]) {
  struct rip_aes128 aes;
  uint8_t key[16];
  uint8_t iv[16];
  uint8_t *copy;

  // The copy permission bits of the first packet's header: clear, not encrypted.
  if (!(unit[0] & 0xC0)) {
    return 0;
  }
  copy = malloc(AACS_UNIT);
  if (!copy) {
    return -1;
  }
  memcpy(copy, unit, AACS_UNIT);
  unit_key(uk, copy, key);
  rip_aes128_init_dec(&aes, key);
  memcpy(iv, IV, sizeof(iv));
  rip_aes128_cbc_decrypt(&aes, iv, copy + 16, AACS_UNIT - 16);
  if (!looks_like_ts(copy)) {
    free(copy);
    return -1;
  }
  copy[0] &= 0x3F;  // decrypted now, as libaacs marks it
  memcpy(unit, copy, AACS_UNIT);
  free(copy);
  return 1;
}

void aacs_encrypt_unit(const uint8_t uk[16], uint8_t unit[AACS_UNIT]) {
  struct rip_aes128 aes;
  uint8_t key[16];
  uint8_t iv[16];

  unit[0] |= 0xC0;
  unit_key(uk, unit, key);
  rip_aes128_init_enc(&aes, key);
  memcpy(iv, IV, sizeof(iv));
  rip_aes128_cbc_encrypt(&aes, iv, unit + 16, AACS_UNIT - 16);
}

/* The reader */

// A run of a stream file's sectors, physically in a row: lba to end, the
// first being off bytes into its file.
struct range {
  uint32_t lba;
  uint32_t end;
  uint64_t off;
  unsigned file;
};

struct aacs_ctx {
  struct rip_reader *base;
  struct range *ranges;
  unsigned count;
  uint8_t uk[AACS_MAX_UNIT_KEYS][16];
  unsigned uk_count;
  unsigned *file_key;  // per file: the unit key that last worked
  unsigned files;
  uint8_t *buf;
  size_t buf_sectors;
  int warned;
};

static int cmp_range(const void *a, const void *b) {
  const struct range *x = a;
  const struct range *y = b;

  return x->lba < y->lba ? -1 : x->lba > y->lba;
}

struct collect {
  struct aacs_ctx *c;
  unsigned files_max;
};

static int collect_stream(const char *name, const struct udf_entry *e, void *user) {
  struct collect *col = user;
  struct aacs_ctx *c = col->c;
  size_t n = strlen(name);
  uint64_t off = 0;
  unsigned i;

  if (e->is_dir || n < 5 || strcasecmp(name + n - 5, ".m2ts") != 0 || c->files >= col->files_max) {
    return 0;
  }
  for (i = 0; i < e->extent_count && c->count < MAX_RANGES; i++) {
    uint32_t sectors = (e->extents[i].length + SECTOR - 1) / SECTOR;
    struct range *last = c->count ? &c->ranges[c->count - 1] : NULL;
    if (last && last->file == c->files && last->end == e->extents[i].lba &&
        last->off + (uint64_t)(last->end - last->lba) * SECTOR == off) {
      last->end += sectors;  // the next extent follows on the disc: one run
    } else {
      c->ranges[c->count].lba = e->extents[i].lba;
      c->ranges[c->count].end = e->extents[i].lba + sectors;
      c->ranges[c->count].off = off;
      c->ranges[c->count].file = c->files;
      c->count++;
    }
    off += e->extents[i].length;
  }
  c->files++;
  return 0;
}

static const struct range *range_at(const struct aacs_ctx *c, uint64_t lba) {
  unsigned lo = 0;
  unsigned hi = c->count;

  while (lo < hi) {
    unsigned mid = (lo + hi) / 2;
    if (lba < c->ranges[mid].lba) {
      hi = mid;
    } else if (lba >= c->ranges[mid].end) {
      lo = mid + 1;
    } else {
      return &c->ranges[mid];
    }
  }
  return NULL;
}

// Where the next range starts after lba, or UINT64_MAX.
static uint64_t next_range(const struct aacs_ctx *c, uint64_t lba) {
  unsigned i;

  for (i = 0; i < c->count; i++) {
    if (c->ranges[i].lba > lba) {
      return c->ranges[i].lba;
    }
  }
  return UINT64_MAX;
}

static int decrypt(struct aacs_ctx *c, unsigned file, uint8_t *unit) {
  unsigned k = c->file_key[file];
  unsigned tried;
  int rc = aacs_decrypt_unit(c->uk[k], unit);

  // Discs with several CPS units: the key that works for this file.
  for (tried = 1; rc < 0 && tried < c->uk_count; tried++) {
    k = (k + 1) % c->uk_count;
    rc = aacs_decrypt_unit(c->uk[k], unit);
    if (rc > 0) {
      c->file_key[file] = k;
    }
  }
  if (rc < 0 && !c->warned) {
    RIP_ERROR("AACS: a unit didn't decrypt with any of the disc's %u unit key%s (bus encryption, or the wrong key)",
              c->uk_count, c->uk_count == 1 ? "" : "s");
    c->warned = 1;
  }
  return rc < 0 ? RIP_ERR : RIP_OK;
}

// Sectors [lba, lba+count), all in range r: read from the start of the
// first unit to the end of the last, the units decrypted, the part asked
// for copied out. A unit cut by the end of a run (the file goes on elsewhere
// on the disc) is read as it is: none was seen like that.
static int read_stream(struct aacs_ctx *c, const struct range *r, uint64_t lba, uint32_t count, uint8_t *out) {
  uint64_t off = r->off + (lba - r->lba) * SECTOR;
  uint64_t lead = (off % AACS_UNIT) / SECTOR;  // sectors of the first unit before lba
  uint64_t from = lba - lead;
  uint64_t to = lba + count;
  uint64_t tail = ((off + (uint64_t)count * SECTOR) % AACS_UNIT);
  uint64_t u;

  if (tail) {
    to += (AACS_UNIT - tail) / SECTOR;
  }
  if (from < r->lba) {
    from = r->lba;
  }
  if (to > r->end) {
    to = r->end;
  }
  if (to - from > c->buf_sectors) {
    return RIP_ERR;
  }
  if (rip_reader_read(c->base, from, (uint32_t)(to - from), c->buf) != RIP_OK) {
    return RIP_ERR;
  }
  for (u = 0; (u + 1) * UNIT_SECTORS <= to - from; u += 1) {
    uint8_t *unit = c->buf + u * AACS_UNIT;
    uint64_t at = r->off + (from - r->lba) * SECTOR + u * AACS_UNIT;
    if (at % AACS_UNIT) {
      break;  // the run started mid-unit: left as read
    }
    if (decrypt(c, r->file, unit) != RIP_OK) {
      return RIP_ERR;
    }
  }
  memcpy(out, c->buf + (lba - from) * SECTOR, (size_t)count * SECTOR);
  return RIP_OK;
}

static int aacs_read(void *ctx, uint64_t lba, uint32_t count, void *buf) {
  struct aacs_ctx *c = ctx;
  uint8_t *out = buf;

  while (count) {
    const struct range *r = range_at(c, lba);
    uint32_t run = count;
    int rc;
    if (r) {
      if (r->end - lba < run) {
        run = (uint32_t)(r->end - lba);
      }
      if (run > c->buf_sectors - 2 * UNIT_SECTORS) {
        run = (uint32_t)(c->buf_sectors - 2 * UNIT_SECTORS);
      }
      rc = read_stream(c, r, lba, run, out);
    } else {
      uint64_t next = next_range(c, lba);
      if (next - lba < run) {
        run = (uint32_t)(next - lba);
      }
      rc = rip_reader_read(c->base, lba, run, out);
    }
    if (rc != RIP_OK) {
      return rc;
    }
    lba += run;
    out += (size_t)run * SECTOR;
    count -= run;
  }
  return RIP_OK;
}

static void aacs_close(void *ctx) {
  struct aacs_ctx *c = ctx;

  free(c->ranges);
  free(c->file_key);
  free(c->buf);
  free(c);
}

static const struct rip_reader_ops aacs_ops = {aacs_read, aacs_close};

int rip_reader_open_aacs(struct rip_reader *out, struct rip_reader *base, const char *keydb_path, char *why,
                         size_t wn) {
  struct udf_fs *fs = malloc(sizeof(*fs));
  struct udf_entry *e = malloc(sizeof(*e));
  struct aacs_ctx *c = calloc(1, sizeof(*c));
  struct aacs_keys keys;
  struct collect col;
  uint8_t *inf = NULL;
  size_t inf_len = 0;
  uint8_t id[20];
  char hex[41];
  int rc = RIP_ERR;
  size_t i;

  memset(out, 0, sizeof(*out));
  if (why && wn) {
    why[0] = '\0';
  }
  if (!fs || !e || !c) {
    rc = RIP_ENOMEM;
    goto fail;
  }
  if (udf_mount(fs, base) != RIP_OK || udf_lookup(fs, "AACS/Unit_Key_RO.inf", e) != RIP_OK ||
      udf_read_file(fs, e, MAX_INF, &inf, &inf_len) != RIP_OK) {
    rc = RIP_ENOTSUP;
    goto fail;
  }
  {
    struct rip_sha1 h;
    rip_sha1_init(&h);
    rip_sha1_update(&h, inf, inf_len);
    rip_sha1_final(&h, id);
  }
  rip_hex(id, 20, hex);
  rc = keydb_path && keydb_path[0] ? aacs_keydb_find(keydb_path, id, &keys) : RIP_ERR;
  if (rc != RIP_OK) {
    if (why) {
      snprintf(why, wn,
               rc == RIP_ENOENT ? "the KEYDB.cfg has no key for this disc (disc ID 0x%s)"
                                : "there's no KEYDB.cfg (disc ID 0x%s)",
               hex);
    }
    RIP_WARN("AACS: no key for disc ID 0x%s in %s", hex, keydb_path ? keydb_path : "(none)");
    rc = RIP_ENOENT;
    goto fail;
  }
  if (keys.uk_count) {
    memcpy(c->uk, keys.uk, sizeof(keys.uk));
    c->uk_count = keys.uk_count;
  } else {
    int n = aacs_unit_keys(inf, inf_len, keys.vuk, c->uk, AACS_MAX_UNIT_KEYS);
    if (n <= 0) {
      if (why) {
        snprintf(why, wn, "the disc's unit key file can't be read (disc ID 0x%s)", hex);
      }
      rc = RIP_ERR;
      goto fail;
    }
    c->uk_count = (unsigned)n;
  }
  // Where the streams are: their sectors are the encrypted ones.
  c->ranges = calloc(MAX_RANGES, sizeof(*c->ranges));
  c->file_key = calloc(4096, sizeof(*c->file_key));
  c->buf_sectors = (base->max_sectors ? base->max_sectors : 64) + 4 * UNIT_SECTORS;
  c->buf = malloc(c->buf_sectors * SECTOR);
  if (!c->ranges || !c->file_key || !c->buf) {
    rc = RIP_ENOMEM;
    goto fail;
  }
  col.c = c;
  col.files_max = 4096;
  if (udf_lookup(fs, "BDMV/STREAM", e) == RIP_OK) {
    udf_list(fs, e, collect_stream, &col);
  }
  qsort(c->ranges, c->count, sizeof(*c->ranges), cmp_range);
  for (i = 1; i < c->count; i++) {
    if (c->ranges[i].lba < c->ranges[i - 1].end) {
      RIP_WARN("AACS: two streams share sectors at %u", c->ranges[i].lba);
    }
  }
  RIP_INFO("AACS: disc ID 0x%s (%s), %u unit key%s, %u stream file%s", hex, keys.title[0] ? keys.title : "no title",
           c->uk_count, c->uk_count == 1 ? "" : "s", c->files, c->files == 1 ? "" : "s");
  c->base = base;
  out->ops = &aacs_ops;
  out->ctx = c;
  out->sector_size = base->sector_size;
  out->sector_count = base->sector_count;
  out->max_sectors = base->max_sectors;
  free(inf);
  free(fs);
  free(e);
  return RIP_OK;

fail:
  free(inf);
  free(fs);
  free(e);
  if (c) {
    free(c->ranges);
    free(c->file_key);
    free(c->buf);
    free(c);
  }
  return rc;
}
