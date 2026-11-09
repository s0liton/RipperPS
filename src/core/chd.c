#include "ripper/chd.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <zlib.h>
#include <zstd.h>

#include "ripper/common.h"
#include "ripper/hash.h"
#include "ripper/log.h"
#include "ripper/util.h"

#define HEADER_BYTES 124
#define CD_UNIT (CD_FRAME + CD_SUB)
#define CD_FRAMES_PER_HUNK 8
#define CD_TRACK_PADDING 4
#define DVD_HUNK 4096

// Map entry types (chd.h in MAME). Only these two are written: a hunk is
// either compressed with the one codec or stored as it is.
#define COMP_TYPE_0 0
#define COMP_NONE 4

#define ZLIB_LEVEL 6
#define ZSTD_LEVEL 7

struct chd_writer {
  int fd;
  char path[1024];
  int cd;
  enum chd_codec codec;
  int has_sub;
  uint32_t hunk_bytes;
  uint32_t unit_bytes;
  uint64_t logical_bytes;
  uint32_t hunk_count;
  uint64_t written;  // logical bytes so far
  uint8_t *hunk;
  uint32_t fill;
  uint8_t *scratch;  // CD: data then subchannel of a hunk, before compression
  uint8_t *comp;
  size_t comp_cap;
  uint8_t *map;  // hunk_count raw 12-byte entries
  uint32_t hunks_done;
  uint64_t first_offset;
  uint64_t offset;
  uint32_t max_complen;
  struct rip_sha1 sha;
  uint8_t meta_hash[CD_MAX_TRACKS][24];  // tag + SHA-1 of each metadata entry
  unsigned meta_count;
  z_stream z;
  int z_ready;
  ZSTD_CCtx *zstd;
  // CD
  struct chd_cd_track tracks[CD_MAX_TRACKS];
  unsigned track_count;
  unsigned track;
  uint32_t track_written;
};

static void put_be48(uint8_t *p, uint64_t v) {
  p[0] = (uint8_t)(v >> 40);
  p[1] = (uint8_t)(v >> 32);
  rip_put_be32(p + 2, (uint32_t)v);
}

static void put_be64(uint8_t *p, uint64_t v) {
  rip_put_be32(p, (uint32_t)(v >> 32));
  rip_put_be32(p + 4, (uint32_t)v);
}

static void put_be24(uint8_t *p, uint32_t v) {
  p[0] = (uint8_t)(v >> 16);
  p[1] = (uint8_t)(v >> 8);
  p[2] = (uint8_t)v;
}

// CRC-16/CCITT-FALSE, what libchdr checks each hunk and the map against.
static uint16_t crc16(const uint8_t *p, size_t n) {
  uint16_t crc = 0xFFFF;
  int b;

  while (n--) {
    crc ^= (uint16_t)(*p++ << 8);
    for (b = 0; b < 8; b++) {
      crc = (crc & 0x8000) ? (uint16_t)((crc << 1) ^ 0x1021) : (uint16_t)(crc << 1);
    }
  }
  return crc;
}

static uint32_t codec_tag(const struct chd_writer *w) {
  if (w->cd) {
    return w->codec == CHD_ZSTD ? 0x63647A73u /* cdzs */ : 0x63647A6Cu /* cdzl */;
  }
  return w->codec == CHD_ZSTD ? 0x7A737464u /* zstd */ : 0x7A6C6962u /* zlib */;
}

static int write_at(struct chd_writer *w, uint64_t off, const void *p, size_t n) {
  if (rip_seek_write(w->fd, off, p, n) != 0) {
    RIP_ERROR("writing %s failed: %s", w->path, strerror(errno));
    return RIP_ERR;
  }
  return RIP_OK;
}

