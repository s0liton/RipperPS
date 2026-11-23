// PFS images for PS4 packages, ported from LibOrbisPkg (OpenOrbis fork,
// https://github.com/OpenOrbis/LibOrbisPkg, LGPL-3.0, by maxton and
// contributors): PFS/PFSBuilder.cs, PFSCWriter.cs, FlatPathTable.cs and
// PfsStructs.cs. Conveyed under the GPL-3.0 as LGPL-3.0 section 2 allows.
//
// The layout is LibOrbisPkg's block for block, with two fixes: a directory
// whose entries spill into another block is sized to hold them (LibOrbisPkg
// sizes it by the entries' bytes and loses the last ones), and a flat path
// table or collision resolver of more than 12 blocks gets all its blocks.
//
// Streamed in one pass over the game: each 64 KiB block of pfs_image.dat is
// made, signed, encrypted and written in turn, its signature going to the
// indirect block it belongs in, which is written as soon as it fills. The
// metadata in front (inodes, header) follows, then the image is read back
// for its SHA-256, which covers those first blocks.

#include "pfs.h"

#include <dirent.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "ripper/aes.h"
#include "ripper/common.h"
#include "ripper/hash.h"
#include "ripper/log.h"
#include "ripper/util.h"

#define INNER_INODE 0xA8u                           // DinodeD32
#define OUTER_INODE 0x2C8u                          // DinodeS32
#define INODES_PER_BLOCK (PFS_BLOCK / INNER_INODE)  // 390
#define SIG 36u                                     // HMAC-SHA256, then a 32-bit block number
#define SIGS_PER_BLOCK (PFS_BLOCK / SIG)            // 1820
#define DIRENT_ROOM 280u                            // no dirent starts in a block's last 280 bytes
#define FPT_PER_BLOCK (PFS_BLOCK / 8)
#define MIN_INNER_BLOCKS 0x55u  // LibOrbisPkg's minimum for a game's inner image (5.3 MiB)
#define PFS_MAGIC 20130315

#define MODE_DIR 0x416D  // directory, r-x for everyone
#define MODE_FILE 0x816D
#define FL_COMPRESSED 0x1
#define FL_SIGNED 0xC  // set on every inode of a signed image
#define FL_READONLY 0x10
#define FL_INTERNAL 0x20000
#define PDT_FILE 2
#define PDT_DIR 3
#define PDT_DOT 4
#define PDT_DOTDOT 5

// The outer image: header, one block of inodes (superroot, flat_path_table,
// uroot, pfs_image.dat), the superroot's entries, the flat path table, a
// zero block left unencrypted, pfs_image.dat's indirect blocks, uroot's
// entries, then pfs_image.dat.
#define OB_INODES 1
#define OB_SUPERROOT 2
#define OB_FPT 3
#define OB_EMPTY 4
#define OB_IND 5
#define DIRECT 12
#define MAX_IMAGE_BLOCKS ((uint64_t)DIRECT + SIGS_PER_BLOCK + (uint64_t)SIGS_PER_BLOCK * SIGS_PER_BLOCK)

#define BATCH 64  // blocks signed and encrypted together: 4 MiB
#define MAX_WORKERS 7

#define N_DIR 1
#define N_ENTRY 2  // a package entry, not in the image
#define N_DROP 4   // made by the package instead
#define N_MEM 8    // the generated keystone

struct node {
  uint32_t name;  // pool offsets, NUL-terminated
  uint32_t path;  // below the folder: "/sce_sys/param.sfo"; the root's is ""
  uint32_t path_len;
  uint16_t name_len;
  uint16_t flags;
  int32_t parent;
  int32_t first;  // children, sorted by name
  int32_t next;
  uint32_t ino;
  uint32_t subdirs;
  uint32_t start;  // first block in the inner image
  uint32_t blocks;
  uint64_t size;  // a file's bytes
};

struct pfs_image {
  char *folder;
  char *pool;
  size_t pool_len;
  size_t pool_cap;
  struct node *nodes;
  uint32_t count;
  uint32_t cap;
  int32_t sce_sys;
  struct pfs_entry *entries;
  size_t nentries;
  int keystone;  // the folder has sce_sys/keystone, or one was added
  uint8_t keystone_data[96];
  int64_t time;

  // Layout.
  uint32_t *dirs;  // every directory but the root, by path: inode order
  uint32_t ndirs;
  uint32_t *files;  // files in the image, by path: inode order
  uint32_t nfiles;
  uint32_t *order;  // data block order: root, dirs, then files folder by folder
  uint32_t norder;
  uint64_t *fpt;  // hash << 32 | value, by hash
  uint32_t nfpt;
  uint8_t *cr;  // collision_resolver
  uint32_t cr_size;
  uint32_t ninodes;
  uint32_t uroot_ino;
  uint32_t inode_blocks;
  uint32_t sr_block;
  uint32_t fpt_block;
  uint32_t fpt_blocks;
  uint32_t cr_block;
  uint32_t cr_blocks;
  uint32_t data_start;
  uint32_t data_end;
  uint32_t inner_blocks;
  uint64_t pfsc_size;  // PFSC header bytes
  uint64_t img_size;   // pfs_image.dat
  uint64_t img_blocks;
  uint64_t ind_blocks;
  uint64_t outer_blocks;
};

static void set_err(char *err, size_t en, const char *msg) {
  if (en) {
    snprintf(err, en, "%s", msg);
  }
}

static void put_le64(uint8_t *p, uint64_t v) {
  rip_put_le32(p, (uint32_t)v);
  rip_put_le32(p + 4, (uint32_t)(v >> 32));
}

static uint64_t ceil_div(uint64_t a, uint64_t b) {
  return (a + b - 1) / b;
}

/* Names and order */

static unsigned weight(unsigned char c) {
  static const char order[] = " _-,;:!?.'\"()[]{}@*/\\&#%`^+<=>|~$0123456789";
  const char *at;

  if (c >= 'A' && c <= 'Z') {
    c = (unsigned char)(c - 'A' + 'a');
  }
  if (c >= 'a' && c <= 'z') {
    return 100u + (c - 'a');
  }
  if (c && (at = strchr(order, c)) != NULL) {
    return (unsigned)(at - order);
  }
  return 200u + c;
}

int pfs_name_cmp(const char *a, size_t al, const char *b, size_t bl) {
  size_t n = al < bl ? al : bl;
  size_t i;

  for (i = 0; i < n; i++) {
    unsigned wa = weight((unsigned char)a[i]);
    unsigned wb = weight((unsigned char)b[i]);
    if (wa != wb) {
      return wa < wb ? -1 : 1;
    }
  }
  if (al != bl) {
    return al < bl ? -1 : 1;
  }
  for (i = 0; i < n; i++) {
    int ua = a[i] >= 'A' && a[i] <= 'Z';
    int ub = b[i] >= 'A' && b[i] <= 'Z';
    if (ua != ub) {
      return ua ? 1 : -1;
    }
  }
  return 0;
}

// The flat path table's hash: uppercase ASCII, times 31.
static uint32_t path_hash(const char *s, size_t len) {
  uint32_t h = 0;
  size_t i;

  for (i = 0; i < len; i++) {
    unsigned char c = (unsigned char)s[i];
    if (c >= 'a' && c <= 'z') {
      c = (unsigned char)(c - 32);
    }
    h = c + 31 * h;
  }
  return h;
}

static const char *node_name(const struct pfs_image *p, const struct node *n) {
  return p->pool + n->name;
}

static const char *node_path(const struct pfs_image *p, const struct node *n) {
  return p->pool + n->path;
}

typedef int (*cmp_fn)(const struct pfs_image *p, uint32_t a, uint32_t b);

static int by_name(const struct pfs_image *p, uint32_t a, uint32_t b) {
  const struct node *x = &p->nodes[a];
  const struct node *y = &p->nodes[b];
  return pfs_name_cmp(node_name(p, x), x->name_len, node_name(p, y), y->name_len);
}

