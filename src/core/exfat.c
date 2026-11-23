#include "ripper/exfat.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "ripper/common.h"
#include "ripper/log.h"
#include "ripper/util.h"

#define SECTOR 512u
#define SECTOR_SHIFT 9
#define CLUSTER (64u * 1024)
#define CLUSTER_SHIFT 16        // bytes
#define SPC (CLUSTER / SECTOR)  // sectors per cluster
#define FAT_OFFSET SPC          // the FAT starts at the first cluster-aligned sector after the boot regions
#define ENTRY 32u
#define UPCASE_BYTES (65536u * 2)
#define ALIGN_BYTES (1024u * 1024)

struct node {
  char path[1024];
  uint16_t name[255];
  unsigned name_len;
  int is_dir;
  uint64_t size;   // a file's length; a directory's entry bytes, rounded to clusters
  uint32_t first;  // first cluster, 0 for an empty file
  uint32_t clusters;
  time_t mtime;
  int parent;
  int first_child;
  int next;
};

struct plan {
  struct node *nodes;
  int count;
  int cap;
  uint32_t used;  // clusters handed out so far (from cluster 2)
  uint64_t data;  // file bytes, for progress
};

/* Names */

// UTF-8 to UTF-16; RIP_ERR for names exFAT can't hold.
static int to_utf16(const char *s, uint16_t *out, unsigned *len) {
  const unsigned char *p = (const unsigned char *)s;
  unsigned n = 0;

  while (*p) {
    uint32_t c;
    int extra;
    if (*p < 0x80) {
      c = *p;
      extra = 0;
    } else if ((*p & 0xE0) == 0xC0) {
      c = *p & 0x1F;
      extra = 1;
    } else if ((*p & 0xF0) == 0xE0) {
      c = *p & 0x0F;
      extra = 2;
    } else if ((*p & 0xF8) == 0xF0) {
      c = *p & 0x07;
      extra = 3;
    } else {
      return RIP_ERR;
    }
    p++;
    while (extra--) {
      if ((*p & 0xC0) != 0x80) {
        return RIP_ERR;
      }
      c = (c << 6) | (*p++ & 0x3F);
    }
    if (c >= 0x10000) {
      if (n + 2 > 255) {
        return RIP_ERR;
      }
      c -= 0x10000;
      out[n++] = (uint16_t)(0xD800 | (c >> 10));
      out[n++] = (uint16_t)(0xDC00 | (c & 0x3FF));
    } else {
      if (n + 1 > 255) {
        return RIP_ERR;
      }
      out[n++] = (uint16_t)c;
    }
  }
  *len = n;
  return n ? RIP_OK : RIP_ERR;
}

// The volume's up-case table: ASCII and Latin-1 letters, the rest mapped to itself.
static uint16_t upcase(uint16_t c) {
  if (c >= 'a' && c <= 'z') {
    return (uint16_t)(c - 32);
  }
  if (c >= 0xE0 && c <= 0xFE && c != 0xF7) {
    return (uint16_t)(c - 32);
  }
  if (c == 0xFF) {
    return 0x178;
  }
  return c;
}

static uint16_t name_hash(const uint16_t *name, unsigned len) {
  uint16_t h = 0;
  unsigned i;

  for (i = 0; i < len; i++) {
    uint16_t u = upcase(name[i]);
    uint8_t b[2] = {(uint8_t)u, (uint8_t)(u >> 8)};
    int k;
    for (k = 0; k < 2; k++) {
      h = (uint16_t)(((h & 1) ? 0x8000 : 0) + (h >> 1) + b[k]);
    }
  }
  return h;
}

/* Planning */

static int add_node(struct plan *p) {
  if (p->count == p->cap) {
    int cap = p->cap ? p->cap * 2 : 256;
    struct node *n = realloc(p->nodes, (size_t)cap * sizeof(*n));
    if (!n) {
      return -1;
    }
    p->nodes = n;
    p->cap = cap;
  }
  memset(&p->nodes[p->count], 0, sizeof(p->nodes[0]));
  p->nodes[p->count].first_child = -1;
  p->nodes[p->count].next = -1;
  return p->count++;
}