static void write_header(struct chd_writer *w, uint8_t *h, uint64_t map_offset, const uint8_t raw_sha1[20],
                         const uint8_t sha1[20]) {
  memset(h, 0, HEADER_BYTES);
  memcpy(h, "MComprHD", 8);
  rip_put_be32(h + 8, HEADER_BYTES);
  rip_put_be32(h + 12, 5);
  rip_put_be32(h + 16, codec_tag(w));
  put_be64(h + 32, w->logical_bytes);
  put_be64(h + 40, map_offset);
  put_be64(h + 48, w->meta_count ? HEADER_BYTES : 0);
  rip_put_be32(h + 56, w->hunk_bytes);
  rip_put_be32(h + 60, w->unit_bytes);
  if (raw_sha1) {
    memcpy(h + 64, raw_sha1, 20);
  }
  if (sha1) {
    memcpy(h + 84, sha1, 20);
  }
}

// Metadata entries, chained one after the other from the header on.
static int write_metadata(struct chd_writer *w, const char *tag, const char *const *texts, size_t *lens,
                          unsigned count) {
  uint64_t off = HEADER_BYTES;
  unsigned i;

  for (i = 0; i < count; i++) {
    uint8_t head[16];
    uint64_t next = i + 1 < count ? off + 16 + lens[i] : 0;
    struct rip_sha1 s;

    memcpy(head, tag, 4);
    head[4] = 0x01;  // CHD_MDFLAGS_CHECKSUM: part of the overall SHA-1
    put_be24(head + 5, (uint32_t)lens[i]);
    put_be64(head + 8, next);
    if (write_at(w, off, head, 16) != RIP_OK || write_at(w, off + 16, texts[i], lens[i]) != RIP_OK) {
      return RIP_ERR;
    }
    memcpy(w->meta_hash[w->meta_count], tag, 4);
    rip_sha1_init(&s);
    rip_sha1_update(&s, texts[i], lens[i]);
    rip_sha1_final(&s, w->meta_hash[w->meta_count] + 4);
    w->meta_count++;
    off += 16 + lens[i];
  }
  w->first_offset = off;
  w->offset = off;
  return RIP_OK;
}

static int setup(struct chd_writer *w, const char *path) {
  uint8_t h[HEADER_BYTES];

  w->hunk_count = (uint32_t)((w->logical_bytes + w->hunk_bytes - 1) / w->hunk_bytes);
  w->hunk = calloc(1, w->hunk_bytes);
  w->scratch = calloc(1, w->hunk_bytes);
  w->comp_cap = w->hunk_bytes + w->hunk_bytes / 2 + 1024;
  if (ZSTD_compressBound(w->hunk_bytes) + 64 > w->comp_cap) {
    w->comp_cap = ZSTD_compressBound(w->hunk_bytes) + 64;
  }
  w->comp = malloc(w->comp_cap);
  w->map = calloc(w->hunk_count ? w->hunk_count : 1, 12);
  if (!w->hunk || !w->scratch || !w->comp || !w->map) {
    return RIP_ENOMEM;
  }
  if (w->codec == CHD_ZSTD) {
    w->zstd = ZSTD_createCCtx();
    if (!w->zstd) {
      return RIP_ENOMEM;
    }
    ZSTD_CCtx_setParameter(w->zstd, ZSTD_c_compressionLevel, ZSTD_LEVEL);
  } else {
    if (deflateInit2(&w->z, ZLIB_LEVEL, Z_DEFLATED, -MAX_WBITS, 8, Z_DEFAULT_STRATEGY) != Z_OK) {
      return RIP_ENOMEM;
    }
    w->z_ready = 1;
  }
  snprintf(w->path, sizeof(w->path), "%s", path);
  w->fd = open(path, O_RDWR | O_CREAT | O_TRUNC, 0644);
  if (w->fd < 0) {
    RIP_ERROR("cannot create %s: %s", path, strerror(errno));
    return RIP_ERR;
  }
  write_header(w, h, 0, NULL, NULL);
  rip_sha1_init(&w->sha);
  return write_at(w, 0, h, sizeof(h));
}

static void destroy(struct chd_writer *w) {
  if (!w) {
    return;
  }
  if (w->fd >= 0) {
    close(w->fd);
  }
  if (w->z_ready) {
    deflateEnd(&w->z);
  }
  ZSTD_freeCCtx(w->zstd);
  free(w->hunk);
  free(w->scratch);
  free(w->comp);
  free(w->map);
  free(w);
}