static int by_path(const struct pfs_image *p, uint32_t a, uint32_t b) {
  const struct node *x = &p->nodes[a];
  const struct node *y = &p->nodes[b];
  return pfs_name_cmp(node_path(p, x), x->path_len, node_path(p, y), y->path_len);
}

// A stable merge sort with a context, which qsort lacks.
static void merge_sort(const struct pfs_image *p, uint32_t *v, uint32_t *tmp, size_t n, cmp_fn cmp) {
  size_t half = n / 2;
  size_t i = 0;
  size_t j = half;
  size_t k = 0;

  if (n < 2) {
    return;
  }
  merge_sort(p, v, tmp, half, cmp);
  merge_sort(p, v + half, tmp, n - half, cmp);
  while (i < half && j < n) {
    tmp[k++] = cmp(p, v[j], v[i]) < 0 ? v[j++] : v[i++];
  }
  while (i < half) {
    tmp[k++] = v[i++];
  }
  while (j < n) {
    tmp[k++] = v[j++];
  }
  memcpy(v, tmp, n * sizeof(*v));
}

static int sort_nodes(const struct pfs_image *p, uint32_t *v, size_t n, cmp_fn cmp) {
  uint32_t *tmp = malloc((n ? n : 1) * sizeof(*tmp));

  if (!tmp) {
    return RIP_ENOMEM;
  }
  merge_sort(p, v, tmp, n, cmp);
  free(tmp);
  return RIP_OK;
}

/* Scanning */

static int pool_add(struct pfs_image *p, const char *a, size_t al, const char *b, size_t bl, uint32_t *off) {
  size_t need = p->pool_len + al + bl + 1;

  if (need > UINT32_MAX) {
    return RIP_ENOMEM;
  }
  if (need > p->pool_cap) {
    size_t cap = p->pool_cap ? p->pool_cap : 65536;
    char *np;
    while (cap < need) {
      cap *= 2;
    }
    np = realloc(p->pool, cap);
    if (!np) {
      return RIP_ENOMEM;
    }
    p->pool = np;
    p->pool_cap = cap;
  }
  *off = (uint32_t)p->pool_len;
  memcpy(p->pool + p->pool_len, a, al);
  memcpy(p->pool + p->pool_len + al, b, bl);
  p->pool[p->pool_len + al + bl] = '\0';
  p->pool_len = need;
  return RIP_OK;
}

static int add_node(struct pfs_image *p, uint32_t *idx) {
  if (p->count == p->cap) {
    uint32_t cap = p->cap ? p->cap * 2 : 1024;
    struct node *n = realloc(p->nodes, (size_t)cap * sizeof(*n));
    if (!n) {
      return RIP_ENOMEM;
    }
    p->nodes = n;
    p->cap = cap;
  }
  memset(&p->nodes[p->count], 0, sizeof(p->nodes[0]));
  p->nodes[p->count].first = -1;
  p->nodes[p->count].next = -1;
  p->nodes[p->count].parent = -1;
  *idx = p->count++;
  return RIP_OK;
}

static int source_path(const struct pfs_image *p, const struct node *n, char *out, size_t len) {
  return snprintf(out, len, "%s%s", p->folder, node_path(p, n)) < (int)len ? RIP_OK : RIP_ERR;
}

static int scan_dir(struct pfs_image *p, uint32_t dir, int depth, char *err, size_t en) {
  // On the heap: this recurses once per folder level.
  struct {
    char src[4096];
    char parent[4096];
    char child[4096];
  } *buf = malloc(sizeof(*buf));
  char *src = buf ? buf->src : NULL;
  char *parent_path = buf ? buf->parent : NULL;
  char *child = buf ? buf->child : NULL;
  size_t parent_len = p->nodes[dir].path_len;
  uint32_t *kids = NULL;
  size_t nkids = 0;
  size_t cap = 0;
  struct dirent *e;
  DIR *d;
  size_t i;
  int rc = RIP_OK;

  if (!buf) {
    set_err(err, en, "out of memory");
    return RIP_ENOMEM;
  }
  if (depth > 64 || source_path(p, &p->nodes[dir], src, 4096) != RIP_OK || parent_len >= 4096) {
    free(buf);
    set_err(err, en, "the game folder is nested too deeply");
    return RIP_ERR;
  }
  memcpy(parent_path, node_path(p, &p->nodes[dir]), parent_len + 1);
  d = opendir(src);
  if (!d) {
    RIP_ERROR("fpkg: can't read %s", src);
    free(buf);
    set_err(err, en, "can't read the game folder");
    return RIP_ERR;
  }
  while ((e = readdir(d)) != NULL) {
    size_t len = strlen(e->d_name);
    struct stat st;
    struct node *n;
    uint32_t idx;
    if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) {
      continue;
    }
    if (len > 255 || snprintf(child, 4096, "%s/%s", src, e->d_name) >= 4096 || stat(child, &st) != 0 ||
        (!S_ISDIR(st.st_mode) && !S_ISREG(st.st_mode))) {
      RIP_ERROR("fpkg: can't take %s/%s", src, e->d_name);
      set_err(err, en, "the game folder has a file that can't be read");
      rc = RIP_ERR;
      break;
    }
    if (nkids == cap) {
      size_t nc = cap ? cap * 2 : 64;
      uint32_t *nk = realloc(kids, nc * sizeof(*nk));
      if (!nk) {
        rc = RIP_ENOMEM;
        break;
      }
      kids = nk;
      cap = nc;
    }
    if ((rc = add_node(p, &idx)) != RIP_OK) {
      break;
    }
    n = &p->nodes[idx];
    n->parent = (int32_t)dir;
    n->name_len = (uint16_t)len;
    n->path_len = (uint32_t)(parent_len + 1 + len);
    n->flags = S_ISDIR(st.st_mode) ? N_DIR : 0;
    n->size = S_ISDIR(st.st_mode) ? 0 : (uint64_t)st.st_size;
    if ((rc = pool_add(p, e->d_name, len, "", 0, &p->nodes[idx].name)) != RIP_OK) {
      break;
    }
    if (parent_len + 1 + len >= 4096) {
      rc = RIP_ENOMEM;
      break;
    }
    memcpy(child, parent_path, parent_len);
    child[parent_len] = '/';
    if ((rc = pool_add(p, child, parent_len + 1, e->d_name, len, &p->nodes[idx].path)) != RIP_OK) {
      break;
    }
    if (S_ISDIR(st.st_mode)) {
      p->nodes[dir].subdirs++;
    }
    kids[nkids++] = idx;
  }
  closedir(d);
  if (rc == RIP_ENOMEM) {
    set_err(err, en, "out of memory");
  }
  if (rc == RIP_OK) {
    rc = sort_nodes(p, kids, nkids, by_name);
  }
  if (rc == RIP_OK) {
    for (i = 0; i < nkids; i++) {
      if (i + 1 < nkids) {
        p->nodes[kids[i]].next = (int32_t)kids[i + 1];
      }
    }
    p->nodes[dir].first = nkids ? (int32_t)kids[0] : -1;
    for (i = 0; i < nkids && rc == RIP_OK; i++) {
      if (p->nodes[kids[i]].flags & N_DIR) {
        rc = scan_dir(p, kids[i], depth + 1, err, en);
      }
    }
  }
  free(kids);
  free(buf);
  return rc;
}

// LibOrbisPkg's FSDir.GetAllChildrenDirs: a folder's subfolders, then each
// one's own, recursively.
static void all_dirs(const struct pfs_image *p, uint32_t dir, uint32_t *out, uint32_t *n) {
  uint32_t first = *n;
  uint32_t last;
  int32_t c;
  uint32_t i;

  for (c = p->nodes[dir].first; c >= 0; c = p->nodes[c].next) {
    if (p->nodes[c].flags & N_DIR) {
      out[(*n)++] = (uint32_t)c;
    }
  }
  last = *n;
  for (i = first; i < last; i++) {
    all_dirs(p, out[i], out, n);
  }
}

