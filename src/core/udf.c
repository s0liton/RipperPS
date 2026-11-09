#include "ripper/udf.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ripper/common.h"
#include "ripper/log.h"

#define SECTOR 2048
#define AVDP_SECTOR 256
#define MAX_DIR_BYTES (4u << 20)
#define MAX_AED_CHAIN 64

enum {
  TAG_PVD = 1,
  TAG_AVDP = 2,
  TAG_PD = 5,
  TAG_LVD = 6,
  TAG_TERM = 8,
  TAG_FSD = 256,
  TAG_FID = 257,
  TAG_AED = 258,
  TAG_FE = 261,
  TAG_EFE = 266
};

static uint64_t le64(const uint8_t *p) {
  return (uint64_t)rip_le32(p) | ((uint64_t)rip_le32(p + 4) << 32);
}

static int read_sector(struct udf_fs *fs, uint32_t lba, uint8_t *buf) {
  return rip_reader_read(fs->r, lba, 1, buf);
}

// The tag is right for what the caller expects: id, a valid checksum, and
// written for the place it was read from.
static int tag_ok(const uint8_t *d, uint16_t id) {
  uint8_t sum = 0;
  int i;

  if (rip_le16(d) != id) {
    return 0;
  }
  for (i = 0; i < 16; i++) {
    if (i != 4) {
      sum = (uint8_t)(sum + d[i]);
    }
  }
  return sum == d[4];
}

// A dstring or file identifier to UTF-8: compression 8 is Latin-1, 16 is UCS-2.
static void decode_name(const uint8_t *p, size_t len, char *out, size_t n) {
  size_t o = 0;
  size_t i;

  if (n == 0) {
    return;
  }
  out[0] = '\0';
  if (len < 1) {
    return;
  }
  if (p[0] == 8) {
    for (i = 1; i < len && o + 3 < n; i++) {
      uint8_t c = p[i];
      if (c < 0x80) {
        out[o++] = (char)c;
      } else {
        out[o++] = (char)(0xC0 | (c >> 6));
        out[o++] = (char)(0x80 | (c & 0x3F));
      }
    }
  } else if (p[0] == 16) {
    for (i = 1; i + 1 < len && o + 4 < n; i += 2) {
      unsigned c = ((unsigned)p[i] << 8) | p[i + 1];
      if (c < 0x80) {
        out[o++] = (char)c;
      } else if (c < 0x800) {
        out[o++] = (char)(0xC0 | (c >> 6));
        out[o++] = (char)(0x80 | (c & 0x3F));
      } else {
        out[o++] = (char)(0xE0 | (c >> 12));
        out[o++] = (char)(0x80 | ((c >> 6) & 0x3F));
        out[o++] = (char)(0x80 | (c & 0x3F));
      }
    }
  }
  out[o] = '\0';
}

// A dstring field: the used length is in its last byte.
static void decode_dstring(const uint8_t *field, size_t size, char *out, size_t n) {
  size_t used = field[size - 1];

  if (used > size - 1) {
    used = size - 1;
  }
  decode_name(field, used, out, n);
  while (strlen(out) && out[strlen(out) - 1] == ' ') {
    out[strlen(out) - 1] = '\0';
  }
}

/* Addresses */

static int part_index(const struct udf_fs *fs, uint16_t number) {
  unsigned i;

  for (i = 0; i < fs->part_count; i++) {
    if (fs->parts[i].number == number) {
      return (int)i;
    }
  }
  return -1;
}

// A logical block of a partition map to an absolute sector.
static int translate(const struct udf_fs *fs, uint16_t ref, uint32_t block, uint32_t *lba) {
  if (ref >= fs->map_count) {
    return RIP_ERR;
  }
  if (!fs->maps[ref].metadata) {
    *lba = fs->parts[fs->maps[ref].part].start + block;
    return RIP_OK;
  }
  {
    uint64_t want = (uint64_t)block * SECTOR;
    unsigned i;
    for (i = 0; i < fs->metadata_file.extent_count; i++) {
      const struct udf_extent *e = &fs->metadata_file.extents[i];
      if (want < e->length) {
        *lba = e->lba + (uint32_t)(want / SECTOR);
        return RIP_OK;
      }
      want -= e->length;
    }
  }
  return RIP_ERR;
}

/* File entries */

struct ad_ctx {
  struct udf_fs *fs;
  struct udf_entry *e;
  uint16_t ref;   // partition map of the entry, for short_ads
  int phys_part;  // >= 0: the metadata file, whose extents are blocks of this partition
  int depth;
};