/* Compression */

// One buffer into dst; returns its compressed size, or 0 if it did not shrink.
static size_t squeeze(struct chd_writer *w, const uint8_t *src, size_t len, uint8_t *dst, size_t cap) {
  if (w->codec == CHD_ZSTD) {
    size_t n = ZSTD_compress2(w->zstd, dst, cap, src, len);
    return ZSTD_isError(n) ? 0 : n;
  }
  deflateReset(&w->z);
  w->z.next_in = (Bytef *)src;
  w->z.avail_in = (uInt)len;
  w->z.next_out = dst;
  w->z.avail_out = (uInt)cap;
  if (deflate(&w->z, Z_FINISH) != Z_STREAM_END) {
    return 0;
  }
  return cap - w->z.avail_out;
}

// The CD codecs' layout: a bitmap of frames whose sync and ECC were dropped
// (libchdr makes them again), the compressed length of the frame data, then
// the frame data and the subchannel, each compressed on its own.
static size_t squeeze_cd(struct chd_writer *w) {
  uint32_t frames = w->hunk_bytes / CD_UNIT;
  size_t ecc_bytes = (frames + 7) / 8;
  size_t len_bytes = w->hunk_bytes < 65536 ? 2 : 3;
  size_t header = ecc_bytes + len_bytes;
  uint8_t *data = w->scratch;
  uint8_t *sub = w->scratch + (size_t)frames * CD_FRAME;
  size_t base;
  size_t subc;
  uint32_t f;

  memset(w->comp, 0, header);
  for (f = 0; f < frames; f++) {
    uint8_t *sector = data + (size_t)f * CD_FRAME;
    memcpy(sector, w->hunk + (size_t)f * CD_UNIT, CD_FRAME);
    memcpy(sub + (size_t)f * CD_SUB, w->hunk + (size_t)f * CD_UNIT + CD_FRAME, CD_SUB);
    if (memcmp(sector, cd_sync, sizeof(cd_sync)) == 0 && cd_ecc_verify(sector)) {
      w->comp[f / 8] |= (uint8_t)(1u << (f % 8));
      memset(sector, 0, sizeof(cd_sync));
      memset(sector + 2076, 0, CD_FRAME - 2076);
    }
  }
  if (header >= w->hunk_bytes) {
    return 0;
  }
  base = squeeze(w, data, (size_t)frames * CD_FRAME, w->comp + header, w->comp_cap - header);
  if (!base || base >= (1u << (8 * len_bytes))) {
    return 0;
  }
  subc = squeeze(w, sub, (size_t)frames * CD_SUB, w->comp + header + base, w->comp_cap - header - base);
  if (!subc) {
    return 0;
  }
  if (len_bytes == 2) {
    rip_put_be16(w->comp + ecc_bytes, (uint16_t)base);
  } else {
    put_be24(w->comp + ecc_bytes, (uint32_t)base);
  }
  return header + base + subc;
}

static int flush_hunk(struct chd_writer *w) {
  uint8_t *entry = w->map + (size_t)w->hunks_done * 12;
  uint16_t crc;
  size_t n;

  if (w->hunks_done >= w->hunk_count) {
    return RIP_ERR;
  }
  memset(w->hunk + w->fill, 0, w->hunk_bytes - w->fill);
  crc = crc16(w->hunk, w->hunk_bytes);
  n = w->cd ? squeeze_cd(w) : squeeze(w, w->hunk, w->hunk_bytes, w->comp, w->comp_cap);
  if (n == 0 || n >= w->hunk_bytes) {
    entry[0] = COMP_NONE;
    n = w->hunk_bytes;
    if (write_at(w, w->offset, w->hunk, n) != RIP_OK) {
      return RIP_ERR;
    }
  } else {
    entry[0] = COMP_TYPE_0;
    if (write_at(w, w->offset, w->comp, n) != RIP_OK) {
      return RIP_ERR;
    }
    if (n > w->max_complen) {
      w->max_complen = (uint32_t)n;
    }
  }
  put_be24(entry + 1, (uint32_t)n);
  put_be48(entry + 4, w->offset);
  rip_put_be16(entry + 10, crc);
  w->offset += n;
  w->hunks_done++;
  w->fill = 0;
  return RIP_OK;
}