static unsigned set_entries(const struct node *n) {
  return 2 + (n->name_len + 14) / 15;  // file, stream extension, names
}

// dir/name into out, which is another node's path than dir. RIP_ERR when it
// doesn't fit.
static int child_path(char *out, size_t len, const char *dir, const char *name) {
  size_t dl = strlen(dir);
  size_t nl = strlen(name);
  if (dl + 1 + nl >= len) {
    out[0] = 0;
    return RIP_ERR;
  }
  memcpy(out, dir, dl);
  out[dl] = '/';
  memcpy(out + dl + 1, name, nl + 1);
  return RIP_OK;
}

// Reads a directory's children into the plan, depth first.
static int scan(struct plan *p, int dir) {
  DIR *d = opendir(p->nodes[dir].path);
  struct dirent *e;
  int last = -1;

  if (!d) {
    RIP_ERROR("exfat: can't read %s", p->nodes[dir].path);
    return RIP_ERR;
  }
  while ((e = readdir(d)) != NULL) {
    struct stat st;
    int i;
    struct node *n;
    if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) {
      continue;
    }
    i = add_node(p);
    if (i < 0) {
      closedir(d);
      return RIP_ENOMEM;
    }
    n = &p->nodes[i];
    if (child_path(n->path, sizeof(n->path), p->nodes[dir].path, e->d_name) != RIP_OK ||
        stat(n->path, &st) != 0 || to_utf16(e->d_name, n->name, &n->name_len) != RIP_OK) {
      RIP_ERROR("exfat: can't take %s/%s", p->nodes[dir].path, e->d_name);
      closedir(d);
      return RIP_ERR;
    }
    n->is_dir = S_ISDIR(st.st_mode);
    n->size = n->is_dir ? 0 : (uint64_t)st.st_size;
    n->mtime = st.st_mtime;
    n->parent = dir;
    if (last < 0) {
      p->nodes[dir].first_child = i;
    } else {
      p->nodes[last].next = i;
    }
    last = i;
    if (!n->is_dir) {
      p->data += n->size;
    }
  }
  closedir(d);
  for (last = p->nodes[dir].first_child; last >= 0; last = p->nodes[last].next) {
    if (p->nodes[last].is_dir) {
      int rc = scan(p, last);
      if (rc != RIP_OK) {
        return rc;
      }
    }
  }
  return RIP_OK;
}

static uint32_t clusters_for(uint64_t bytes) {
  return (uint32_t)((bytes + CLUSTER - 1) / CLUSTER);
}

// Gives each directory its size and every node its clusters, in the order
// they will be written: a directory, then what's in it.
static void allocate(struct plan *p, int dir, int is_root) {
  struct node *d = &p->nodes[dir];
  unsigned entries = is_root ? 3 : 0;  // root: label, bitmap, up-case table
  int c;

  for (c = d->first_child; c >= 0; c = p->nodes[c].next) {
    entries += set_entries(&p->nodes[c]);
  }
  entries++;  // room for the end-of-directory mark
  d->clusters = clusters_for((uint64_t)entries * ENTRY);
  d->size = (uint64_t)d->clusters * CLUSTER;
  d->first = 2 + p->used;
  p->used += d->clusters;
  for (c = d->first_child; c >= 0; c = p->nodes[c].next) {
    struct node *n = &p->nodes[c];
    if (!n->is_dir && n->size) {
      n->clusters = clusters_for(n->size);
      n->first = 2 + p->used;
      p->used += n->clusters;
    }
  }
  for (c = d->first_child; c >= 0; c = p->nodes[c].next) {
    if (p->nodes[c].is_dir) {
      allocate(p, c, 0);
    }
  }
}

/* Writing */

struct out {
  int fd;
  uint64_t heap;  // byte offset of cluster 2
  exfat_progress_fn progress;
  void *ud;
  uint64_t done;
  uint64_t total;
};