// FSDir.GetAllChildrenFiles: a folder's files, then those of each folder in
// all_dirs order. Those with all of want and none of skip.
static uint32_t all_files(const struct pfs_image *p, uint32_t dir, uint16_t want, uint16_t skip, uint32_t *out,
                          uint32_t *dirs) {
  uint32_t nd = 0;
  uint32_t n = 0;
  uint32_t i;

  all_dirs(p, dir, dirs, &nd);
  for (i = 0; i <= nd; i++) {
    uint32_t d = i == 0 ? dir : dirs[i - 1];
    int32_t c;
    for (c = p->nodes[d].first; c >= 0; c = p->nodes[c].next) {
      uint16_t f = p->nodes[c].flags;
      if (!(f & N_DIR) && (f & want) == want && !(f & skip)) {
        out[n++] = (uint32_t)c;
      }
    }
  }
  return n;
}

int pfs_scan(const char *folder, struct pfs_image **out, char *err, size_t en) {
  struct pfs_image *p = calloc(1, sizeof(*p));
  uint32_t root;
  uint32_t *list = NULL;
  uint32_t *dirs = NULL;
  uint32_t n;
  uint32_t i;
  int32_t c;
  int rc;

  *out = NULL;
  if (!p || !(p->folder = strdup(folder))) {
    free(p);
    set_err(err, en, "out of memory");
    return RIP_ENOMEM;
  }
  p->sce_sys = -1;
  if ((rc = add_node(p, &root)) != RIP_OK || (rc = pool_add(p, "", 0, "", 0, &p->nodes[root].name)) != RIP_OK) {
    pfs_free(p);
    set_err(err, en, "out of memory");
    return rc;
  }
  p->nodes[root].path = p->nodes[root].name;
  p->nodes[root].flags = N_DIR;
  rc = scan_dir(p, root, 0, err, en);
  if (rc != RIP_OK) {
    pfs_free(p);
    return rc;
  }
  for (c = p->nodes[root].first; c >= 0; c = p->nodes[c].next) {
    if ((p->nodes[c].flags & N_DIR) && !strcmp(node_name(p, &p->nodes[c]), "sce_sys")) {
      p->sce_sys = c;
    }
  }
  if (p->sce_sys < 0) {
    pfs_free(p);
    set_err(err, en, "the game folder has no sce_sys folder");
    return RIP_ENOENT;
  }

  // Files below sce_sys with an entry name leave the image for the package.
  list = malloc((size_t)p->count * sizeof(*list));
  dirs = malloc((size_t)p->count * sizeof(*dirs));
  p->entries = calloc(p->count, sizeof(*p->entries));
  if (!list || !dirs || !p->entries) {
    free(list);
    free(dirs);
    pfs_free(p);
    set_err(err, en, "out of memory");
    return RIP_ENOMEM;
  }
  n = all_files(p, (uint32_t)p->sce_sys, 0, 0, list, dirs);
  for (i = 0; i < n; i++) {
    struct node *f = &p->nodes[list[i]];
    const char *rel = node_path(p, f) + strlen("/sce_sys/");
    uint32_t id = fpkg_entry_id(rel);
    if (!strcmp(rel, "keystone")) {
      p->keystone = 1;
    }
    if (id == FPKG_ENTRY_DROP) {
      f->flags |= N_DROP;
    } else if (id) {
      struct pfs_entry *e = &p->entries[p->nentries++];
      char src[4096];
      f->flags |= N_ENTRY;
      e->id = id;
      e->size = f->size;
      if (source_path(p, f, src, sizeof(src)) != RIP_OK || !(e->name = strdup(rel)) || !(e->path = strdup(src))) {
        free(list);
        free(dirs);
        pfs_free(p);
        set_err(err, en, "out of memory");
        return RIP_ENOMEM;
      }
    }
  }
  free(list);
  free(dirs);
  *out = p;
  return RIP_OK;
}

void pfs_free(struct pfs_image *p) {
  size_t i;

  if (!p) {
    return;
  }
  for (i = 0; i < p->nentries; i++) {
    free((char *)p->entries[i].name);
    free((char *)p->entries[i].path);
  }
  free(p->entries);
  free(p->folder);
  free(p->pool);
  free(p->nodes);
  free(p->dirs);
  free(p->files);
  free(p->order);
  free(p->fpt);
  free(p->cr);
  free(p);
}

size_t pfs_entry_count(const struct pfs_image *p) {
  return p->nentries;
}

const struct pfs_entry *pfs_entry_at(const struct pfs_image *p, size_t i) {
  return &p->entries[i];
}

int pfs_has_keystone(const struct pfs_image *p) {
  return p->keystone;
}

int pfs_add_keystone(struct pfs_image *p, const uint8_t data[96]) {
  uint32_t idx;
  int32_t c;
  struct node *n;
  struct node *s;

  if (p->keystone || add_node(p, &idx) != RIP_OK) {
    return RIP_ERR;
  }
  n = &p->nodes[idx];
  n->parent = p->sce_sys;
  n->flags = N_MEM;
  n->size = 96;
  n->name_len = 8;
  if (pool_add(p, "keystone", 8, "", 0, &p->nodes[idx].name) != RIP_OK ||
      pool_add(p, "/sce_sys/", 9, "keystone", 8, &p->nodes[idx].path) != RIP_OK) {
    return RIP_ENOMEM;
  }
  p->nodes[idx].path_len = 17;
  // LibOrbisPkg adds it after sce_sys's other files.
  s = &p->nodes[p->sce_sys];
  if (s->first < 0) {
    s->first = (int32_t)idx;
  } else {
    for (c = s->first; p->nodes[c].next >= 0; c = p->nodes[c].next) {
    }
    p->nodes[c].next = (int32_t)idx;
  }
  memcpy(p->keystone_data, data, 96);
  p->keystone = 1;
  return RIP_OK;
}

/* Layout */

static uint32_t entsize(uint32_t name_len) {
  return (name_len + 17 + 7) & ~7u;
}

static int in_image(const struct node *n) {
  return !(n->flags & (N_ENTRY | N_DROP));
}

// Calls fn for each of a directory's entries in order: ".", "..", folders,
// then files. Returns the blocks they take.
typedef void (*dirent_fn)(void *ctx, uint32_t off, uint32_t ino, uint32_t type, const char *name, uint32_t len);

static uint32_t walk_dir(const struct pfs_image *p, uint32_t dir, dirent_fn fn, void *ctx) {
  const struct node *d = &p->nodes[dir];
  uint32_t parent_ino = d->parent >= 0 ? p->nodes[d->parent].ino : d->ino;
  uint32_t blk = 0;
  uint32_t off = 0;
  uint32_t last = 0;
  int pass;
  int32_t c;

#define EMIT(ino, type, name, len)                                  \
  do {                                                              \
    if (fn) {                                                       \
      fn(ctx, blk * PFS_BLOCK + off, (ino), (type), (name), (len)); \
    }                                                               \
    last = blk;                                                     \
    off += entsize(len);                                            \
    if (off > PFS_BLOCK - DIRENT_ROOM) {                            \
      blk++;                                                        \
      off = 0;                                                      \
    }                                                               \
  } while (0)

  EMIT(d->ino, PDT_DOT, ".", 1);
  EMIT(parent_ino, PDT_DOTDOT, "..", 2);
  for (pass = 0; pass < 2; pass++) {
    for (c = d->first; c >= 0; c = p->nodes[c].next) {
      const struct node *n = &p->nodes[c];
      int is_dir = (n->flags & N_DIR) != 0;
      if (is_dir == (pass == 0) && in_image(n)) {
        EMIT(n->ino, is_dir ? PDT_DIR : PDT_FILE, node_name(p, n), n->name_len);
      }
    }
  }
#undef EMIT
  return last + 1;
}

static void put_dirent(uint8_t *at, uint32_t ino, uint32_t type, const char *name, uint32_t len) {
  rip_put_le32(at, ino);
  rip_put_le32(at + 4, type);
  rip_put_le32(at + 8, len);
  rip_put_le32(at + 12, entsize(len));
  memcpy(at + 16, name, len);
}

static void dirent_to_buf(void *ctx, uint32_t off, uint32_t ino, uint32_t type, const char *name, uint32_t len) {
  put_dirent((uint8_t *)ctx + off, ino, type, name, len);
}