// Logical bytes in order; full hunks go out as they fill.
static int append(struct chd_writer *w, const uint8_t *p, size_t len) {
  if (w->written + len > w->logical_bytes) {
    RIP_ERROR("CHD: more data than the image holds");
    return RIP_ERR;
  }
  rip_sha1_update(&w->sha, p, len);
  w->written += len;
  while (len) {
    size_t take = w->hunk_bytes - w->fill;
    if (take > len) {
      take = len;
    }
    memcpy(w->hunk + w->fill, p, take);
    w->fill += (uint32_t)take;
    p += take;
    len -= take;
    if (w->fill == w->hunk_bytes && flush_hunk(w) != RIP_OK) {
      return RIP_ERR;
    }
  }
  return RIP_OK;
}

/* Creation */

int chd_create_dvd(struct chd_writer **out, const char *path, uint64_t sectors, enum chd_codec codec) {
  struct chd_writer *w = calloc(1, sizeof(*w));
  const char *text = "";
  size_t len = 1;  // chdman writes an empty string, its NUL included

  *out = NULL;
  if (!w) {
    return RIP_ENOMEM;
  }
  w->fd = -1;
  w->codec = codec;
  w->hunk_bytes = DVD_HUNK;
  w->unit_bytes = CD_DATA;
  w->logical_bytes = sectors * CD_DATA;
  if (setup(w, path) != RIP_OK || write_metadata(w, "DVD ", &text, &len, 1) != RIP_OK) {
    chd_abort(w);
    return RIP_ERR;
  }
  *out = w;
  return RIP_OK;
}

static const char *cht2_type(enum cd_track_mode mode) {
  switch (mode) {
  case CD_MODE_AUDIO:
    return "AUDIO";
  case CD_MODE1:
    return "MODE1_RAW";
  case CD_MODE2:
  case CD_MODE2_FORMLESS:
    return "MODE2_RAW";
  }
  return "MODE1_RAW";
}

int chd_create_cd(struct chd_writer **out, const char *path, const struct chd_cd_track *tracks, unsigned count,
                  enum chd_codec codec, int has_sub) {
  struct chd_writer *w = calloc(1, sizeof(*w));
  char (*texts)[160] = NULL;
  const char *ptrs[CD_MAX_TRACKS];
  size_t lens[CD_MAX_TRACKS];
  uint64_t frames = 0;
  unsigned i;

  *out = NULL;
  if (!w || count == 0 || count > CD_MAX_TRACKS) {
    free(w);
    return RIP_ERR;
  }
  w->fd = -1;
  w->cd = 1;
  w->codec = codec;
  w->has_sub = has_sub;
  w->hunk_bytes = CD_UNIT * CD_FRAMES_PER_HUNK;
  w->unit_bytes = CD_UNIT;
  w->track_count = count;
  memcpy(w->tracks, tracks, count * sizeof(*tracks));
  texts = calloc(count, sizeof(*texts));
  if (!texts) {
    chd_abort(w);
    return RIP_ENOMEM;
  }
  for (i = 0; i < count; i++) {
    const struct chd_cd_track *t = &tracks[i];
    const char *type = cht2_type(t->mode);
    char pgtype[16];

    // "V" before the pregap's type: its frames are in the image.
    snprintf(pgtype, sizeof(pgtype), "%s%s", t->pregap ? "V" : "", type);
    lens[i] = (size_t)snprintf(texts[i], sizeof(texts[i]),
                               "TRACK:%u TYPE:%s SUBTYPE:%s FRAMES:%u PREGAP:%u PGTYPE:%s PGSUB:%s POSTGAP:0", i + 1,
                               type, has_sub ? "RW" : "NONE", t->frames, t->pregap, pgtype, has_sub ? "RW" : "NONE") +
              1;
    ptrs[i] = texts[i];
    frames += (t->frames + CD_TRACK_PADDING - 1) / CD_TRACK_PADDING * CD_TRACK_PADDING;
  }
  w->logical_bytes = frames * CD_UNIT;
  if (setup(w, path) != RIP_OK || write_metadata(w, "CHT2", ptrs, lens, count) != RIP_OK) {
    free(texts);
    chd_abort(w);
    return RIP_ERR;
  }
  free(texts);
  *out = w;
  return RIP_OK;
}