static uint64_t cluster_at(const struct out *o, uint32_t cluster) {
  return o->heap + (uint64_t)(cluster - 2) * CLUSTER;
}

// In UTC, said so by each entry's offset fields (put_set).
static uint32_t timestamp(time_t t) {
  struct tm tm;

  gmtime_r(&t, &tm);
  if (tm.tm_year < 80) {
    return (1u << 21) | (1u << 16);  // 1980-01-01
  }
  return ((uint32_t)(tm.tm_year - 80) << 25) | ((uint32_t)(tm.tm_mon + 1) << 21) | ((uint32_t)tm.tm_mday << 16) |
         ((uint32_t)tm.tm_hour << 11) | ((uint32_t)tm.tm_min << 5) | ((uint32_t)tm.tm_sec / 2);
}

static uint16_t set_checksum(const uint8_t *e, unsigned count) {
  uint16_t c = 0;
  unsigned i;

  for (i = 0; i < count * ENTRY; i++) {
    if (i == 2 || i == 3) {
      continue;
    }
    c = (uint16_t)(((c & 1) ? 0x8000 : 0) + (c >> 1) + e[i]);
  }
  return c;
}

// One file or directory's entry set at e; returns the entries used.
static unsigned put_set(uint8_t *e, const struct node *n) {
  unsigned count = set_entries(n);
  uint32_t ts = timestamp(n->mtime);
  uint64_t length = n->size;
  unsigned i;

  memset(e, 0, (size_t)count * ENTRY);
  e[0] = 0x85;
  e[1] = (uint8_t)(count - 1);
  rip_put_le16(e + 4, n->is_dir ? 0x10 : 0x20);
  rip_put_le32(e + 8, ts);
  rip_put_le32(e + 12, ts);
  rip_put_le32(e + 16, ts);
  e[22] = 0x80;  // create, modified, accessed: UTC (offset valid, 0)
  e[23] = 0x80;
  e[24] = 0x80;
  e[32] = 0xC0;
  e[33] = 0x01;  // allocation possible; the FAT chain is valid
  e[35] = (uint8_t)n->name_len;
  rip_put_le16(e + 36, name_hash(n->name, n->name_len));
  rip_put_le32(e + 40, (uint32_t)length);
  rip_put_le32(e + 44, (uint32_t)(length >> 32));
  rip_put_le32(e + 52, n->first);
  rip_put_le32(e + 56, (uint32_t)length);
  rip_put_le32(e + 60, (uint32_t)(length >> 32));
  for (i = 0; i < n->name_len; i++) {
    uint8_t *ne = e + (2 + i / 15) * ENTRY;
    ne[0] = 0xC1;
    rip_put_le16(ne + 2 + (i % 15) * 2, n->name[i]);
  }
  rip_put_le16(e + 2, set_checksum(e, count));
  return count;
}

static int write_dir(struct out *o, const struct plan *p, int dir, int is_root, const uint8_t *root_entries) {
  const struct node *d = &p->nodes[dir];
  uint8_t *buf = calloc(1, (size_t)d->size);
  size_t at = 0;
  int c;
  int rc;

  if (!buf) {
    return RIP_ENOMEM;
  }
  if (is_root) {
    memcpy(buf, root_entries, 3 * ENTRY);
    at = 3 * ENTRY;
  }
  for (c = d->first_child; c >= 0; c = p->nodes[c].next) {
    at += put_set(buf + at, &p->nodes[c]) * ENTRY;
  }
  rc = rip_seek_write(o->fd, cluster_at(o, d->first), buf, (size_t)d->size) == 0 ? RIP_OK : RIP_ERR;
  free(buf);
  return rc;
}