struct fpt_item {
  uint32_t hash;
  uint32_t node;
};

// By hash, then by place in the list, which orders a collision's entries.
static int fpt_cmp(const void *a, const void *b) {
  const struct fpt_item *x = a;
  const struct fpt_item *y = b;
  if (x->hash != y->hash) {
    return x->hash < y->hash ? -1 : 1;
  }
  return x->node < y->node ? -1 : x->node > y->node;
}

// The flat path table and, when two paths share a hash, the collision
// resolver: lists of dirents with full paths, each list followed by 0x18
// zero bytes. Nodes in LibOrbisPkg's order: folders by path, then files.
// Without assign, only finds out whether there is a collision (inode
// numbers depend on it).
static int build_fpt(struct pfs_image *p, const uint32_t *nodes, uint32_t n, int assign) {
  struct fpt_item *items = malloc((n ? n : 1) * sizeof(*items));
  uint32_t i;
  uint32_t j;
  uint32_t off = 0;
  int collision = 0;

  if (!items) {
    return RIP_ENOMEM;
  }
  for (i = 0; i < n; i++) {
    const struct node *x = &p->nodes[nodes[i]];
    items[i].hash = path_hash(node_path(p, x), x->path_len);
    items[i].node = i;
  }
  qsort(items, n, sizeof(*items), fpt_cmp);
  for (i = 1; i < n; i++) {
    collision |= items[i].hash == items[i - 1].hash;
  }
  if (!assign) {
    p->cr_size = (uint32_t)collision;
    free(items);
    return RIP_OK;
  }
  free(p->fpt);
  p->fpt = malloc((n ? n : 1) * sizeof(*p->fpt));
  p->nfpt = 0;
  p->cr_size = 0;
  for (i = 0; i < n; i = j) {
    for (j = i + 1; j < n && items[j].hash == items[i].hash; j++) {
    }
    if (j - i > 1) {
      for (; i < j; i++) {
        p->cr_size += entsize(p->nodes[nodes[items[i].node]].path_len);
      }
      p->cr_size += 0x18;
    }
  }
  p->cr = p->cr_size ? calloc(1, p->cr_size) : NULL;
  if (!p->fpt || (p->cr_size && !p->cr)) {
    free(items);
    return RIP_ENOMEM;
  }
  for (i = 0; i < n; i = j) {
    for (j = i + 1; j < n && items[j].hash == items[i].hash; j++) {
    }
    if (j - i == 1) {
      const struct node *x = &p->nodes[nodes[items[i].node]];
      uint32_t v = x->ino | ((x->flags & N_DIR) ? 0x20000000u : 0);
      p->fpt[p->nfpt++] = (uint64_t)items[i].hash << 32 | v;
    } else {
      p->fpt[p->nfpt++] = (uint64_t)items[i].hash << 32 | 0x80000000u | off;
      for (; i < j; i++) {
        const struct node *x = &p->nodes[nodes[items[i].node]];
        put_dirent(p->cr + off, x->ino, (x->flags & N_DIR) ? PDT_DIR : PDT_FILE, node_path(p, x), x->path_len);
        off += entsize(x->path_len);
      }
      off += 0x18;
    }
  }
  free(items);
  return RIP_OK;
}

static uint64_t indirect_blocks(uint64_t blocks) {
  uint64_t ib = 0;

  if (blocks > DIRECT) {
    blocks -= DIRECT;
    ib++;
  }
  if (blocks > SIGS_PER_BLOCK) {
    blocks -= SIGS_PER_BLOCK;
    ib += 1 + ceil_div(blocks, SIGS_PER_BLOCK);
  }
  return ib;
}

int pfs_plan(struct pfs_image *p, int64_t time, char *err, size_t en) {
  uint32_t *fpt_nodes = NULL;
  uint32_t *scratch = NULL;
  uint32_t i;
  uint32_t nb;
  uint64_t table;
  int has_cr;
  int rc = RIP_ENOMEM;

  p->time = time;
  p->dirs = malloc((size_t)p->count * sizeof(*p->dirs));
  p->files = malloc((size_t)p->count * sizeof(*p->files));
  p->order = malloc((size_t)p->count * sizeof(*p->order));
  fpt_nodes = malloc((size_t)p->count * sizeof(*fpt_nodes));
  scratch = malloc((size_t)p->count * sizeof(*scratch));
  if (!p->dirs || !p->files || !p->order || !fpt_nodes || !scratch) {
    goto done;
  }
  p->ndirs = 0;
  p->nfiles = 0;
  for (i = 1; i < p->count; i++) {
    if (p->nodes[i].flags & N_DIR) {
      p->dirs[p->ndirs++] = i;
    } else if (in_image(&p->nodes[i])) {
      p->files[p->nfiles++] = i;
    }
  }
  if (sort_nodes(p, p->dirs, p->ndirs, by_path) != RIP_OK || sort_nodes(p, p->files, p->nfiles, by_path) != RIP_OK) {
    goto done;
  }

  // Data order, and the flat path table's: folders by path, then files folder
  // by folder.
  p->norder = 0;
  p->order[p->norder++] = 0;
  memcpy(p->order + p->norder, p->dirs, (size_t)p->ndirs * sizeof(*p->dirs));
  p->norder += p->ndirs;
  p->norder += all_files(p, 0, 0, N_ENTRY | N_DROP, p->order + p->norder, scratch);
  memcpy(fpt_nodes, p->order + 1, (size_t)(p->norder - 1) * sizeof(*fpt_nodes));

  if (build_fpt(p, fpt_nodes, p->norder - 1, 0) != RIP_OK) {
    goto done;
  }
  has_cr = p->cr_size != 0;
  p->uroot_ino = has_cr ? 3 : 2;
  p->nodes[0].ino = p->uroot_ino;
  for (i = 0; i < p->ndirs; i++) {
    p->nodes[p->dirs[i]].ino = p->uroot_ino + 1 + i;
  }
  for (i = 0; i < p->nfiles; i++) {
    p->nodes[p->files[i]].ino = p->uroot_ino + 1 + p->ndirs + i;
  }
  p->ninodes = p->uroot_ino + 1 + p->ndirs + p->nfiles;
  if (build_fpt(p, fpt_nodes, p->norder - 1, 1) != RIP_OK) {
    goto done;
  }

  // Inner image: header, inodes, superroot, flat path table, collision
  // resolver or an empty block, then every node contiguous in data order.
  nb = 1;
  p->inode_blocks = (uint32_t)ceil_div(p->ninodes, INODES_PER_BLOCK);
  nb += p->inode_blocks;
  p->sr_block = nb++;
  p->fpt_block = nb;
  p->fpt_blocks = (uint32_t)ceil_div((uint64_t)p->nfpt * 8, PFS_BLOCK);
  nb += p->fpt_blocks ? p->fpt_blocks : 1;
  p->cr_block = nb;
  p->cr_blocks = (uint32_t)ceil_div(p->cr_size, PFS_BLOCK);
  nb += p->cr_blocks ? p->cr_blocks : 1;
  p->data_start = nb;
  for (i = 0; i < p->norder; i++) {
    struct node *n = &p->nodes[p->order[i]];
    uint64_t blocks = (n->flags & N_DIR) ? walk_dir(p, p->order[i], NULL, NULL) : ceil_div(n->size, PFS_BLOCK);
    if (nb + blocks > INT32_MAX) {
      set_err(err, en, "the game is too big for a package");
      rc = RIP_ERR;
      goto done;
    }
    n->start = nb;
    n->blocks = (uint32_t)blocks;
    nb += (uint32_t)blocks;
  }
  p->data_end = nb;
  p->inner_blocks = nb < MIN_INNER_BLOCKS ? MIN_INNER_BLOCKS : nb;

  // pfs_image.dat: a PFSC header with a table of nb + 1 offsets at 0x400,
  // grown by whole blocks past 0xFC00 bytes.
  table = 8 + (uint64_t)p->inner_blocks * 8;
  p->pfsc_size = PFS_BLOCK + (table > 0xFC00 ? ceil_div(table - 0xFC00, PFS_BLOCK) * PFS_BLOCK : 0);
  p->img_size = p->pfsc_size + (uint64_t)p->inner_blocks * PFS_BLOCK;
  p->img_blocks = p->img_size / PFS_BLOCK;
  if (p->img_blocks > MAX_IMAGE_BLOCKS) {
    set_err(err, en, "the game is too big for a package");
    rc = RIP_ERR;
    goto done;
  }
  p->ind_blocks = indirect_blocks(p->img_blocks);
  p->outer_blocks = OB_IND + p->ind_blocks + 1 + p->img_blocks;
  rc = RIP_OK;

done:
  if (rc == RIP_ENOMEM) {
    set_err(err, en, "out of memory");
  }
  free(fpt_nodes);
  free(scratch);
  return rc;
}