/* Writing */

int chd_write_sectors(struct chd_writer *w, const uint8_t *data, uint32_t count) {
  if (w->cd) {
    return RIP_ERR;
  }
  return append(w, data, (size_t)count * CD_DATA);
}

// Zero frames after a track, up to the next multiple of four.
static int pad_track(struct chd_writer *w) {
  static const uint8_t zero[CD_UNIT];
  uint32_t frames = w->tracks[w->track].frames;
  uint32_t padded = (frames + CD_TRACK_PADDING - 1) / CD_TRACK_PADDING * CD_TRACK_PADDING;

  for (; frames < padded; frames++) {
    if (append(w, zero, CD_UNIT) != RIP_OK) {
      return RIP_ERR;
    }
  }
  w->track++;
  w->track_written = 0;
  return RIP_OK;
}

int chd_write_frames(struct chd_writer *w, const uint8_t *frames, const uint8_t *sub, uint32_t count) {
  static const uint8_t no_sub[CD_SUB];
  uint32_t i;

  if (!w->cd) {
    return RIP_ERR;
  }
  for (i = 0; i < count; i++) {
    uint8_t unit[CD_UNIT];

    while (w->track < w->track_count && w->track_written == w->tracks[w->track].frames) {
      if (pad_track(w) != RIP_OK) {
        return RIP_ERR;
      }
    }
    if (w->track >= w->track_count) {
      RIP_ERROR("CHD: more frames than the tracks hold");
      return RIP_ERR;
    }
    memcpy(unit, frames + (size_t)i * CD_FRAME, CD_FRAME);
    if (w->tracks[w->track].mode == CD_MODE_AUDIO) {
      // CHD keeps audio samples big-endian; chdman swaps them on the way in and out.
      size_t k;
      for (k = 0; k < CD_FRAME; k += 2) {
        uint8_t b = unit[k];
        unit[k] = unit[k + 1];
        unit[k + 1] = b;
      }
    }
    memcpy(unit + CD_FRAME, sub ? sub + (size_t)i * CD_SUB : no_sub, CD_SUB);
    if (append(w, unit, CD_UNIT) != RIP_OK) {
      return RIP_ERR;
    }
    w->track_written++;
  }
  return RIP_OK;
}

/* The map */

struct bits {
  uint8_t *buf;
  size_t cap;
  size_t bytes;
  uint32_t acc;
  int used;  // bits in acc
};

static void bits_put(struct bits *b, uint32_t value, int count) {
  int i;

  for (i = count - 1; i >= 0; i--) {
    b->acc = (b->acc << 1) | ((value >> i) & 1);
    if (++b->used == 8) {
      if (b->bytes < b->cap) {
        b->buf[b->bytes] = (uint8_t)b->acc;
      }
      b->bytes++;
      b->acc = 0;
      b->used = 0;
    }
  }
}

static void bits_flush(struct bits *b) {
  if (b->used) {
    bits_put(b, 0, 8 - b->used);
  }
}

static int bits_for(uint32_t v) {
  int n = 0;

  while (v) {
    n++;
    v >>= 1;
  }
  return n;
}

