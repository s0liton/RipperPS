#include "udf_builder.h"

#include <stdlib.h>
#include <string.h>

#include "ripper/common.h"

#define SECTOR 2048
#define VDS 32
#define AVDP 256
#define PART_START 300
#define MAX_NODES 64

// Image layout: volume recognition at 16, the descriptor sequence at 32, the
// anchor at 256, the partition from 300. Inside the partition: for 2.50 the
// metadata file's entry at block 0 and its data (the metadata partition:
// FSD, then entries and directories) from block 1; for 1.02 the same things
// directly in the partition. File data follows.

struct node {
  char name[64];
  int parent;
  int is_dir;
  const uint8_t *data;
  size_t len;
  uint32_t fe_block;    // in the metadata partition (2.50) or the partition (1.02)
  uint32_t dir_block;   // directories: where their FIDs are
  uint32_t dir_len;
  uint32_t data_block;  // files: in the physical partition
};

struct build {
  uint8_t *img;
  size_t len;
  int rev250;
  struct node nodes[MAX_NODES];
  int count;
  uint32_t meta_blocks;  // size of the metadata partition, 2.50
};

static void put_le16(uint8_t *p, uint16_t v) {
  rip_put_le16(p, v);
}

static void put_le32(uint8_t *p, uint32_t v) {
  rip_put_le32(p, v);
}

static void put_le64(uint8_t *p, uint64_t v) {
  rip_put_le32(p, (uint32_t)v);
  rip_put_le32(p + 4, (uint32_t)(v >> 32));
}

static uint16_t crc_itu(const uint8_t *p, size_t n) {
  uint16_t crc = 0;
  int b;

  while (n--) {
    crc ^= (uint16_t)(*p++ << 8);
    for (b = 0; b < 8; b++) {
      crc = (crc & 0x8000) ? (uint16_t)((crc << 1) ^ 0x1021) : (uint16_t)(crc << 1);
    }
  }
  return crc;
}

// Fills a descriptor tag over body_len bytes after it.
static void tag(uint8_t *d, uint16_t id, uint32_t location, size_t body_len, int rev250) {
  uint8_t sum = 0;
  int i;

  put_le16(d, id);
  put_le16(d + 2, rev250 ? 3 : 2);
  put_le16(d + 6, 1);
  put_le16(d + 8, crc_itu(d + 16, body_len));
  put_le16(d + 10, (uint16_t)body_len);
  put_le32(d + 12, location);
  d[4] = 0;
  for (i = 0; i < 16; i++) {
    if (i != 4) {
      sum = (uint8_t)(sum + d[i]);
    }
  }
  d[4] = sum;
}

static void dstring(uint8_t *field, size_t size, const char *s) {
  size_t n = strlen(s);

  if (n > size - 2) {
    n = size - 2;
  }
  field[0] = 8;
  memcpy(field + 1, s, n);
  field[size - 1] = (uint8_t)(n + 1);
}

static void regid(uint8_t *field, const char *ident) {
  memcpy(field + 1, ident, strlen(ident));
}

static uint8_t *sec(struct build *b, uint32_t lba) {
  return b->img + (size_t)lba * SECTOR;
}

// Where a block of the metadata partition (2.50) or the partition (1.02) is.
static uint32_t meta_lba(const struct build *b, uint32_t block) {
  return b->rev250 ? PART_START + 1 + block : PART_START + block;
}

static int find_child(struct build *b, int parent, const char *name, size_t len) {
  int i;

  for (i = 0; i < b->count; i++) {
    if (b->nodes[i].parent == parent && strlen(b->nodes[i].name) == len && !strncmp(b->nodes[i].name, name, len)) {
      return i;
    }
  }
  return -1;
}

static int add_path(struct build *b, const struct udfb_file *f) {
  const char *p = f->path;
  int parent = 0;

  while (*p) {
    const char *slash = strchr(p, '/');
    size_t n = slash ? (size_t)(slash - p) : strlen(p);
    int idx = find_child(b, parent, p, n);
    if (idx < 0) {
      struct node *nd;
      if (b->count >= MAX_NODES) {
        return -1;
      }
      idx = b->count++;
      nd = &b->nodes[idx];
      memset(nd, 0, sizeof(*nd));
      memcpy(nd->name, p, n < sizeof(nd->name) - 1 ? n : sizeof(nd->name) - 1);
      nd->parent = parent;
      nd->is_dir = slash != NULL;
      if (!slash) {
        nd->data = f->data;
        nd->len = f->len;
      }
    }
    parent = idx;
    p += n;
    if (*p == '/') {
      p++;
    }
  }
  return 0;
}

static size_t fid_size(const char *name) {
  size_t l_fi = name ? strlen(name) + 1 : 0;
  return (38 + l_fi + 3) & ~(size_t)3;
}