uint64_t pfs_inner_size(const struct pfs_image *p) {
  return (uint64_t)p->inner_blocks * PFS_BLOCK;
}

uint64_t pfs_outer_size(const struct pfs_image *p) {
  return p->outer_blocks * PFS_BLOCK;
}

/* Inodes and headers */

struct inode {
  uint16_t mode;
  uint16_t nlink;
  uint32_t flags;
  uint64_t size;
  uint64_t size_c;
  uint32_t blocks;
};

static void put_inode_head(uint8_t *at, const struct inode *n, int64_t time) {
  int i;

  rip_put_le16(at, n->mode);
  rip_put_le16(at + 2, n->nlink);
  rip_put_le32(at + 4, n->flags);
  put_le64(at + 8, n->size);
  put_le64(at + 0x10, n->size_c);
  for (i = 0; i < 4; i++) {
    put_le64(at + 0x18 + 8 * i, (uint64_t)time);
  }
  rip_put_le32(at + 0x60, n->blocks);
}

// DinodeD32. Data is contiguous: db[0] is the first block, the rest of the
// used direct pointers are -1 (or, with real, the following blocks).
static void put_inner_inode(uint8_t *at, const struct inode *n, uint32_t start, int real, int64_t time) {
  uint32_t i;

  put_inode_head(at, n, time);
  rip_put_le32(at + 0x64, start);
  for (i = 1; i < n->blocks && i < DIRECT; i++) {
    rip_put_le32(at + 0x64 + 4 * i, real ? start + i : 0xFFFFFFFFu);
  }
}

static void inner_inode(const struct pfs_image *p, uint32_t ino, uint8_t *at) {
  struct inode n;
  int has_cr = p->cr_size != 0;

  memset(&n, 0, sizeof(n));
  n.nlink = 1;
  if (ino == 0) {
    n.mode = MODE_DIR;
    n.flags = FL_INTERNAL | FL_READONLY;
    n.size = n.size_c = PFS_BLOCK;
    n.blocks = 1;
    put_inner_inode(at, &n, p->sr_block, 1, p->time);
  } else if (ino == 1) {
    n.mode = MODE_FILE;
    n.flags = FL_INTERNAL | FL_READONLY;
    n.size = n.size_c = (uint64_t)p->nfpt * 8;
    n.blocks = p->fpt_blocks;
    put_inner_inode(at, &n, p->fpt_block, 1, p->time);
  } else if (ino == 2 && has_cr) {
    n.mode = MODE_FILE;
    n.flags = FL_INTERNAL | FL_READONLY;
    n.size = n.size_c = p->cr_size;
    n.blocks = p->cr_blocks;
    put_inner_inode(at, &n, p->cr_block, 1, p->time);
  } else {
    uint32_t k = ino - p->uroot_ino;
    const struct node *x;
    if (k == 0) {
      x = &p->nodes[0];
    } else if (k <= p->ndirs) {
      x = &p->nodes[p->dirs[k - 1]];
    } else {
      x = &p->nodes[p->files[k - 1 - p->ndirs]];
    }
    n.flags = FL_READONLY;
    n.blocks = x->blocks;
    if (x->flags & N_DIR) {
      n.mode = MODE_DIR;
      n.nlink = (uint16_t)((k == 0 ? 3 : 2) + x->subdirs);
      n.size = (uint64_t)x->blocks * PFS_BLOCK;
      // uroot keeps the one block LibOrbisPkg first gives it.
      n.size_c = k == 0 ? PFS_BLOCK : n.size;
    } else {
      n.mode = MODE_FILE;
      n.size = n.size_c = x->size;
    }
    put_inner_inode(at, &n, x->start, 0, p->time);
  }
}

// The superblock. inode_sigs is where block 0's DinodeS64 describing the inode
// blocks keeps its pointers.
static void put_superblock(uint8_t *b, int outer, uint64_t inodes, uint64_t blocks, uint32_t inode_blocks,
                           int64_t time) {
  struct inode n;
  uint32_t i;

  memset(b, 0, PFS_BLOCK);
  put_le64(b, 1);
  put_le64(b + 8, PFS_MAGIC);
  b[0x1A] = 1;  // read-only
  rip_put_le16(b + 0x1C, outer ? 0xD : 0x8);
  rip_put_le32(b + 0x20, PFS_BLOCK);
  put_le64(b + 0x28, 1);
  put_le64(b + 0x30, inodes);
  put_le64(b + 0x38, blocks);
  put_le64(b + 0x40, inode_blocks);
  memset(&n, 0, sizeof(n));
  n.nlink = 1;
  n.flags = outer ? 0 : FL_READONLY;
  n.size = n.size_c = (uint64_t)inode_blocks * PFS_BLOCK;
  n.blocks = inode_blocks;
  put_inode_head(b + 0x50, &n, time);
  for (i = 0; i < inode_blocks && i < DIRECT; i++) {
    put_le64(b + 0x50 + 0x68 + 40 * i + 32, i == 0 || outer ? 1 + i : UINT64_MAX);
  }
  if (outer) {
    rip_put_le32(b + 0x36C, 1);  // key index; the 16-byte seed after it stays zero
  } else {
    rip_put_le32(b + 0x368, 1);
  }
}

/* The inner image as a stream of blocks */

struct gen {
  struct pfs_image *p;
  uint64_t k;       // next pfs_image.dat block
  uint32_t cursor;  // into p->order
  uint32_t node_blk;
  int fd;
  uint8_t *dir;
  char *err;
  size_t en;
};

static void pfsc_block(const struct pfs_image *p, uint64_t k, uint8_t *b) {
  uint64_t lo = k * PFS_BLOCK;
  uint64_t hi = lo + PFS_BLOCK;
  uint64_t i;

  if (k == 0) {
    memcpy(b, "PFSC", 4);
    rip_put_le32(b + 8, 6);
    rip_put_le32(b + 0xC, PFS_BLOCK);
    put_le64(b + 0x10, PFS_BLOCK);
    put_le64(b + 0x18, 0x400);
    put_le64(b + 0x20, p->pfsc_size);
    put_le64(b + 0x28, (uint64_t)p->inner_blocks * PFS_BLOCK);
  }
  // Offsets of the stored blocks, and one past the last.
  for (i = lo < 0x400 ? 0 : (lo - 0x400) / 8; i <= p->inner_blocks && 0x400 + 8 * i < hi; i++) {
    put_le64(b + (0x400 + 8 * i - lo), p->pfsc_size + i * PFS_BLOCK);
  }
}

static void close_node(struct gen *g) {
  if (g->fd >= 0) {
    close(g->fd);
    g->fd = -1;
  }
  free(g->dir);
  g->dir = NULL;
}