static int add_extent(struct ad_ctx *c, uint32_t len_field, uint16_t ref, uint32_t block);

// Allocation descriptors of one kind in a buffer. Extent type 3 continues in
// an Allocation Extent Descriptor elsewhere.
static int parse_ads(struct ad_ctx *c, const uint8_t *p, size_t len, int type) {
  size_t step = type == 0 ? 8 : 16;
  size_t off;

  for (off = 0; off + step <= len; off += step) {
    uint32_t len_field = rip_le32(p + off);
    uint32_t block = rip_le32(p + off + 4);
    uint16_t ref = type == 0 ? c->ref : rip_le16(p + off + 8);

    if ((len_field & 0x3FFFFFFF) == 0) {
      break;
    }
    if ((len_field >> 30) == 3) {
      uint8_t aed[SECTOR];
      uint32_t lba;
      if (++c->depth > MAX_AED_CHAIN || translate(c->fs, ref, block, &lba) != RIP_OK ||
          read_sector(c->fs, lba, aed) != RIP_OK || !tag_ok(aed, TAG_AED)) {
        return RIP_ERR;
      }
      {
        uint32_t l_ad = rip_le32(aed + 20);
        if (l_ad > SECTOR - 24) {
          return RIP_ERR;
        }
        return parse_ads(c, aed + 24, l_ad, type);
      }
    }
    if (add_extent(c, len_field, ref, block) != RIP_OK) {
      return RIP_ERR;
    }
  }
  return RIP_OK;
}

static int add_extent(struct ad_ctx *c, uint32_t len_field, uint16_t ref, uint32_t block) {
  struct udf_entry *e = c->e;
  uint32_t lba;

  if (e->extent_count >= UDF_MAX_EXTENTS) {
    RIP_WARN("UDF: file has more than %d extents", UDF_MAX_EXTENTS);
    return RIP_ERR;
  }
  if (c->phys_part >= 0) {
    lba = c->fs->parts[c->phys_part].start + block;
  } else if (translate(c->fs, ref, block, &lba) != RIP_OK) {
    return RIP_ERR;
  }
  e->extents[e->extent_count].lba = lba;
  e->extents[e->extent_count].length = len_field & 0x3FFFFFFF;
  e->extent_count++;
  return RIP_OK;
}

// Reads the (Extended) File Entry at lba. ref is the partition map its short
// allocation descriptors are relative to; phys_part >= 0 reads the metadata
// file, whose extents are blocks of that physical partition.
static int read_entry(struct udf_fs *fs, uint32_t lba, uint16_t ref, int phys_part, struct udf_entry *e) {
  uint8_t d[SECTOR];
  uint32_t l_ea;
  uint32_t l_ad;
  size_t ad_off;
  int ad_type;
  int extended;
  struct ad_ctx c;

  memset(e, 0, sizeof(*e));
  if (read_sector(fs, lba, d) != RIP_OK) {
    return RIP_ERR;
  }
  extended = tag_ok(d, TAG_EFE);
  if (!extended && !tag_ok(d, TAG_FE)) {
    return RIP_ERR;
  }
  e->is_dir = d[16 + 11] == 4;
  e->size = le64(d + 56);
  ad_type = rip_le16(d + 16 + 18) & 7;
  l_ea = rip_le32(d + (extended ? 208 : 168));
  l_ad = rip_le32(d + (extended ? 212 : 172));
  ad_off = (size_t)(extended ? 216 : 176) + l_ea;
  if (ad_off > SECTOR || l_ad > SECTOR - ad_off) {
    return RIP_ERR;
  }
  if (ad_type == 3) {
    e->embedded_len = l_ad < sizeof(e->embedded) ? l_ad : (uint32_t)sizeof(e->embedded);
    memcpy(e->embedded, d + ad_off, e->embedded_len);
    return RIP_OK;
  }
  if (ad_type > 1) {
    return RIP_ENOTSUP;
  }
  c.fs = fs;
  c.e = e;
  c.ref = ref;
  c.phys_part = phys_part;
  c.depth = 0;
  return parse_ads(&c, d + ad_off, l_ad, ad_type);
}

static int read_icb(struct udf_fs *fs, const uint8_t long_ad[16], struct udf_entry *e) {
  uint32_t block = rip_le32(long_ad + 4);
  uint16_t ref = rip_le16(long_ad + 8);
  uint32_t lba;

  if (translate(fs, ref, block, &lba) != RIP_OK) {
    return RIP_ERR;
  }
  return read_entry(fs, lba, ref, -1, e);
}

/* Mounting */