// One FID at p for node target; the parent entry when name is NULL.
static size_t write_fid(struct build *b, uint8_t *p, uint32_t lba, const struct node *target, const char *name) {
  size_t l_fi = name ? strlen(name) + 1 : 0;
  size_t total = fid_size(name);

  memset(p, 0, total);
  put_le16(p + 16, 1);
  p[18] = (uint8_t)((target->is_dir ? 0x02 : 0) | (name ? 0 : 0x08));
  p[19] = (uint8_t)l_fi;
  put_le32(p + 20, SECTOR);
  put_le32(p + 24, target->fe_block);
  put_le16(p + 28, b->rev250 ? 1 : 0);  // entries live in the metadata partition (map 1)
  if (name) {
    p[38] = 8;
    memcpy(p + 39, name, l_fi - 1);
  }
  tag(p, 257, lba, total - 16, b->rev250);
  return total;
}

// A File Entry (1.02) or Extended File Entry (2.50) at block, with one extent.
static void write_entry(struct build *b, uint32_t block, int type, uint64_t size, uint32_t ext_block,
                        uint32_t ext_len, int data_in_physical) {
  uint32_t lba = meta_lba(b, block);
  uint8_t *d = sec(b, lba);
  int ext = b->rev250;
  size_t ad = ext ? 216 : 176;
  size_t ad_len;

  memset(d, 0, SECTOR);
  put_le16(d + 16 + 4, 4);  // strategy 4
  put_le16(d + 16 + 8, 1);
  d[16 + 11] = (uint8_t)type;
  put_le32(d + 36, 0xFFFFFFFF);
  put_le32(d + 40, 0xFFFFFFFF);
  put_le32(d + 44, 0x14A5);
  put_le16(d + 48, 1);
  put_le64(d + 56, size);
  if (ext) {
    put_le64(d + 64, size);
    put_le64(d + 72, (size + SECTOR - 1) / SECTOR);
  } else {
    put_le64(d + 64, (size + SECTOR - 1) / SECTOR);
  }
  if (data_in_physical && b->rev250) {
    // long_ad into the physical partition (map 0)
    put_le16(d + 16 + 18, 1);
    put_le32(d + ad, ext_len);
    put_le32(d + ad + 4, ext_block);
    put_le16(d + ad + 8, 0);
    ad_len = 16;
  } else {
    put_le16(d + 16 + 18, 0);
    put_le32(d + ad, ext_len);
    put_le32(d + ad + 4, ext_block);
    ad_len = 8;
  }
  put_le32(d + (ext ? 212 : 172), (uint32_t)ad_len);
  tag(d, ext ? 266 : 261, block, ad + ad_len - 16, b->rev250);
}