static int node_block(struct gen *g, uint32_t j, uint8_t *b) {
  struct pfs_image *p = g->p;
  struct node *n;

  while (g->cursor < p->norder && g->node_blk >= p->nodes[p->order[g->cursor]].blocks) {
    close_node(g);
    g->cursor++;
    g->node_blk = 0;
  }
  if (g->cursor >= p->norder || p->nodes[p->order[g->cursor]].start + g->node_blk != j) {
    set_err(g->err, g->en, "the package layout is inconsistent");
    return RIP_ERR;
  }
  n = &p->nodes[p->order[g->cursor]];
  if (g->node_blk == 0) {
    if (n->flags & N_DIR) {
      g->dir = calloc(n->blocks, PFS_BLOCK);
      if (!g->dir) {
        set_err(g->err, g->en, "out of memory");
        return RIP_ENOMEM;
      }
      walk_dir(p, p->order[g->cursor], dirent_to_buf, g->dir);
    } else if (!(n->flags & N_MEM)) {
      char src[4096];
      if (source_path(p, n, src, sizeof(src)) != RIP_OK || (g->fd = open(src, O_RDONLY)) < 0) {
        RIP_ERROR("fpkg: can't open %s", src);
        set_err(g->err, g->en, "can't read a file in the game folder");
        return RIP_ERR;
      }
    }
  }
  if (n->flags & N_DIR) {
    memcpy(b, g->dir + (size_t)g->node_blk * PFS_BLOCK, PFS_BLOCK);
  } else if (n->flags & N_MEM) {
    memcpy(b, p->keystone_data, sizeof(p->keystone_data));
  } else {
    uint64_t left = n->size - (uint64_t)g->node_blk * PFS_BLOCK;
    size_t want = left < PFS_BLOCK ? (size_t)left : PFS_BLOCK;
    if (rip_read_full(g->fd, b, want) != (long)want) {
      RIP_ERROR("fpkg: short read in %s%s", p->folder, node_path(p, n));
      set_err(g->err, g->en, "a file in the game folder changed while it was read");
      return RIP_ERR;
    }
  }
  g->node_blk++;
  return RIP_OK;
}

static int inner_block(struct gen *g, uint32_t j, uint8_t *b) {
  struct pfs_image *p = g->p;

  if (j == 0) {
    put_superblock(b, 0, p->ninodes, p->inner_blocks, p->inode_blocks, p->time);
  } else if (j < 1 + p->inode_blocks) {
    uint32_t first = (j - 1) * INODES_PER_BLOCK;
    uint32_t i;
    for (i = first; i < p->ninodes && i < first + INODES_PER_BLOCK; i++) {
      inner_inode(p, i, b + (i - first) * INNER_INODE);
    }
  } else if (j == p->sr_block) {
    uint32_t off = 0;
    put_dirent(b, 1, PDT_FILE, "flat_path_table", 15);
    off += entsize(15);
    if (p->cr_size) {
      put_dirent(b + off, 2, PDT_FILE, "collision_resolver", 18);
      off += entsize(18);
    }
    put_dirent(b + off, p->uroot_ino, PDT_DIR, "uroot", 5);
  } else if (j >= p->fpt_block && j < p->fpt_block + p->fpt_blocks) {
    uint32_t first = (j - p->fpt_block) * FPT_PER_BLOCK;
    uint32_t i;
    for (i = first; i < p->nfpt && i < first + FPT_PER_BLOCK; i++) {
      rip_put_le32(b + 8 * (i - first), (uint32_t)(p->fpt[i] >> 32));
      rip_put_le32(b + 8 * (i - first) + 4, (uint32_t)p->fpt[i]);
    }
  } else if (p->cr_size && j >= p->cr_block && j < p->cr_block + p->cr_blocks) {
    uint64_t off = (uint64_t)(j - p->cr_block) * PFS_BLOCK;
    uint64_t len = p->cr_size - off < PFS_BLOCK ? p->cr_size - off : PFS_BLOCK;
    memcpy(b, p->cr + off, (size_t)len);
  } else if (j >= p->data_start && j < p->data_end) {
    return node_block(g, j, b);
  }
  return RIP_OK;
}

static int gen_block(struct gen *g, uint8_t *b) {
  uint64_t hdr = g->p->pfsc_size / PFS_BLOCK;
  uint64_t k = g->k++;

  memset(b, 0, PFS_BLOCK);
  if (k < hdr) {
    pfsc_block(g->p, k, b);
    return RIP_OK;
  }
  return inner_block(g, (uint32_t)(k - hdr), b);
}

/* Signing and encrypting, on every core */

struct keys {
  struct rip_hmac_sha256 sign;  // keyed, nothing hashed yet
  struct rip_aes128 data;
  struct rip_aes128 tweak;
};

struct batch {
  const struct keys *k;
  uint8_t *buf;
  uint64_t first_block;
  int count;
  uint8_t sig[BATCH][32];
  uint8_t chunk[BATCH][4];
  int next;
};

// Worker threads run one job at a time while the caller does the I/O: every
// worker calls fn(arg), which shares the work out itself.
struct pool {
  pthread_t th[MAX_WORKERS];
  int n;
  pthread_mutex_t mu;
  pthread_cond_t go;
  pthread_cond_t done;
  unsigned gen;
  int busy;
  int quit;
  void (*fn)(void *arg);
  void *arg;
};

static void encrypt_block(const struct keys *k, uint64_t block, uint8_t *b) {
  int s;

  for (s = 0; s < 16; s++) {
    rip_aes128_xts_encrypt(&k->data, &k->tweak, block * 16 + (uint64_t)s, b + s * 0x1000, 0x1000);
  }
}

static void chunk_hash(const uint8_t *b, uint8_t out[4]) {
  struct rip_sha256 h;
  uint8_t d[32];

  rip_sha256_init(&h);
  rip_sha256_update(&h, b, PFS_BLOCK);
  rip_sha256_final(&h, d);
  memcpy(out, d, 4);
}

// Sign the plain block, encrypt it, hash what's written.
static void run_batch(void *arg) {
  struct batch *bt = arg;

  for (;;) {
    int i = __atomic_fetch_add(&bt->next, 1, __ATOMIC_RELAXED);
    struct rip_hmac_sha256 h;
    uint8_t *b;
    if (i >= bt->count) {
      return;
    }
    b = bt->buf + (size_t)i * PFS_BLOCK;
    h = bt->k->sign;
    rip_hmac_sha256_update(&h, b, PFS_BLOCK);
    rip_hmac_sha256_final(&h, bt->sig[i]);
    encrypt_block(bt->k, bt->first_block + (uint64_t)i, b);
    chunk_hash(b, bt->chunk[i]);
  }
}

static void *worker(void *arg) {
  struct pool *pl = arg;
  unsigned seen = 0;

  pthread_mutex_lock(&pl->mu);
  for (;;) {
    while (pl->gen == seen && !pl->quit) {
      pthread_cond_wait(&pl->go, &pl->mu);
    }
    if (pl->quit) {
      break;
    }
    seen = pl->gen;
    pthread_mutex_unlock(&pl->mu);
    pl->fn(pl->arg);
    pthread_mutex_lock(&pl->mu);
    if (--pl->busy == 0) {
      pthread_cond_signal(&pl->done);
    }
  }
  pthread_mutex_unlock(&pl->mu);
  return NULL;
}

static void pool_start(struct pool *pl) {
  long cpus = sysconf(_SC_NPROCESSORS_ONLN);
  int want = cpus > 1 ? (int)cpus - 1 : 1;
  int i;

  memset(pl, 0, sizeof(*pl));
  pthread_mutex_init(&pl->mu, NULL);
  pthread_cond_init(&pl->go, NULL);
  pthread_cond_init(&pl->done, NULL);
  if (want > MAX_WORKERS) {
    want = MAX_WORKERS;
  }
  for (i = 0; i < want; i++) {
    if (pthread_create(&pl->th[i], NULL, worker, pl) != 0) {
      break;
    }
    pl->n++;
  }
}

// Starts fn on the workers, or runs it here when there are none.
static void pool_dispatch(struct pool *pl, void (*fn)(void *), void *arg) {
  if (pl->n == 0) {
    fn(arg);
    return;
  }
  pthread_mutex_lock(&pl->mu);
  pl->fn = fn;
  pl->arg = arg;
  pl->busy = pl->n;
  pl->gen++;
  pthread_cond_broadcast(&pl->go);
  pthread_mutex_unlock(&pl->mu);
}

static void pool_wait(struct pool *pl) {
  pthread_mutex_lock(&pl->mu);
  while (pl->busy > 0) {
    pthread_cond_wait(&pl->done, &pl->mu);
  }
  pthread_mutex_unlock(&pl->mu);
}