// The map as libchdr reads it: a Huffman tree for the entry types, the types,
// then each entry's length and CRC. With 16 four-bit codes the tree is flat:
// type t is written as the four bits of t.
static int write_map(struct chd_writer *w, uint64_t *map_offset) {
  struct bits b;
  uint8_t head[16];
  int length_bits = bits_for(w->max_complen);
  uint32_t i;
  int rc;

  b.cap = (size_t)w->hunk_count * 8 + 64;
  b.buf = malloc(b.cap);
  b.bytes = 0;
  b.acc = 0;
  b.used = 0;
  if (!b.buf) {
    return RIP_ENOMEM;
  }
  for (i = 0; i < 16; i++) {
    bits_put(&b, 4, 4);  // every code is 4 bits long (RLE-coded tree, no runs)
  }
  for (i = 0; i < w->hunk_count; i++) {
    bits_put(&b, w->map[(size_t)i * 12], 4);
  }
  for (i = 0; i < w->hunk_count; i++) {
    const uint8_t *e = w->map + (size_t)i * 12;
    if (e[0] == COMP_TYPE_0) {
      bits_put(&b, ((uint32_t)e[1] << 16) | ((uint32_t)e[2] << 8) | e[3], length_bits);
    }
    bits_put(&b, rip_be16(e + 10), 16);
  }
  bits_flush(&b);
  if (b.bytes > b.cap) {
    free(b.buf);
    return RIP_ERR;
  }
  rip_put_be32(head, (uint32_t)b.bytes);
  put_be48(head + 4, w->first_offset);
  rip_put_be16(head + 10, crc16(w->map, (size_t)w->hunk_count * 12));
  head[12] = (uint8_t)length_bits;
  head[13] = 0;  // no self references
  head[14] = 0;  // no parent
  head[15] = 0;
  *map_offset = w->offset;
  rc = write_at(w, w->offset, head, sizeof(head));
  if (rc == RIP_OK) {
    rc = write_at(w, w->offset + sizeof(head), b.buf, b.bytes);
  }
  free(b.buf);
  return rc;
}

static int cmp_hash(const void *a, const void *b) {
  return memcmp(a, b, 24);
}

int chd_finish(struct chd_writer *w, uint8_t sha1[20]) {
  uint8_t raw[20];
  uint8_t overall[20];
  uint8_t h[HEADER_BYTES];
  struct rip_sha1 s;
  uint64_t map_offset = 0;
  int rc = RIP_ERR;

  if (w->cd) {
    while (w->track < w->track_count) {
      if (w->track_written != w->tracks[w->track].frames) {
        RIP_ERROR("CHD: track %u is %u frames short", w->track + 1, w->tracks[w->track].frames - w->track_written);
        goto done;
      }
      if (pad_track(w) != RIP_OK) {
        goto done;
      }
    }
  }
  if (w->written != w->logical_bytes) {
    RIP_ERROR("CHD: %llu of %llu bytes written", (unsigned long long)w->written, (unsigned long long)w->logical_bytes);
    goto done;
  }
  if (w->fill && flush_hunk(w) != RIP_OK) {
    goto done;
  }
  if (w->hunks_done != w->hunk_count || write_map(w, &map_offset) != RIP_OK) {
    goto done;
  }
  // The overall hash: the data's, then each metadata entry's, sorted.
  rip_sha1_final(&w->sha, raw);
  qsort(w->meta_hash, w->meta_count, 24, cmp_hash);
  rip_sha1_init(&s);
  rip_sha1_update(&s, raw, 20);
  rip_sha1_update(&s, w->meta_hash, (size_t)w->meta_count * 24);
  rip_sha1_final(&s, overall);
  write_header(w, h, map_offset, raw, overall);
  if (write_at(w, 0, h, sizeof(h)) != RIP_OK || fsync(w->fd) != 0) {
    goto done;
  }
  if (sha1) {
    memcpy(sha1, raw, 20);
  }
  rc = RIP_OK;

done:
  if (rc != RIP_OK) {
    unlink(w->path);
  }
  destroy(w);
  return rc;
}

void chd_abort(struct chd_writer *w) {
  if (!w) {
    return;
  }
  if (w->fd >= 0) {
    close(w->fd);
    w->fd = -1;
    unlink(w->path);
  }
  destroy(w);
}
