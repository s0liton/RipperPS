#include "ripper/ufs2.h"

#include <dirent.h>
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
#include "ufs2_layout.h"

// One block size for blocks and fragments: frag is 1, so every address is a
// block number and no file ends in a partial block.
#define BSIZE 65536u
#define BSHIFT 16
#define NINDIR (BSIZE / 8)   // block pointers in an indirect block
#define INOPB (BSIZE / 256)  // inodes in a block
#define SBLKNO 2             // howmany(SBLOCK_UFS2 + SBLOCKSIZE, BSIZE)
#define CBLKNO (SBLKNO + 1)
#define IBLKNO (CBLKNO + 1)
#define MAXCONTIG 2  // maxphys (128 KiB on the PS5's kernel) / BSIZE
#define CGSIZEFUDGE 8
#define DENSITY_FRAGS 4  // a block per inode... times four: newfs -i 262144
#define MIN_BLOCKS 128   // 8 MiB, so a near-empty image still has two groups' worth
#define MODE_DIR 0040555
#define MODE_FILE 0100555
#define DT_DIR_ 4
#define DT_REG_ 8

// A file's blocks in the order they're handed out: 12 direct, the single
// indirect block, its NINDIR blocks, then the double indirect block followed
// by each indirect block it holds and that block's NINDIR blocks. Its data
// and the blocks that map it sit together, as the kernel would place them.
#define SEQ_IND1 UFS_NDADDR
#define SEQ_DIND (SEQ_IND1 + 1 + NINDIR)
#define DIRECT_MAX ((uint64_t)UFS_NDADDR)
#define SINGLE_MAX (DIRECT_MAX + NINDIR)
#define DOUBLE_MAX (SINGLE_MAX + (uint64_t)NINDIR * NINDIR)

struct node {
  char path[1024];
  char name[256];
  unsigned name_len;
  int is_dir;
  uint64_t size;    // a file's length; a directory's entry bytes
  uint64_t blocks;  // data blocks
  uint64_t seq;     // first of this node's blocks in allocation order
  uint64_t count;   // data and indirect blocks
  time_t mtime;
  uint32_t ino;
  unsigned subdirs;
  int parent;
  int first_child;
  int next;
};

struct plan {
  struct node *nodes;
  int count;
  int cap;
  uint64_t used;  // blocks handed out so far, in allocation order
  uint64_t data;  // file bytes, for progress
};

struct geom {
  uint64_t size;  // blocks in the image
  uint32_t fpg;   // blocks per cylinder group
  uint32_t ipg;   // inodes per cylinder group
  uint32_t ncg;
  uint32_t dblkno;
  uint32_t cgsize;
};

/* Geometry */

static uint64_t howmany(uint64_t x, uint64_t y) {
  return (x + y - 1) / y;
}

static uint64_t roundup(uint64_t x, uint64_t y) {
  return howmany(x, y) * y;
}

static uint64_t cg_bytes(uint64_t fpg, uint64_t ipg) {
  // CGSIZE: struct cg, the old rotational tables for one cylinder, the inode
  // map, the block map with its spare word, the cluster summary and map.
  return sizeof(struct ufs_cg) + 4 + 2 + howmany(ipg, 8) + howmany(fpg, 8) + 4 + MAXCONTIG * 4 + howmany(fpg, 8);
}

static uint32_t ipg_for(uint64_t fpg, uint64_t frags_per_inode) {
  return (uint32_t)roundup(howmany(fpg, frags_per_inode), INOPB);
}

// mkfs's choice of group size for an image of g->size blocks: the smallest
// group meeting the inode density, grown until its map fills a block, then
// shrunk until the last group can hold its own metadata.
static int geometry(struct geom *g, uint64_t frags_per_inode) {
  uint64_t size = g->size;
  uint64_t minfpg = frags_per_inode * INOPB;
  uint64_t fpg;
  uint64_t ipg;

  if (minfpg > size) {
    minfpg = size;
  }
  ipg = INOPB;
  fpg = IBLKNO + ipg / INOPB;
  if (fpg < minfpg) {
    fpg = minfpg;
  }
  ipg = ipg_for(fpg, frags_per_inode);
  fpg = IBLKNO + ipg / INOPB;
  if (fpg < minfpg) {
    fpg = minfpg;
  }
  ipg = ipg_for(fpg, frags_per_inode);
  if (cg_bytes(fpg, ipg) >= BSIZE - CGSIZEFUDGE) {
    return RIP_ERR;
  }
  for (; fpg < size - 1; fpg++) {
    ipg = ipg_for(fpg, frags_per_inode);
    if (size / fpg < 1) {
      break;
    }
    if (cg_bytes(fpg, ipg) < BSIZE - CGSIZEFUDGE) {
      continue;
    }
    if (cg_bytes(fpg, ipg) == BSIZE - CGSIZEFUDGE) {
      break;
    }
    fpg--;
    ipg = ipg_for(fpg, frags_per_inode);
    break;
  }
  for (;;) {
    uint64_t lastmin = IBLKNO + ipg / INOPB;
    if (size < lastmin) {
      return RIP_ERR;
    }
    if (size % fpg >= lastmin || size % fpg == 0) {
      break;
    }
    fpg--;
    ipg = ipg_for(fpg, frags_per_inode);
  }
  g->fpg = (uint32_t)fpg;
  g->ipg = (uint32_t)ipg;
  g->ncg = (uint32_t)howmany(size, fpg);
  g->dblkno = (uint32_t)(IBLKNO + ipg / INOPB);
  g->cgsize = BSIZE;
  return RIP_OK;
}