uint8_t *udfb_build(const struct udfb_file *files, size_t n, int rev250, const char *volume_id, size_t *out_len) {
  struct build b;
  uint32_t meta_next;  // next free block in the metadata area
  uint32_t data_next;  // next free block of file data, in the physical partition
  uint32_t total;
  uint32_t part_len;
  size_t i;
  int k;

  memset(&b, 0, sizeof(b));
  b.rev250 = rev250;
  b.count = 1;  // root
  b.nodes[0].is_dir = 1;
  b.nodes[0].parent = 0;
  for (i = 0; i < n; i++) {
    if (add_path(&b, &files[i]) != 0) {
      return NULL;
    }
  }

  // Blocks: FSD at 0, then for each node its entry, and for directories one
  // block of FIDs (enough for these tests).
  meta_next = 1;
  for (k = 0; k < b.count; k++) {
    b.nodes[k].fe_block = meta_next++;
    if (b.nodes[k].is_dir) {
      b.nodes[k].dir_block = meta_next++;
    }
  }
  b.meta_blocks = meta_next;
  data_next = rev250 ? 1 + b.meta_blocks : b.meta_blocks;
  for (k = 0; k < b.count; k++) {
    if (!b.nodes[k].is_dir) {
      b.nodes[k].data_block = data_next;
      data_next += (uint32_t)((b.nodes[k].len + SECTOR - 1) / SECTOR);
      if (b.nodes[k].len == 0) {
        data_next++;
      }
    }
  }
  part_len = data_next + 16;
  total = PART_START + part_len + 1;
  b.len = (size_t)total * SECTOR;
  b.img = calloc(1, b.len);
  if (!b.img) {
    return NULL;
  }

  // Volume recognition sequence.
  memcpy(sec(&b, 16) + 1, "BEA01", 5);
  sec(&b, 16)[6] = 1;
  memcpy(sec(&b, 17) + 1, rev250 ? "NSR03" : "NSR02", 5);
  sec(&b, 17)[6] = 1;
  memcpy(sec(&b, 18) + 1, "TEA01", 5);
  sec(&b, 18)[6] = 1;

  {  // PVD
    uint8_t *d = sec(&b, VDS);
    put_le32(d + 16, 1);
    dstring(d + 24, 32, volume_id);
    put_le16(d + 64, 1);
    put_le16(d + 66, 1);
    tag(d, 1, VDS, 512 - 16, rev250);
  }
  {  // Partition descriptor
    uint8_t *d = sec(&b, VDS + 1);
    put_le32(d + 16, 2);
    put_le16(d + 20, 1);
    put_le16(d + 22, 0);
    regid(d + 24, rev250 ? "+NSR03" : "+NSR02");
    put_le32(d + 184, 1);
    put_le32(d + 188, PART_START);
    put_le32(d + 192, part_len);
    tag(d, 5, VDS + 1, 512 - 16, rev250);
  }
  {  // Logical volume descriptor
    uint8_t *d = sec(&b, VDS + 2);
    uint8_t *m = d + 440;
    put_le32(d + 16, 3);
    dstring(d + 84, 128, volume_id);
    put_le32(d + 212, SECTOR);
    regid(d + 216, "*OSTA UDF Compliant");
    put_le16(d + 216 + 24, rev250 ? 0x0250 : 0x0102);
    put_le32(d + 248, SECTOR);  // FSD: block 0 of the metadata partition (map 1) or the partition
    put_le32(d + 252, 0);
    put_le16(d + 256, rev250 ? 1 : 0);
    m[0] = 1;
    m[1] = 6;
    put_le16(m + 2, 1);
    put_le16(m + 4, 0);
    if (rev250) {
      uint8_t *t = m + 6;
      t[0] = 2;
      t[1] = 64;
      regid(t + 4, "*UDF Metadata Partition");
      put_le16(t + 36, 1);
      put_le16(t + 38, 0);
      put_le32(t + 40, 0);  // metadata file entry: block 0 of the partition
      put_le32(t + 44, 0xFFFFFFFF);
      put_le32(t + 48, 0xFFFFFFFF);
      put_le32(t + 52, 32);
      put_le16(t + 56, 1);
    }
    put_le32(d + 264, rev250 ? 70 : 6);
    put_le32(d + 268, rev250 ? 2 : 1);
    tag(d, 6, VDS + 2, 440 + (rev250 ? 70 : 6) - 16, rev250);
  }
  tag(sec(&b, VDS + 3), 8, VDS + 3, 512 - 16, rev250);  // terminator

  {  // Anchor
    uint8_t *d = sec(&b, AVDP);
    put_le32(d + 16, 16 * SECTOR);
    put_le32(d + 20, VDS);
    put_le32(d + 24, 16 * SECTOR);
    put_le32(d + 28, VDS);
    tag(d, 2, AVDP, 512 - 16, rev250);
  }

  if (rev250) {
    // The metadata file's entry at partition block 0, its data the metadata blocks.
    uint8_t *d = sec(&b, PART_START);
    {
      size_t ad = 216;
      put_le16(d + 16 + 4, 4);
      put_le16(d + 16 + 8, 1);
      d[16 + 11] = 250;
      put_le64(d + 56, (uint64_t)b.meta_blocks * SECTOR);
      put_le64(d + 64, (uint64_t)b.meta_blocks * SECTOR);
      put_le64(d + 72, b.meta_blocks);
      put_le32(d + ad, b.meta_blocks * SECTOR);
      put_le32(d + ad + 4, 1);  // short_ad: partition block 1
      put_le32(d + 212, 8);
      tag(d, 266, 0, ad + 8 - 16, 1);
    }
  }

  {  // File set descriptor
    uint8_t *d = sec(&b, meta_lba(&b, 0));
    dstring(d + 112, 128, volume_id);
    put_le32(d + 400, SECTOR);
    put_le32(d + 404, b.nodes[0].fe_block);
    put_le16(d + 408, rev250 ? 1 : 0);
    tag(d, 256, 0, 512 - 16, rev250);
  }

  for (k = 0; k < b.count; k++) {
    struct node *nd = &b.nodes[k];
    if (nd->is_dir) {
      uint8_t *dir = sec(&b, meta_lba(&b, nd->dir_block));
      size_t off = write_fid(&b, dir, nd->dir_block, &b.nodes[nd->parent], NULL);
      int c;
      for (c = 1; c < b.count; c++) {
        if (b.nodes[c].parent == k && c != k) {
          off += write_fid(&b, dir + off, nd->dir_block, &b.nodes[c], b.nodes[c].name);
        }
      }
      nd->dir_len = (uint32_t)off;
      write_entry(&b, nd->fe_block, 4, off, nd->dir_block, (uint32_t)off, 0);
    } else {
      if (nd->len) {
        memcpy(sec(&b, PART_START + nd->data_block), nd->data, nd->len);
      }
      write_entry(&b, nd->fe_block, 5, nd->len, nd->data_block, (uint32_t)nd->len, 1);
    }
  }
  *out_len = b.len;
  return b.img;
}