static int write_file(struct out *o, const struct node *n) {
  enum { BUF = 4 << 20 };
  uint8_t *buf;
  uint64_t left = n->size;
  uint64_t at = cluster_at(o, n->first);
  int in;
  int rc = RIP_OK;

  if (!n->size) {
    return RIP_OK;
  }
  buf = malloc(BUF);
  in = open(n->path, O_RDONLY);
  if (!buf || in < 0) {
    RIP_ERROR("exfat: can't open %s: %s", n->path, strerror(errno));
    free(buf);
    if (in >= 0) {
      close(in);
    }
    return RIP_ERR;
  }
  if (lseek(o->fd, (off_t)at, SEEK_SET) < 0) {
    RIP_ERROR("exfat: can't seek to %llu: %s", (unsigned long long)at, strerror(errno));
    rc = RIP_ERR;
  }
  while (rc == RIP_OK && left) {
    long got = rip_read_full(in, buf, left < BUF ? (size_t)left : BUF);
    if (got <= 0) {
      RIP_ERROR("exfat: %s ended %llu bytes early (%s)", n->path, (unsigned long long)left,
                got < 0 ? strerror(errno) : "shorter than it said");
      rc = RIP_ERR;
      break;
    }
    if (rip_write_all(o->fd, buf, (size_t)got) != 0) {
      RIP_ERROR("exfat: can't write %s into the image: %s", n->path, strerror(errno));
      rc = RIP_ERR;
      break;
    }
    left -= (uint64_t)got;
    o->done += (uint64_t)got;
    if (o->progress && o->progress(o->ud, o->done, o->total)) {
      rc = RIP_ECANCEL;
    }
  }
  close(in);
  free(buf);
  return rc;
}

static int write_tree(struct out *o, const struct plan *p, int dir, int is_root, const uint8_t *root_entries) {
  int c;
  int rc = write_dir(o, p, dir, is_root, root_entries);

  for (c = p->nodes[dir].first_child; rc == RIP_OK && c >= 0; c = p->nodes[c].next) {
    rc = p->nodes[c].is_dir ? write_tree(o, p, c, 0, NULL) : write_file(o, &p->nodes[c]);
  }
  return rc;
}

static uint32_t boot_checksum(const uint8_t *s) {
  uint32_t c = 0;
  unsigned i;

  for (i = 0; i < 11 * SECTOR; i++) {
    if (i == 106 || i == 107 || i == 112) {
      continue;
    }
    c = ((c & 1) ? 0x80000000u : 0) + (c >> 1) + s[i];
  }
  return c;
}

static void boot_region(uint8_t *r, uint64_t volume_sectors, uint32_t fat_sectors, uint32_t heap_sector,
                        uint32_t cluster_count, uint32_t root, uint32_t serial, uint8_t percent) {
  uint32_t sum;
  unsigned i;

  memset(r, 0, 12 * SECTOR);
  r[0] = 0xEB;
  r[1] = 0x76;
  r[2] = 0x90;
  memcpy(r + 3, "EXFAT   ", 8);
  rip_put_le32(r + 72, (uint32_t)volume_sectors);
  rip_put_le32(r + 76, (uint32_t)(volume_sectors >> 32));
  rip_put_le32(r + 80, FAT_OFFSET);
  rip_put_le32(r + 84, fat_sectors);
  rip_put_le32(r + 88, heap_sector);
  rip_put_le32(r + 92, cluster_count);
  rip_put_le32(r + 96, root);
  rip_put_le32(r + 100, serial);
  rip_put_le16(r + 104, 0x0100);
  r[108] = SECTOR_SHIFT;
  r[109] = CLUSTER_SHIFT - SECTOR_SHIFT;
  r[110] = 1;     // one FAT
  r[111] = 0x80;  // drive select
  r[112] = percent;
  memset(r + 120, 0xF4, 390);  // boot code: halt
  r[510] = 0x55;
  r[511] = 0xAA;
  for (i = 1; i <= 8; i++) {  // extended boot sectors
    r[i * SECTOR + 510] = 0x55;
    r[i * SECTOR + 511] = 0xAA;
  }
  sum = boot_checksum(r);
  for (i = 0; i < SECTOR / 4; i++) {
    rip_put_le32(r + 11 * SECTOR + i * 4, sum);
  }
}