static void pool_stop(struct pool *pl) {
  int i;

  pthread_mutex_lock(&pl->mu);
  pl->quit = 1;
  pthread_cond_broadcast(&pl->go);
  pthread_mutex_unlock(&pl->mu);
  for (i = 0; i < pl->n; i++) {
    pthread_join(pl->th[i], NULL);
  }
  pthread_mutex_destroy(&pl->mu);
  pthread_cond_destroy(&pl->go);
  pthread_cond_destroy(&pl->done);
}

// Reading back for the image digest: one worker reads the next chunk while
// the caller hashes the last.
struct read_job {
  int fd;
  uint64_t off;
  size_t len;
  uint8_t *buf;
  int taken;
  int failed;
};

static void run_read(void *arg) {
  struct read_job *r = arg;

  if (__atomic_exchange_n(&r->taken, 1, __ATOMIC_ACQ_REL) == 0) {
    r->failed = rip_seek_read(r->fd, r->off, r->buf, r->len) != 0;
  }
}

/* The outer image */

struct writer {
  struct pfs_image *p;
  int fd;
  uint64_t offset;
  struct keys k;
  uint8_t *chunk_sha;
  uint8_t *tmp;
  uint8_t inodes[4][OUTER_INODE];
  uint8_t *ib0;  // pfs_image.dat's indirect block
  uint8_t *ib1;  // its doubly indirect block
  uint8_t *ind;  // the indirect block being filled below ib1
  char *err;
  size_t en;
};

static void put_sig(uint8_t *rec, const uint8_t sig[32], uint64_t block) {
  memcpy(rec, sig, 32);
  rip_put_le32(rec + 32, (uint32_t)block);
}

static int write_block(struct writer *w, uint64_t block, const uint8_t *b, size_t len) {
  if (rip_seek_write(w->fd, w->offset + block * PFS_BLOCK, b, len) != 0) {
    set_err(w->err, w->en, "can't write the package");
    return RIP_ERR;
  }
  return RIP_OK;
}

// Signs a metadata block into rec, then encrypts and writes it.
static int seal(struct writer *w, uint64_t block, const uint8_t *plain, uint8_t *rec, int encrypt) {
  struct rip_hmac_sha256 h = w->k.sign;
  uint8_t sig[32];

  rip_hmac_sha256_update(&h, plain, PFS_BLOCK);
  rip_hmac_sha256_final(&h, sig);
  if (rec) {
    put_sig(rec, sig, block);
  }
  memcpy(w->tmp, plain, PFS_BLOCK);
  if (encrypt) {
    encrypt_block(&w->k, block, w->tmp);
  }
  chunk_hash(w->tmp, w->chunk_sha + 4 * block);
  return write_block(w, block, w->tmp, PFS_BLOCK);
}

static uint8_t *inode_rec(struct writer *w, int ino, int idx) {
  return w->inodes[ino] + 0x64 + SIG * (uint32_t)idx;
}

// Where data block x (0 is uroot's entries, then pfs_image.dat) signs to.
static int place_sig(struct writer *w, uint64_t x, const uint8_t sig[32]) {
  struct pfs_image *p = w->p;
  uint64_t block = OB_IND + p->ind_blocks + x;
  uint64_t j;

  if (x == 0) {
    put_sig(inode_rec(w, 2, 0), sig, block);
    return RIP_OK;
  }
  j = x - 1;
  if (j < DIRECT) {
    put_sig(inode_rec(w, 3, (int)j), sig, block);
  } else if (j < DIRECT + SIGS_PER_BLOCK) {
    put_sig(w->ib0 + SIG * (j - DIRECT), sig, block);
    if (j == DIRECT + SIGS_PER_BLOCK - 1 || j == p->img_blocks - 1) {
      return seal(w, OB_IND, w->ib0, inode_rec(w, 3, DIRECT), 1);
    }
  } else {
    uint64_t q = (j - DIRECT - SIGS_PER_BLOCK) / SIGS_PER_BLOCK;
    uint64_t r = (j - DIRECT - SIGS_PER_BLOCK) % SIGS_PER_BLOCK;
    put_sig(w->ind + SIG * r, sig, block);
    if (r == SIGS_PER_BLOCK - 1 || j == p->img_blocks - 1) {
      int rc = seal(w, OB_IND + 2 + q, w->ind, w->ib1 + SIG * q, 1);
      memset(w->ind, 0, PFS_BLOCK);
      if (rc != RIP_OK) {
        return rc;
      }
      if (j == p->img_blocks - 1) {
        return seal(w, OB_IND + 1, w->ib1, inode_rec(w, 3, DIRECT + 1), 1);
      }
    }
  }
  return RIP_OK;
}

static void outer_inodes(struct writer *w) {
  struct pfs_image *p = w->p;
  struct inode n;

  memset(w->inodes, 0, sizeof(w->inodes));
  memset(&n, 0, sizeof(n));
  n.nlink = 1;
  n.mode = MODE_DIR;
  n.flags = FL_INTERNAL | FL_SIGNED;
  n.size = n.size_c = PFS_BLOCK;
  n.blocks = 1;
  put_inode_head(w->inodes[0], &n, p->time);
  n.mode = MODE_FILE;
  n.size = n.size_c = 8;
  put_inode_head(w->inodes[1], &n, p->time);
  n.mode = MODE_DIR;
  n.nlink = 3;
  n.flags = FL_SIGNED;
  n.size = n.size_c = PFS_BLOCK;
  put_inode_head(w->inodes[2], &n, p->time);
  n.mode = MODE_FILE;
  n.nlink = 1;
  n.flags = FL_SIGNED | FL_COMPRESSED;
  n.size = p->img_size;
  n.size_c = (uint64_t)p->inner_blocks * PFS_BLOCK;
  n.blocks = (uint32_t)p->img_blocks;
  put_inode_head(w->inodes[3], &n, p->time);
  rip_put_le32(inode_rec(w, 3, 0) + 32, (uint32_t)(OB_IND + p->ind_blocks + 1));
}

static int report(iso_progress_fn progress, void *ud, uint64_t done, uint64_t total, char *err, size_t en) {
  if (progress && progress(ud, done, total)) {
    set_err(err, en, "cancelled");
    return RIP_ECANCEL;
  }
  return RIP_OK;
}

static int write_metadata(struct writer *w, uint8_t *b, uint8_t header[PFS_BLOCK]) {
  struct pfs_image *p = w->p;
  uint32_t i;
  int rc;

  // Flat path table: just "/pfs_image.dat", inode 3.
  memset(b, 0, PFS_BLOCK);
  rip_put_le32(b, path_hash("/pfs_image.dat", 14));
  rip_put_le32(b + 4, 3);
  if ((rc = seal(w, OB_FPT, b, inode_rec(w, 1, 0), 1)) != RIP_OK) {
    return rc;
  }
  memset(b, 0, PFS_BLOCK);
  put_dirent(b, 1, PDT_FILE, "flat_path_table", 15);
  put_dirent(b + entsize(15), 2, PDT_DIR, "uroot", 5);
  if ((rc = seal(w, OB_SUPERROOT, b, inode_rec(w, 0, 0), 1)) != RIP_OK) {
    return rc;
  }
  // The zero block after the table stays unencrypted.
  memset(b, 0, PFS_BLOCK);
  if ((rc = seal(w, OB_EMPTY, b, NULL, 0)) != RIP_OK) {
    return rc;
  }
  for (i = 0; i < 4; i++) {
    memcpy(b + i * OUTER_INODE, w->inodes[i], OUTER_INODE);
  }
  put_superblock(header, 1, 4, p->outer_blocks, 1, p->time);
  if ((rc = seal(w, OB_INODES, b, header + 0xB8, 1)) != RIP_OK) {
    return rc;
  }
  {
    struct rip_hmac_sha256 h = w->k.sign;
    uint8_t sig[32];
    rip_hmac_sha256_update(&h, header, 0x5A0);
    rip_hmac_sha256_final(&h, sig);
    put_sig(header + 0x380, sig, 0);
  }
  return seal(w, 0, header, NULL, 0);
}