static uint64_t cg_base(const struct geom *g, uint32_t c) {
  return (uint64_t)g->fpg * c;
}

static uint32_t cg_blocks(const struct geom *g, uint32_t c) {
  uint64_t end = cg_base(g, c) + g->fpg;
  return (uint32_t)((end > g->size ? g->size : end) - cg_base(g, c));
}

// The first block of a group that files may use: past its inodes, and in
// group 0 past the summary block too.
static uint32_t cg_first_data(const struct geom *g, uint32_t c) {
  return g->dblkno + (c == 0 ? 1 : 0);
}

static uint64_t data_capacity(const struct geom *g) {
  uint64_t n = 0;
  uint32_t c;

  for (c = 0; c < g->ncg; c++) {
    uint32_t blocks = cg_blocks(g, c);
    if (blocks > cg_first_data(g, c)) {
      n += blocks - cg_first_data(g, c);
    }
  }
  return n;
}

// The block holding allocation-order index seq. Groups are filled in turn,
// each from its first data block.
struct seq_map {
  const struct geom *g;
  uint32_t cg;
  uint64_t start;  // seq of cg's first data block
};

static uint64_t seq_block(struct seq_map *m, uint64_t seq) {
  const struct geom *g = m->g;

  if (seq < m->start) {
    m->cg = 0;
    m->start = 0;
  }
  for (;;) {
    uint32_t first = cg_first_data(g, m->cg);
    uint32_t blocks = cg_blocks(g, m->cg);
    uint64_t room = blocks > first ? blocks - first : 0;
    if (seq < m->start + room) {
      return cg_base(g, m->cg) + first + (seq - m->start);
    }
    m->start += room;
    m->cg++;
  }
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

static int scan(struct plan *p, int dir) {
  DIR *d = opendir(p->nodes[dir].path);
  struct dirent *e;
  int last = -1;

  if (!d) {
    RIP_ERROR("ufs2: can't read %s", p->nodes[dir].path);
    return RIP_ERR;
  }
  while ((e = readdir(d)) != NULL) {
    struct stat st;
    int i;
    struct node *n;
    size_t len = strlen(e->d_name);
    if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) {
      continue;
    }
    i = add_node(p);
    if (i < 0) {
      closedir(d);
      return RIP_ENOMEM;
    }
    n = &p->nodes[i];
    if (len > 255 ||
        snprintf(n->path, sizeof(n->path), "%s/%s", p->nodes[dir].path, e->d_name) >= (int)sizeof(n->path) ||
        stat(n->path, &st) != 0 || (!S_ISDIR(st.st_mode) && !S_ISREG(st.st_mode))) {
      RIP_ERROR("ufs2: can't take %s/%s", p->nodes[dir].path, e->d_name);
      closedir(d);
      return RIP_ERR;
    }
    memcpy(n->name, e->d_name, len + 1);
    n->name_len = (unsigned)len;
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
    if (n->is_dir) {
      p->nodes[dir].subdirs++;
    } else {
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

static unsigned direct_size(unsigned name_len) {
  return (8 + name_len + 1 + 3) & ~3u;
}

// Bytes of a directory's entries, packed into 512-byte chunks that no entry
// crosses.
static uint64_t dir_bytes(const struct plan *p, int dir) {
  uint64_t chunks = 1;
  unsigned at = direct_size(1) + direct_size(2);  // "." and ".."
  int c;

  for (c = p->nodes[dir].first_child; c >= 0; c = p->nodes[c].next) {
    unsigned need = direct_size(p->nodes[c].name_len);
    if (at + need > UFS_DIRBLKSIZ) {
      chunks++;
      at = 0;
    }
    at += need;
  }
  return chunks * UFS_DIRBLKSIZ;
}

static int block_count(uint64_t blocks, uint64_t *count) {
  uint64_t n = blocks;

  if (blocks > DOUBLE_MAX) {
    return RIP_ERR;  // past 4 TiB
  }
  if (blocks > DIRECT_MAX) {
    n++;
  }
  if (blocks > SINGLE_MAX) {
    n += 1 + howmany(blocks - SINGLE_MAX, NINDIR);
  }
  *count = n;
  return RIP_OK;
}

// Numbers the inodes and hands out blocks: a directory, then its files, then
// its subdirectories the same way.
static int allocate(struct plan *p, int dir) {
  struct node *d = &p->nodes[dir];
  int c;

  d->size = dir_bytes(p, dir);
  d->blocks = howmany(d->size, BSIZE);
  if (block_count(d->blocks, &d->count) != RIP_OK) {
    return RIP_ERR;
  }
  d->seq = p->used;
  p->used += d->count;
  for (c = d->first_child; c >= 0; c = p->nodes[c].next) {
    struct node *n = &p->nodes[c];
    if (!n->is_dir) {
      n->blocks = howmany(n->size, BSIZE);
      if (block_count(n->blocks, &n->count) != RIP_OK) {
        RIP_ERROR("ufs2: %s is too large", n->path);
        return RIP_ERR;
      }
      n->seq = p->used;
      p->used += n->count;
    }
  }
  for (c = d->first_child; c >= 0; c = p->nodes[c].next) {
    if (p->nodes[c].is_dir && allocate(p, c) != RIP_OK) {
      return RIP_ERR;
    }
  }
  return RIP_OK;
}

// Where a node's k-th data block falls in its allocation order.
static uint64_t data_seq(uint64_t k) {
  if (k < DIRECT_MAX) {
    return k;
  }
  if (k < SINGLE_MAX) {
    return SEQ_IND1 + 1 + (k - DIRECT_MAX);
  }
  k -= SINGLE_MAX;
  return SEQ_DIND + 1 + (k / NINDIR) * (NINDIR + 1) + 1 + k % NINDIR;
}

/* Writing */

struct out {
  int fd;
  struct geom g;
  struct seq_map map;
  ufs2_progress_fn progress;
  void *ud;
  uint64_t done;
  uint64_t total;
  uint8_t *buf;  // BATCH blocks
  uint8_t *ind;  // an indirect block
};

#define BATCH 64u  // blocks read and written at once: 4 MiB

static uint64_t node_block(struct out *o, const struct node *n, uint64_t seq_in_node) {
  return seq_block(&o->map, n->seq + seq_in_node);
}

static int write_blocks(struct out *o, uint64_t block, const void *data, size_t len) {
  return rip_seek_write(o->fd, block << BSHIFT, data, len) == 0 ? RIP_OK : RIP_ERR;
}

// Writes a node's indirect blocks and fills in its inode's block pointers.
static int map_node(struct out *o, const struct node *n, struct ufs2_dinode *di) {
  uint64_t k;
  uint64_t *ptr = (uint64_t *)o->ind;

  for (k = 0; k < n->blocks && k < DIRECT_MAX; k++) {
    di->di_db[k] = (int64_t)node_block(o, n, k);
  }
  if (n->blocks > DIRECT_MAX) {
    memset(o->ind, 0, BSIZE);
    for (k = DIRECT_MAX; k < n->blocks && k < SINGLE_MAX; k++) {
      ptr[k - DIRECT_MAX] = node_block(o, n, data_seq(k));
    }
    di->di_ib[0] = (int64_t)node_block(o, n, SEQ_IND1);
    if (write_blocks(o, (uint64_t)di->di_ib[0], o->ind, BSIZE) != RIP_OK) {
      return RIP_ERR;
    }
  }
  if (n->blocks > SINGLE_MAX) {
    uint64_t groups = howmany(n->blocks - SINGLE_MAX, NINDIR);
    uint64_t gi;
    int rc;
    uint64_t *dind = calloc(NINDIR, sizeof(uint64_t));
    if (!dind) {
      return RIP_ENOMEM;
    }
    for (gi = 0; gi < groups; gi++) {
      uint64_t first = SINGLE_MAX + gi * NINDIR;
      uint64_t ind_seq = SEQ_DIND + 1 + gi * (NINDIR + 1);
      memset(o->ind, 0, BSIZE);
      for (k = first; k < n->blocks && k < first + NINDIR; k++) {
        ptr[k - first] = node_block(o, n, data_seq(k));
      }
      dind[gi] = node_block(o, n, ind_seq);
      if (write_blocks(o, dind[gi], o->ind, BSIZE) != RIP_OK) {
        free(dind);
        return RIP_ERR;
      }
    }
    di->di_ib[1] = (int64_t)node_block(o, n, SEQ_DIND);
    rc = write_blocks(o, (uint64_t)di->di_ib[1], dind, BSIZE);
    free(dind);
    if (rc != RIP_OK) {
      return RIP_ERR;
    }
  }
  return RIP_OK;
}

// Writes a node's data blocks from a file (fd) or memory (mem), joining
// blocks that sit next to each other into one write.
static int write_data(struct out *o, const struct node *n, int fd, const uint8_t *mem) {
  uint64_t k = 0;

  while (k < n->blocks) {
    uint64_t want = n->blocks - k < BATCH ? n->blocks - k : BATCH;
    uint64_t bytes = n->size - (k << BSHIFT) < (want << BSHIFT) ? n->size - (k << BSHIFT) : want << BSHIFT;
    uint64_t i = 0;
    if (mem) {
      memcpy(o->buf, mem + (k << BSHIFT), (size_t)bytes);
    } else if (rip_read_full(fd, o->buf, (size_t)bytes) != (long)bytes) {
      RIP_ERROR("ufs2: can't read %s", n->path);
      return RIP_ERR;
    }
    if (bytes & (BSIZE - 1)) {
      memset(o->buf + bytes, 0, BSIZE - (bytes & (BSIZE - 1)));
    }
    while (i < want) {
      uint64_t start = node_block(o, n, data_seq(k + i));
      uint64_t run = 1;
      while (i + run < want && node_block(o, n, data_seq(k + i + run)) == start + run) {
        run++;
      }
      if (write_blocks(o, start, o->buf + (i << BSHIFT), (size_t)(run << BSHIFT)) != RIP_OK) {
        RIP_ERROR("ufs2: write failed");
        return RIP_ERR;
      }
      i += run;
    }
    k += want;
    if (!mem) {
      o->done += bytes;
      if (o->progress && o->progress(o->ud, o->done, o->total)) {
        return RIP_ECANCEL;
      }
    }
  }
  return RIP_OK;
}

static unsigned put_direct(uint8_t *at, uint32_t ino, uint8_t type, const char *name, unsigned len, unsigned reclen) {
  rip_put_le32(at, ino);
  rip_put_le16(at + 4, (uint16_t)reclen);
  at[6] = type;
  at[7] = (uint8_t)len;
  memcpy(at + 8, name, len);
  return direct_size(len);
}

// A directory's entries, each chunk's last entry reaching to its end.
static uint8_t *dir_contents(const struct plan *p, int dir) {
  const struct node *d = &p->nodes[dir];
  uint8_t *buf = calloc(1, (size_t)roundup(d->size, BSIZE));
  unsigned chunk = 0;
  unsigned at = 0;
  unsigned last = 0;
  int c;

  if (!buf) {
    return NULL;
  }
  last = 0;
  at += put_direct(buf, d->ino, DT_DIR_, ".", 1, direct_size(1));
  last = at;
  at += put_direct(buf + at, d->parent < 0 ? d->ino : p->nodes[d->parent].ino, DT_DIR_, "..", 2, direct_size(2));
  for (c = d->first_child; c >= 0; c = p->nodes[c].next) {
    const struct node *n = &p->nodes[c];
    unsigned need = direct_size(n->name_len);
    if (at + need > UFS_DIRBLKSIZ) {
      // Stretch the chunk's last entry to its end and start the next chunk.
      rip_put_le16(buf + chunk + last + 4, (uint16_t)(UFS_DIRBLKSIZ - last));
      chunk += UFS_DIRBLKSIZ;
      at = 0;
    }
    last = at;
    at += put_direct(buf + chunk + at, n->ino, n->is_dir ? DT_DIR_ : DT_REG_, n->name, n->name_len, need);
  }
  rip_put_le16(buf + chunk + last + 4, (uint16_t)(UFS_DIRBLKSIZ - last));
  return buf;
}

static uint32_t next_gen(uint32_t *state) {
  uint32_t x = *state;
  x ^= x << 13;
  x ^= x >> 17;
  x ^= x << 5;
  *state = x;
  return x ? x : 1;
}

static void fill_inode(struct ufs2_dinode *di, const struct node *n, uint32_t gen) {
  memset(di, 0, sizeof(*di));
  di->di_mode = n->is_dir ? MODE_DIR : MODE_FILE;
  di->di_nlink = (uint16_t)(n->is_dir ? 2 + n->subdirs : 1);
  di->di_blksize = BSIZE;
  di->di_size = n->size;
  di->di_blocks = n->count * (BSIZE / 512);
  di->di_atime = di->di_mtime = di->di_ctime = di->di_birthtime = (int64_t)n->mtime;
  di->di_gen = gen;
}

static void set_bit(uint8_t *map, uint64_t i) {
  map[i / 8] |= (uint8_t)(1u << (i % 8));
}

static int test_bit(const uint8_t *map, uint64_t i) {
  return (map[i / 8] >> (i % 8)) & 1;
}

// A cylinder group's map: its inodes in use, its free blocks and runs of them.
static void build_cg(const struct geom *g, uint32_t c, uint64_t used_blocks, uint32_t inodes_used, uint32_t dirs,
                     time_t now, uint8_t *buf, struct ufs_csum *cs) {
  struct ufs_cg *cg = (struct ufs_cg *)buf;
  uint32_t ndblk = cg_blocks(g, c);
  uint32_t first = cg_first_data(g, c);
  uint8_t *iused;
  uint8_t *blksfree;
  uint8_t *clustersfree;
  int32_t *clustersum;
  uint32_t i;
  uint32_t run = 0;

  memset(buf, 0, BSIZE);
  cg->cg_magic = UFS_CG_MAGIC;
  cg->cg_old_time = (int32_t)now;
  cg->cg_time = now;
  cg->cg_cgx = c;
  cg->cg_ndblk = ndblk;
  cg->cg_niblk = g->ipg;
  cg->cg_initediblk = (uint32_t)roundup(inodes_used, INOPB);
  if (cg->cg_initediblk < 2 * INOPB) {
    cg->cg_initediblk = 2 * INOPB;
  }
  if (cg->cg_initediblk > g->ipg) {
    cg->cg_initediblk = g->ipg;
  }
  cg->cg_nclusterblks = ndblk;
  cg->cg_iusedoff = sizeof(struct ufs_cg);
  cg->cg_freeoff = cg->cg_iusedoff + (uint32_t)howmany(g->ipg, 8);
  cg->cg_clustersumoff = (uint32_t)roundup(cg->cg_freeoff + howmany(g->fpg, 8) - 4, 4);
  cg->cg_clusteroff = cg->cg_clustersumoff + (MAXCONTIG + 1) * 4;
  cg->cg_nextfreeoff = cg->cg_clusteroff + (uint32_t)howmany(g->fpg, 8);
  iused = buf + cg->cg_iusedoff;
  blksfree = buf + cg->cg_freeoff;
  clustersfree = buf + cg->cg_clusteroff;
  clustersum = (int32_t *)(buf + cg->cg_clustersumoff);

  for (i = 0; i < inodes_used; i++) {
    set_bit(iused, i);
  }
  cg->cg_cs.cs_nifree = (int32_t)(g->ipg - inodes_used);
  cg->cg_cs.cs_ndir = (int32_t)dirs;
  // Free: in groups past the first, the blocks ahead of the superblock copy;
  // in each, what follows the inodes and the blocks given out.
  for (i = 0; i < ndblk; i++) {
    int is_free = (c > 0 && i < SBLKNO) || (i >= first + used_blocks);
    if (is_free) {
      set_bit(blksfree, i);
      set_bit(clustersfree, i);
      cg->cg_cs.cs_nbfree++;
    }
  }
  for (i = 0; i < ndblk; i++) {
    if (test_bit(clustersfree, i)) {
      run++;
    } else if (run) {
      clustersum[run > MAXCONTIG ? MAXCONTIG : run]++;
      run = 0;
    }
  }
  if (run) {
    clustersum[run > MAXCONTIG ? MAXCONTIG : run]++;
  }
  cs->cs_ndir = cg->cg_cs.cs_ndir;
  cs->cs_nbfree = cg->cg_cs.cs_nbfree;
  cs->cs_nifree = cg->cg_cs.cs_nifree;
  cs->cs_nffree = 0;
}

static void build_superblock(struct ufs_fs *fs, const struct geom *g, const struct ufs_csum_total *tot,
                             const char *label, time_t now, uint32_t id) {
  uint64_t sizepb = BSIZE;
  int i;

  memset(fs, 0, sizeof(*fs));
  fs->fs_sblkno = SBLKNO;
  fs->fs_cblkno = CBLKNO;
  fs->fs_iblkno = IBLKNO;
  fs->fs_dblkno = (int32_t)g->dblkno;
  fs->fs_old_time = (int32_t)now;
  fs->fs_old_size = (int32_t)g->size;
  fs->fs_ncg = g->ncg;
  fs->fs_bsize = BSIZE;
  fs->fs_fsize = BSIZE;
  fs->fs_frag = 1;
  fs->fs_minfree = 0;
  fs->fs_bmask = ~(int32_t)(BSIZE - 1);
  fs->fs_fmask = ~(int32_t)(BSIZE - 1);
  fs->fs_bshift = BSHIFT;
  fs->fs_fshift = BSHIFT;
  fs->fs_maxcontig = MAXCONTIG;
  fs->fs_maxbpg = NINDIR;
  fs->fs_fragshift = 0;
  fs->fs_fsbtodb = BSHIFT - 9;
  fs->fs_sbsize = UFS_SBLOCKSIZE;
  fs->fs_nindir = NINDIR;
  fs->fs_inopb = INOPB;
  fs->fs_old_nspf = BSIZE / 512;
  fs->fs_optim = 1;  // space: minfree is 0
  fs->fs_id[0] = (int32_t)now;
  fs->fs_id[1] = (int32_t)id;
  fs->fs_old_csaddr = (int32_t)g->dblkno;
  fs->fs_cssize = BSIZE;  // ncg * 16 bytes, rounded to a block
  fs->fs_cgsize = (int32_t)g->cgsize;
  fs->fs_old_ncyl = (int32_t)g->ncg;
  fs->fs_old_cpg = 1;
  fs->fs_ipg = g->ipg;
  fs->fs_fpg = (int32_t)g->fpg;
  fs->fs_old_cstotal.cs_ndir = (int32_t)tot->cs_ndir;
  fs->fs_old_cstotal.cs_nbfree = (int32_t)tot->cs_nbfree;
  fs->fs_old_cstotal.cs_nifree = (int32_t)tot->cs_nifree;
  fs->fs_clean = UFS_FS_ISCLEAN;
  fs->fs_old_flags = (int8_t)UFS_FS_FLAGS_UPDATED;
  if (label) {
    snprintf((char *)fs->fs_volname, sizeof(fs->fs_volname), "%s", label);
  }
  fs->fs_maxbsize = BSIZE;
  fs->fs_providersize = (int64_t)g->size;
  fs->fs_sblockactualloc = UFS_SBLOCK;
  fs->fs_sblockloc = UFS_SBLOCK;
  fs->fs_cstotal = *tot;
  fs->fs_time = now;
  fs->fs_mtime = now;
  fs->fs_size = (int64_t)g->size;
  fs->fs_dsize = (int64_t)(g->size - SBLKNO - (uint64_t)g->ncg * (g->dblkno - SBLKNO) - 1);
  fs->fs_old_dsize = (int32_t)fs->fs_dsize;
  fs->fs_csaddr = g->dblkno;
  fs->fs_avgfilesize = 16384;
  fs->fs_avgfpdir = 64;
  fs->fs_contigsumsize = MAXCONTIG;
  fs->fs_maxsymlinklen = (UFS_NDADDR + UFS_NIADDR) * 8;
  fs->fs_old_inodefmt = UFS_FS_44INODEFMT;
  fs->fs_maxfilesize = (uint64_t)BSIZE * UFS_NDADDR - 1;
  for (i = 0; i < UFS_NIADDR; i++) {
    sizepb *= NINDIR;
    fs->fs_maxfilesize += sizepb;
  }
  fs->fs_qbmask = BSIZE - 1;
  fs->fs_qfmask = BSIZE - 1;
  fs->fs_old_postblformat = -1;
  fs->fs_old_nrpos = 1;
  fs->fs_magic = UFS2_MAGIC;
}

// Sizes the image to what it holds: enough blocks for the data and the
// groups' own metadata, enough inodes for every entry.
static int size_image(struct geom *g, const struct plan *p) {
  uint64_t need = p->used;
  uint64_t inodes = (uint64_t)p->count + UFS_ROOTINO;
  uint64_t fpi = DENSITY_FRAGS;
  int round;

  g->size = need + IBLKNO + 2;
  for (round = 0; round < 64; round++) {
    uint64_t cap;
    if (g->size < MIN_BLOCKS) {
      g->size = MIN_BLOCKS;
    }
    if (geometry(g, fpi) != RIP_OK) {
      return RIP_ERR;
    }
    if ((uint64_t)g->ncg * g->ipg < inodes) {
      if (fpi > 1) {
        fpi--;
      } else {
        g->size += howmany(inodes - (uint64_t)g->ncg * g->ipg, INOPB) * INOPB;
      }
      continue;
    }
    if ((uint64_t)g->ncg * 16 > BSIZE) {
      RIP_ERROR("ufs2: too many cylinder groups");
      return RIP_ERR;
    }
    cap = data_capacity(g);
    if (cap >= need) {
      return RIP_OK;
    }
    g->size += need - cap + (uint64_t)g->dblkno + 1;
  }
  RIP_ERROR("ufs2: couldn't size the image");
  return RIP_ERR;
}

static int write_tree(struct out *o, struct plan *p, int dir) {
  int c;
  int rc = RIP_OK;

  for (c = p->nodes[dir].first_child; rc == RIP_OK && c >= 0; c = p->nodes[c].next) {
    struct node *n = &p->nodes[c];
    int fd;
    if (n->is_dir || !n->blocks) {
      continue;
    }
    fd = open(n->path, O_RDONLY);
    if (fd < 0) {
      RIP_ERROR("ufs2: can't open %s", n->path);
      return RIP_ERR;
    }
    rc = write_data(o, n, fd, NULL);
    close(fd);
  }
  for (c = p->nodes[dir].first_child; rc == RIP_OK && c >= 0; c = p->nodes[c].next) {
    if (p->nodes[c].is_dir) {
      rc = write_tree(o, p, c);
    }
  }
  return rc;
}

int ufs2_build(const char *src_dir, const char *image_path, const char *label, ufs2_progress_fn progress, void *ud) {
  struct plan p;
  struct out o;
  struct ufs_csum_total tot;
  struct ufs_csum *csums = NULL;
  struct ufs_fs fs;
  uint8_t *cgbuf = NULL;
  uint8_t *sbbuf = NULL;
  uint8_t *itable = NULL;
  uint32_t *cg_inodes = NULL;
  uint32_t *cg_dirs = NULL;
  time_t now = time(NULL);
  uint32_t gen = (uint32_t)now ^ 0x5bd1e995u;
  uint64_t remaining;
  uint32_t c;
  int rc = RIP_ERR;
  int k;

  memset(&p, 0, sizeof(p));
  memset(&o, 0, sizeof(o));
  memset(&tot, 0, sizeof(tot));
  o.fd = -1;
  if (add_node(&p) < 0) {
    return RIP_ENOMEM;
  }
  snprintf(p.nodes[0].path, sizeof(p.nodes[0].path), "%s", src_dir);
  p.nodes[0].is_dir = 1;
  p.nodes[0].parent = -1;
  p.nodes[0].mtime = now;
  if (scan(&p, 0) != RIP_OK) {
    goto done;
  }
  for (k = 0; k < p.count; k++) {
    p.nodes[k].ino = UFS_ROOTINO + (uint32_t)k;
  }
  if (allocate(&p, 0) != RIP_OK || size_image(&o.g, &p) != RIP_OK) {
    goto done;
  }
  o.map.g = &o.g;

  csums = calloc(o.g.ncg, sizeof(*csums));
  cg_inodes = calloc(o.g.ncg, sizeof(*cg_inodes));
  cg_dirs = calloc(o.g.ncg, sizeof(*cg_dirs));
  cgbuf = malloc(BSIZE);
  sbbuf = calloc(1, UFS_SBLOCKSIZE);
  o.buf = malloc((size_t)BATCH * BSIZE);
  o.ind = malloc(BSIZE);
  if (!csums || !cg_inodes || !cg_dirs || !cgbuf || !sbbuf || !o.buf || !o.ind) {
    rc = RIP_ENOMEM;
    goto done;
  }
  // Inodes 0 and 1 are reserved (marked used), the root is 2, the rest
  // follow in order.
  for (k = 0; k < p.count + UFS_ROOTINO; k++) {
    c = (uint32_t)k / o.g.ipg;
    cg_inodes[c] = (uint32_t)k % o.g.ipg + 1;
    if (k >= UFS_ROOTINO && p.nodes[k - UFS_ROOTINO].is_dir) {
      cg_dirs[c]++;
    }
  }

  o.fd = open(image_path, O_RDWR | O_CREAT | O_TRUNC, 0644);
  o.progress = progress;
  o.ud = ud;
  o.total = p.data;
  if (o.fd < 0 || ftruncate(o.fd, (off_t)(o.g.size << BSHIFT)) != 0) {
    RIP_ERROR("ufs2: can't create %s", image_path);
    goto done;
  }

  // Inode tables, group by group, with the directories and indirect blocks
  // written as their inodes are filled in.
  itable = malloc((size_t)o.g.ipg * sizeof(struct ufs2_dinode));
  if (!itable) {
    rc = RIP_ENOMEM;
    goto done;
  }
  for (c = 0; c < o.g.ncg; c++) {
    uint32_t inited = (uint32_t)roundup(cg_inodes[c], INOPB);
    uint32_t i;
    if (inited < 2 * INOPB) {
      inited = 2 * INOPB;
    }
    if (inited > o.g.ipg) {
      inited = o.g.ipg;
    }
    memset(itable, 0, (size_t)inited * sizeof(struct ufs2_dinode));
    for (i = 0; i < inited; i++) {
      uint64_t ino = (uint64_t)c * o.g.ipg + i;
      struct ufs2_dinode *di = (struct ufs2_dinode *)itable + i;
      if (ino >= UFS_ROOTINO && ino < (uint64_t)p.count + UFS_ROOTINO) {
        struct node *n = &p.nodes[ino - UFS_ROOTINO];
        fill_inode(di, n, next_gen(&gen));
        rc = map_node(&o, n, di);
        if (rc == RIP_OK && n->is_dir) {
          uint8_t *contents = dir_contents(&p, (int)(ino - UFS_ROOTINO));
          rc = contents ? write_data(&o, n, -1, contents) : RIP_ENOMEM;
          free(contents);
        }
        if (rc != RIP_OK) {
          goto done;
        }
      } else {
        di->di_gen = next_gen(&gen);
      }
    }
    rc = write_blocks(&o, cg_base(&o.g, c) + IBLKNO, itable, (size_t)inited * sizeof(struct ufs2_dinode));
    if (rc != RIP_OK) {
      goto done;
    }
  }

  // Cylinder groups: blocks are given out from group 0 on.
  remaining = p.used;
  for (c = 0; c < o.g.ncg; c++) {
    uint32_t blocks = cg_blocks(&o.g, c);
    uint32_t first = cg_first_data(&o.g, c);
    uint64_t room = blocks > first ? blocks - first : 0;
    uint64_t used = remaining < room ? remaining : room;
    remaining -= used;
    build_cg(&o.g, c, used, cg_inodes[c], cg_dirs[c], now, cgbuf, &csums[c]);
    tot.cs_ndir += csums[c].cs_ndir;
    tot.cs_nbfree += csums[c].cs_nbfree;
    tot.cs_nifree += csums[c].cs_nifree;
    if (write_blocks(&o, cg_base(&o.g, c) + CBLKNO, cgbuf, BSIZE) != RIP_OK) {
      rc = RIP_ERR;
      goto done;
    }
  }

  // The group summaries, then the superblock and its copy in every group.
  memset(cgbuf, 0, BSIZE);
  memcpy(cgbuf, csums, (size_t)o.g.ncg * sizeof(*csums));
  build_superblock(&fs, &o.g, &tot, label, now, gen);
  memcpy(sbbuf, &fs, sizeof(fs));
  if (write_blocks(&o, o.g.dblkno, cgbuf, BSIZE) != RIP_OK ||
      rip_seek_write(o.fd, UFS_SBLOCK, sbbuf, UFS_SBLOCKSIZE) != 0) {
    rc = RIP_ERR;
    goto done;
  }
  for (c = 0; c < o.g.ncg; c++) {
    if (write_blocks(&o, cg_base(&o.g, c) + SBLKNO, sbbuf, UFS_SBLOCKSIZE) != RIP_OK) {
      rc = RIP_ERR;
      goto done;
    }
  }

  rc = write_tree(&o, &p, 0);
  if (rc == RIP_OK && fsync(o.fd) != 0) {
    rc = RIP_ERR;
  }
  if (rc == RIP_OK) {
    RIP_INFO("ufs2: %s, %d entries, %llu MiB in %u groups of %u blocks, %u inodes each", image_path, p.count - 1,
             (unsigned long long)(o.g.size >> (20 - BSHIFT)), o.g.ncg, o.g.fpg, o.g.ipg);
  }

done:
  if (o.fd >= 0) {
    close(o.fd);
    if (rc != RIP_OK) {
      unlink(image_path);
    }
  }
  free(itable);
  free(csums);
  free(cg_inodes);
  free(cg_dirs);
  free(cgbuf);
  free(sbbuf);
  free(o.buf);
  free(o.ind);
  free(p.nodes);
  return rc;
}