static int parse_lvd(struct udf_fs *fs, const uint8_t *d, uint8_t fsd_ad[16]) {
  uint32_t n_maps = rip_le32(d + 268);
  uint32_t table_len = rip_le32(d + 264);
  size_t off = 440;
  uint32_t i;

  if (rip_le32(d + 212) != SECTOR) {
    return RIP_ENOTSUP;
  }
  decode_dstring(d + 84, 128, fs->logical_id, sizeof(fs->logical_id));
  fs->udf_revision = rip_le16(d + 216 + 24);
  memcpy(fsd_ad, d + 248, 16);
  for (i = 0; i < n_maps && i < 4 && off + 2 <= 440 + table_len && off + 2 <= SECTOR; i++) {
    uint8_t type = d[off];
    uint8_t len = d[off + 1];
    if (len == 0 || off + len > SECTOR) {
      break;
    }
    fs->maps[i].metadata = 0;
    if (type == 1) {
      fs->maps[i].part = rip_le16(d + off + 4);  // a partition number for now
    } else if (type == 2 && len >= 64) {
      char ident[24];
      memcpy(ident, d + off + 5, 23);
      ident[23] = '\0';
      fs->maps[i].part = rip_le16(d + off + 38);
      if (strncmp(ident, "*UDF Metadata Partition", 23) == 0) {
        fs->maps[i].metadata = 1;
        // The metadata file's location rides in the map; keep it in the extent slot until resolved.
        fs->metadata_file.extents[0].lba = rip_le32(d + off + 40);
      } else if (strncmp(ident, "*UDF Virtual Partition", 22) == 0) {
        return RIP_ENOTSUP;  // VAT: written discs, not pressed ones
      }
      // Sparable partitions read like plain ones on a disc without defects.
    }
    fs->map_count = i + 1;
    off += len;
  }
  return fs->map_count ? RIP_OK : RIP_ERR;
}

int udf_mount(struct udf_fs *fs, struct rip_reader *r) {
  uint8_t d[SECTOR];
  uint8_t fsd_ad[16];
  uint32_t vds_lba;
  uint32_t vds_len;
  uint32_t meta_loc = 0;
  uint32_t s;
  int have_lvd = 0;
  unsigned i;

  memset(fs, 0, sizeof(*fs));
  fs->r = r;
  if (r->sector_size != SECTOR || r->sector_count <= AVDP_SECTOR) {
    return RIP_ENOENT;
  }
  if (read_sector(fs, AVDP_SECTOR, d) != RIP_OK || !tag_ok(d, TAG_AVDP)) {
    return RIP_ENOENT;
  }
  vds_len = rip_le32(d + 16);
  vds_lba = rip_le32(d + 20);
  for (s = 0; s < vds_len / SECTOR && s < 64; s++) {
    if (read_sector(fs, vds_lba + s, d) != RIP_OK) {
      return RIP_ERR;
    }
    if (tag_ok(d, TAG_TERM) || rip_le16(d) == 0) {
      break;
    }
    if (tag_ok(d, TAG_PVD)) {
      decode_dstring(d + 24, 32, fs->volume_id, sizeof(fs->volume_id));
    } else if (tag_ok(d, TAG_PD) && fs->part_count < 4) {
      struct udf_partition *p = &fs->parts[fs->part_count++];
      p->number = rip_le16(d + 22);
      p->start = rip_le32(d + 188);
      p->length = rip_le32(d + 192);
    } else if (tag_ok(d, TAG_LVD) && !have_lvd) {
      int rc = parse_lvd(fs, d, fsd_ad);
      if (rc != RIP_OK) {
        return rc;
      }
      meta_loc = fs->metadata_file.extents[0].lba;
      fs->metadata_file.extents[0].lba = 0;
      have_lvd = 1;
    }
  }
  if (!have_lvd || fs->part_count == 0) {
    return RIP_ERR;
  }
  // Maps named partition numbers; make them indices.
  for (i = 0; i < fs->map_count; i++) {
    int idx = part_index(fs, (uint16_t)fs->maps[i].part);
    if (idx < 0) {
      return RIP_ERR;
    }
    fs->maps[i].part = (unsigned)idx;
  }
  for (i = 0; i < fs->map_count; i++) {
    if (fs->maps[i].metadata) {
      // The metadata file lives in the physical partition behind the map.
      struct udf_entry meta;
      uint32_t lba = fs->parts[fs->maps[i].part].start + meta_loc;
      if (read_entry(fs, lba, (uint16_t)i, (int)fs->maps[i].part, &meta) != RIP_OK || meta.extent_count == 0) {
        RIP_WARN("UDF: unreadable metadata file");
        return RIP_ERR;
      }
      fs->metadata_file = meta;
      break;
    }
  }
  {
    uint8_t fsd[SECTOR];
    uint32_t lba;
    if (translate(fs, rip_le16(fsd_ad + 8), rip_le32(fsd_ad + 4), &lba) != RIP_OK ||
        read_sector(fs, lba, fsd) != RIP_OK || !tag_ok(fsd, TAG_FSD)) {
      RIP_WARN("UDF: no file set descriptor");
      return RIP_ERR;
    }
    if (read_icb(fs, fsd + 400, &fs->root) != RIP_OK || !fs->root.is_dir) {
      RIP_WARN("UDF: unreadable root directory");
      return RIP_ERR;
    }
  }
  return RIP_OK;
}