int exfat_build(const char *src_dir, const char *image_path, const char *label, exfat_progress_fn progress, void *ud) {
  struct plan p;
  struct out o;
  uint8_t *boot = NULL;
  uint8_t *fat = NULL;
  uint8_t *bitmap = NULL;
  uint8_t *upcase_table = NULL;
  uint8_t root_entries[3 * ENTRY];
  uint32_t bitmap_clusters = 1;
  uint32_t cluster_count = 0;
  uint32_t fat_sectors = 0;
  uint32_t heap_sector = 0;
  uint64_t volume_sectors = 0;
  uint32_t upcase_sum = 0;
  uint32_t first_meta;
  uint32_t i;
  int rc = RIP_ERR;
  int k;

  memset(&p, 0, sizeof(p));
  memset(&o, 0, sizeof(o));
  o.fd = -1;
  k = add_node(&p);
  if (k < 0) {
    return RIP_ENOMEM;
  }
  snprintf(p.nodes[0].path, sizeof(p.nodes[0].path), "%s", src_dir);
  p.nodes[0].is_dir = 1;
  p.nodes[0].parent = -1;
  if (scan(&p, 0) != RIP_OK) {
    goto done;
  }

  // The bitmap's size depends on the cluster count, which depends on it:
  // settle it in a few rounds. Cluster 2 on: bitmap, up-case table, the tree.
  for (k = 0; k < 4; k++) {
    uint32_t meta = bitmap_clusters + clusters_for(UPCASE_BYTES);
    uint64_t heap_bytes;
    p.used = meta;
    allocate(&p, 0, 1);
    // The FAT covers every cluster of the volume, the slack of the 1 MiB
    // rounding included: sized for the clusters in use plus that slack.
    fat_sectors = (uint32_t)(((uint64_t)(p.used + 2 + ALIGN_BYTES / CLUSTER) * 4 + SECTOR - 1) / SECTOR);
    fat_sectors = (fat_sectors + SPC - 1) / SPC * SPC;
    heap_sector = FAT_OFFSET + fat_sectors;
    heap_bytes = (uint64_t)p.used * CLUSTER;
    volume_sectors =
        ((uint64_t)heap_sector * SECTOR + heap_bytes + ALIGN_BYTES - 1) / ALIGN_BYTES * ALIGN_BYTES / SECTOR;
    cluster_count = (uint32_t)((volume_sectors - heap_sector) / SPC);
    if ((uint64_t)(cluster_count + 2) * 4 > (uint64_t)fat_sectors * SECTOR) {
      RIP_ERROR("exfat: FAT sizing went wrong");
      goto done;
    }
    if (clusters_for((cluster_count + 7) / 8) == bitmap_clusters) {
      break;
    }
    bitmap_clusters = clusters_for((cluster_count + 7) / 8);
  }
  first_meta = 2;

  // Bitmap: every cluster handed out.
  bitmap = calloc(1, (size_t)bitmap_clusters * CLUSTER);
  fat = calloc(1, (size_t)fat_sectors * SECTOR);
  upcase_table = malloc(UPCASE_BYTES);
  boot = malloc(12 * SECTOR);
  if (!bitmap || !fat || !upcase_table || !boot) {
    rc = RIP_ENOMEM;
    goto done;
  }
  for (i = 0; i < p.used; i++) {
    bitmap[i / 8] |= (uint8_t)(1u << (i % 8));
  }
  // FAT: media and end marks, then a chain for each contiguous run.
  rip_put_le32(fat, 0xFFFFFFF8u);
  rip_put_le32(fat + 4, 0xFFFFFFFFu);
  {
    struct {
      uint32_t first;
      uint32_t count;
    } runs[3] = {{first_meta, bitmap_clusters}, {first_meta + bitmap_clusters, clusters_for(UPCASE_BYTES)}, {0, 0}};
    int r;
    for (r = 0; r < 2; r++) {
      for (i = 0; i < runs[r].count; i++) {
        uint32_t c = runs[r].first + i;
        rip_put_le32(fat + (size_t)c * 4, i + 1 < runs[r].count ? c + 1 : 0xFFFFFFFFu);
      }
    }
    for (k = 0; k < p.count; k++) {
      const struct node *n = &p.nodes[k];
      for (i = 0; i < n->clusters; i++) {
        uint32_t c = n->first + i;
        rip_put_le32(fat + (size_t)c * 4, i + 1 < n->clusters ? c + 1 : 0xFFFFFFFFu);
      }
    }
  }
  for (i = 0; i < 65536; i++) {
    rip_put_le16(upcase_table + i * 2, upcase((uint16_t)i));
  }
  for (i = 0; i < UPCASE_BYTES; i++) {
    upcase_sum = ((upcase_sum & 1) ? 0x80000000u : 0) + (upcase_sum >> 1) + upcase_table[i];
  }
  // Root entries: label, bitmap, up-case table.
  memset(root_entries, 0, sizeof(root_entries));
  {
    uint16_t name[255];
    unsigned len = 0;
    root_entries[0] = 0x83;
    if (label && to_utf16(label, name, &len) == RIP_OK) {
      if (len > 11) {
        len = 11;
      }
      root_entries[1] = (uint8_t)len;
      for (i = 0; i < len; i++) {
        rip_put_le16(root_entries + 2 + i * 2, name[i]);
      }
    }
  }
  root_entries[ENTRY] = 0x81;
  rip_put_le32(root_entries + ENTRY + 20, first_meta);
  rip_put_le32(root_entries + ENTRY + 24, (cluster_count + 7) / 8);
  root_entries[2 * ENTRY] = 0x82;
  rip_put_le32(root_entries + 2 * ENTRY + 4, upcase_sum);
  rip_put_le32(root_entries + 2 * ENTRY + 20, first_meta + bitmap_clusters);
  rip_put_le32(root_entries + 2 * ENTRY + 24, UPCASE_BYTES);

  boot_region(boot, volume_sectors, fat_sectors, heap_sector, cluster_count, p.nodes[0].first,
              (uint32_t)time(NULL) ^ (uint32_t)p.data, (uint8_t)((uint64_t)p.used * 100 / cluster_count));

  o.fd = open(image_path, O_RDWR | O_CREAT | O_TRUNC, 0644);
  o.heap = (uint64_t)heap_sector * SECTOR;
  o.progress = progress;
  o.ud = ud;
  o.total = p.data;
  if (o.fd < 0 || ftruncate(o.fd, (off_t)(volume_sectors * SECTOR)) != 0 ||
      rip_seek_write(o.fd, 0, boot, 12 * SECTOR) != 0 || rip_seek_write(o.fd, 12 * SECTOR, boot, 12 * SECTOR) != 0 ||
      rip_seek_write(o.fd, (uint64_t)FAT_OFFSET * SECTOR, fat, (size_t)fat_sectors * SECTOR) != 0 ||
      rip_seek_write(o.fd, cluster_at(&o, first_meta), bitmap, (size_t)bitmap_clusters * CLUSTER) != 0 ||
      rip_seek_write(o.fd, cluster_at(&o, first_meta + bitmap_clusters), upcase_table, UPCASE_BYTES) != 0) {
    RIP_ERROR("exfat: can't write %s", image_path);
    goto done;
  }
  rc = write_tree(&o, &p, 0, 1, root_entries);
  if (rc == RIP_OK && fsync(o.fd) != 0) {
    RIP_ERROR("exfat: can't finish %s: %s", image_path, strerror(errno));
    rc = RIP_ERR;
  }
  if (rc == RIP_OK) {
    RIP_INFO("exfat: %s, %d entries, %llu MiB in %u of %u clusters", image_path, p.count - 1,
             (unsigned long long)(volume_sectors * SECTOR >> 20), p.used, cluster_count);
  }

done:
  if (o.fd >= 0) {
    close(o.fd);
    if (rc != RIP_OK) {
      unlink(image_path);
    }
  }
  free(boot);
  free(fat);
  free(bitmap);
  free(upcase_table);
  free(p.nodes);
  return rc;
}