// Fills a batch with the next plain blocks of the data region (x counts from
// uroot's entries).
static int fill_batch(struct gen *g, struct batch *bt, uint64_t first, uint64_t x, uint64_t count) {
  int rc = RIP_OK;
  int i;

  bt->count = count - x < BATCH ? (int)(count - x) : BATCH;
  bt->first_block = first + x;
  bt->next = 0;
  for (i = 0; i < bt->count && rc == RIP_OK; i++) {
    uint8_t *b = bt->buf + (size_t)i * PFS_BLOCK;
    if (x + (uint64_t)i == 0) {
      // The outer uroot: ".", "..", pfs_image.dat.
      memset(b, 0, PFS_BLOCK);
      put_dirent(b, 2, PDT_DOT, ".", 1);
      put_dirent(b + entsize(1), 2, PDT_DOTDOT, "..", 2);
      put_dirent(b + entsize(1) + entsize(2), 3, PDT_FILE, "pfs_image.dat", 13);
    } else {
      rc = gen_block(g, b);
    }
  }
  return rc;
}

// Files a signed and encrypted batch: its signatures, chunk hashes, bytes.
static int finish_batch(struct writer *w, struct batch *bt, uint64_t first) {
  uint64_t x = bt->first_block - first;
  int rc = RIP_OK;
  int i;

  for (i = 0; i < bt->count && rc == RIP_OK; i++) {
    memcpy(w->chunk_sha + 4 * (bt->first_block + (uint64_t)i), bt->chunk[i], 4);
    rc = place_sig(w, x + (uint64_t)i, bt->sig[i]);
  }
  return rc == RIP_OK ? write_block(w, bt->first_block, bt->buf, (size_t)bt->count * PFS_BLOCK) : rc;
}

// The image's SHA-256, read back: its front was written last.
static int digest_image(struct writer *w, struct pool *pl, uint8_t *bufs[2], uint8_t out[32], iso_progress_fn progress,
                        void *ud, uint64_t total) {
  struct rip_sha256 h;
  struct read_job r[2];
  uint64_t size = pfs_outer_size(w->p);
  uint64_t chunk = (uint64_t)BATCH * PFS_BLOCK;
  uint64_t done = 0;
  int cur = 0;
  int rc = RIP_OK;

  memset(r, 0, sizeof(r));
  rip_sha256_init(&h);
  r[0].fd = w->fd;
  r[0].off = w->offset;
  r[0].len = (size_t)(size < chunk ? size : chunk);
  r[0].buf = bufs[0];
  pool_dispatch(pl, run_read, &r[0]);
  while (done < size) {
    struct read_job *now = &r[cur];
    struct read_job *next = &r[cur ^ 1];
    pool_wait(pl);
    if (now->failed) {
      set_err(w->err, w->en, "can't read the package back");
      rc = RIP_ERR;
      break;
    }
    if (done + now->len < size) {
      uint64_t left = size - done - now->len;
      memset(next, 0, sizeof(*next));
      next->fd = w->fd;
      next->off = now->off + now->len;
      next->len = (size_t)(left < chunk ? left : chunk);
      next->buf = bufs[cur ^ 1];
      pool_dispatch(pl, run_read, next);
    }
    rip_sha256_update(&h, now->buf, now->len);
    done += now->len;
    cur ^= 1;
    rc = report(progress, ud, size + done, total, w->err, w->en);
    if (rc != RIP_OK) {
      if (done < size) {
        pool_wait(pl);
      }
      break;
    }
  }
  rip_sha256_final(&h, out);
  return rc;
}

int pfs_write(struct pfs_image *p, int fd, uint64_t offset, const uint8_t ekpfs[32], uint8_t *chunk_sha,
              uint8_t image_digest[32], uint8_t signed_digest[32], iso_progress_fn progress, void *ud, char *err,
              size_t en) {
  struct writer w;
  struct gen g;
  struct pool pl;
  struct batch *bt[2] = {NULL, NULL};
  uint8_t *header = NULL;
  uint8_t seed[20];
  uint8_t key[32];
  uint64_t first = OB_IND + p->ind_blocks;
  uint64_t count = p->outer_blocks - first;
  uint64_t total = 2 * pfs_outer_size(p);
  uint64_t x;
  int cur = 0;
  int i;
  int rc = RIP_OK;

  memset(&w, 0, sizeof(w));
  memset(&g, 0, sizeof(g));
  w.p = p;
  w.fd = fd;
  w.offset = offset;
  w.chunk_sha = chunk_sha;
  w.err = err;
  w.en = en;
  g.p = p;
  g.fd = -1;
  g.err = err;
  g.en = en;

  // Keys from EKPFS and the all-zero seed: HMAC(EKPFS, LE32(i) || seed),
  // 1 for XTS (tweak key, then data key), 2 for signing.
  memset(seed, 0, sizeof(seed));
  rip_put_le32(seed, 1);
  rip_hmac_sha256(ekpfs, 32, seed, sizeof(seed), key);
  rip_aes128_init_enc(&w.k.tweak, key);
  rip_aes128_init_enc(&w.k.data, key + 16);
  rip_put_le32(seed, 2);
  rip_hmac_sha256(ekpfs, 32, seed, sizeof(seed), key);
  rip_hmac_sha256_init(&w.k.sign, key, sizeof(key));

  header = malloc(PFS_BLOCK);
  w.tmp = malloc(PFS_BLOCK);
  w.ib0 = calloc(1, PFS_BLOCK);
  w.ib1 = calloc(1, PFS_BLOCK);
  w.ind = calloc(1, PFS_BLOCK);
  for (i = 0; i < 2; i++) {
    bt[i] = calloc(1, sizeof(*bt[i]));
    if (bt[i]) {
      bt[i]->buf = malloc((size_t)BATCH * PFS_BLOCK);
      bt[i]->k = &w.k;
    }
  }
  if (!bt[0] || !bt[0]->buf || !bt[1] || !bt[1]->buf || !header || !w.tmp || !w.ib0 || !w.ib1 || !w.ind) {
    set_err(err, en, "out of memory");
    rc = RIP_ENOMEM;
    goto free_bufs;
  }
  outer_inodes(&w);

  // While the workers sign and encrypt one batch, this thread reads the
  // next and writes the last.
  pool_start(&pl);
  rc = fill_batch(&g, bt[0], first, 0, count);
  if (rc == RIP_OK) {
    pool_dispatch(&pl, run_batch, bt[0]);
    for (x = (uint64_t)bt[0]->count;; x += (uint64_t)bt[cur]->count) {
      struct batch *now = bt[cur];
      struct batch *next = bt[cur ^ 1];
      int more = x < count;
      int frc = more ? fill_batch(&g, next, first, x, count) : RIP_OK;
      pool_wait(&pl);
      if (frc != RIP_OK) {
        rc = frc;
        break;
      }
      if (more) {
        pool_dispatch(&pl, run_batch, next);
      }
      rc = finish_batch(&w, now, first);
      if (rc == RIP_OK) {
        rc = report(progress, ud, (now->first_block + (uint64_t)now->count) * PFS_BLOCK, total, err, en);
      }
      if (rc != RIP_OK || !more) {
        if (more) {
          pool_wait(&pl);
        }
        break;
      }
      cur ^= 1;
    }
  }
  close_node(&g);
  if (rc == RIP_OK) {
    rc = write_metadata(&w, bt[0]->buf, header);
  }
  if (rc == RIP_OK) {
    uint8_t *bufs[2];
    struct rip_sha256 h;
    rip_sha256_init(&h);
    rip_sha256_update(&h, header, PFS_BLOCK);
    rip_sha256_final(&h, signed_digest);
    bufs[0] = bt[0]->buf;
    bufs[1] = bt[1]->buf;
    rc = digest_image(&w, &pl, bufs, image_digest, progress, ud, total);
  }
  pool_stop(&pl);

free_bufs:
  for (i = 0; i < 2; i++) {
    if (bt[i]) {
      free(bt[i]->buf);
    }
    free(bt[i]);
  }
  free(header);
  free(w.tmp);
  free(w.ib0);
  free(w.ib1);
  free(w.ind);
  return rc;
}