/* Files and directories */

int udf_read_file(struct udf_fs *fs, const struct udf_entry *e, size_t max, uint8_t **out, size_t *out_len) {
  uint8_t *buf;
  uint64_t done = 0;
  unsigned i;

  *out = NULL;
  *out_len = 0;
  if (e->size > max) {
    return RIP_ERR;
  }
  buf = malloc(e->size ? (size_t)e->size : 1);
  if (!buf) {
    return RIP_ENOMEM;
  }
  if (e->embedded_len || e->extent_count == 0) {
    size_t n = e->size < e->embedded_len ? (size_t)e->size : e->embedded_len;
    memcpy(buf, e->embedded, n);
    done = n;
  }
  for (i = 0; i < e->extent_count && done < e->size; i++) {
    const struct udf_extent *x = &e->extents[i];
    uint32_t sectors = (x->length + SECTOR - 1) / SECTOR;
    uint32_t s;
    for (s = 0; s < sectors && done < e->size; s++) {
      uint8_t sec[SECTOR];
      size_t take = e->size - done < SECTOR ? (size_t)(e->size - done) : SECTOR;
      if (read_sector(fs, x->lba + s, sec) != RIP_OK) {
        free(buf);
        return RIP_ERR;
      }
      memcpy(buf + done, sec, take);
      done += take;
    }
  }
  *out = buf;
  *out_len = (size_t)done;
  return RIP_OK;
}

int udf_list(struct udf_fs *fs, const struct udf_entry *dir, udf_dir_fn fn, void *user) {
  uint8_t *data;
  size_t len;
  size_t off = 0;
  int rc;

  if (!dir->is_dir) {
    return RIP_ERR;
  }
  rc = udf_read_file(fs, dir, MAX_DIR_BYTES, &data, &len);
  if (rc != RIP_OK) {
    return rc;
  }
  while (off + 38 <= len) {
    const uint8_t *f = data + off;
    uint8_t chars = f[18];
    uint8_t l_fi = f[19];
    uint16_t l_iu = rip_le16(f + 36);
    size_t total = (38u + l_iu + l_fi + 3) & ~3u;

    if (!tag_ok(f, TAG_FID) || off + total > len) {
      break;
    }
    if (!(chars & 0x08) && !(chars & 0x04) && l_fi) {  // not the parent, not deleted
      char name[256];
      struct udf_entry e;
      decode_name(f + 38 + l_iu, l_fi, name, sizeof(name));
      if (read_icb(fs, f + 20, &e) == RIP_OK && fn(name, &e, user)) {
        break;
      }
    }
    off += total;
  }
  free(data);
  return RIP_OK;
}

struct find_ctx {
  const char *want;
  size_t want_len;
  struct udf_entry *out;
  int found;
};

static int find_cb(const char *name, const struct udf_entry *e, void *user) {
  struct find_ctx *c = user;
  size_t i;

  if (strlen(name) != c->want_len) {
    return 0;
  }
  for (i = 0; i < c->want_len; i++) {
    if (tolower((unsigned char)name[i]) != tolower((unsigned char)c->want[i])) {
      return 0;
    }
  }
  *c->out = *e;
  c->found = 1;
  return 1;
}

int udf_lookup(struct udf_fs *fs, const char *path, struct udf_entry *out) {
  struct udf_entry cur = fs->root;
  const char *p = path;

  while (*p == '/') {
    p++;
  }
  while (*p) {
    const char *end = strchr(p, '/');
    size_t n = end ? (size_t)(end - p) : strlen(p);
    struct find_ctx c = {p, n, out, 0};

    if (udf_list(fs, &cur, find_cb, &c) != RIP_OK || !c.found) {
      return RIP_ENOENT;
    }
    cur = *out;
    p += n;
    while (*p == '/') {
      p++;
    }
  }
  *out = cur;
  return RIP_OK;
}
