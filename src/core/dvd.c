#include "ripper/dvd.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ripper/common.h"
#include "ripper/log.h"
#include "ripper/udf.h"

// After the DVD-Video layout as libdvdread and the DVD FAQ document it: all
// fields big-endian, tables found by sector offsets in the IFO header.

#define SECTOR 2048u
#define MAX_IFO (1024u * 1024)

static uint16_t be16(const uint8_t *p) {
  return rip_be16(p);
}

static uint32_t be32(const uint8_t *p) {
  return rip_be32(p);
}

static unsigned bcd(uint8_t v) {
  return (v >> 4) * 10 + (v & 0x0F);
}

// Playback time: hours, minutes, seconds in BCD, then frames with the rate in the top bits.
static uint32_t time_ms(const uint8_t *t) {
  unsigned fps = (t[3] >> 6) == 1 ? 25 : 30;
  unsigned frames = bcd(t[3] & 0x3F);
  return (uint32_t)((bcd(t[0]) * 3600u + bcd(t[1]) * 60u + bcd(t[2])) * 1000u + frames * 1000u / fps);
}

uint32_t dvd_ycrcb_to_rgb(uint32_t v) {
  double y = (v >> 16) & 0xFF;
  double cr = ((v >> 8) & 0xFF) - 128.0;
  double cb = (v & 0xFF) - 128.0;
  double r = y + 1.402 * cr;
  double g = y - 0.344136 * cb - 0.714136 * cr;
  double b = y + 1.772 * cb;
  unsigned ri = r < 0 ? 0 : r > 255 ? 255 : (unsigned)(r + 0.5);
  unsigned gi = g < 0 ? 0 : g > 255 ? 255 : (unsigned)(g + 0.5);
  unsigned bi = b < 0 ? 0 : b > 255 ? 255 : (unsigned)(b + 0.5);
  return (ri << 16) | (gi << 8) | bi;
}

static void lang_code(const uint8_t *p, int present, char out[3]) {
  out[0] = out[1] = out[2] = '\0';
  if (present && p[0] >= 'a' && p[0] <= 'z' && p[1] >= 'a' && p[1] <= 'z') {
    out[0] = (char)p[0];
    out[1] = (char)p[1];
  }
}

// Within an IFO of len bytes, the table a header field points to (in sectors).
static const uint8_t *table(const uint8_t *ifo, size_t len, size_t field, size_t need) {
  uint64_t at = (uint64_t)be32(ifo + field) * SECTOR;

  if (!be32(ifo + field) || at + need > len) {
    return NULL;
  }
  return ifo + at;
}

// One title's program chain: cells (first angle only), chapters, controls.
static int read_pgc(const uint8_t *ifo, size_t len, const uint8_t *pgci, size_t pgci_len, unsigned pgcn,
                    uint32_t vobs_lba, struct dvd_title *t, const uint8_t *ptt, size_t ptt_len, unsigned ttn) {
  unsigned npgc = be16(pgci);
  const uint8_t *pgc;
  uint32_t pgc_off;
  unsigned nprog;
  unsigned ncell;
  const uint8_t *cells;
  const uint8_t *progmap;
  uint32_t *cell_start_ms;
  unsigned i;

  // The VTS's stream attributes run to 0x316.
  if (len < 0x316 || pgcn < 1 || pgcn > npgc || 8 + pgcn * 8 > pgci_len) {
    return RIP_ERR;
  }
  pgc_off = be32(pgci + 8 + (pgcn - 1) * 8 + 4);
  if (pgc_off + 0xEC > pgci_len) {
    return RIP_ERR;
  }
  pgc = pgci + pgc_off;
  nprog = pgc[2];
  ncell = pgc[3];
  t->ms = time_ms(pgc + 4);
  // Palette.
  for (i = 0; i < 16; i++) {
    t->palette[i] = be32(pgc + 0xA4 + i * 4) & 0xFFFFFF;
  }
  if (!ncell || pgc_off + be16(pgc + 0xE8) + ncell * 24u > pgci_len || pgc_off + be16(pgc + 0xE6) + nprog > pgci_len) {
    return RIP_ERR;
  }
  cells = pgc + be16(pgc + 0xE8);
  progmap = pgc + be16(pgc + 0xE6);
  t->cells = calloc(ncell, sizeof(*t->cells));
  cell_start_ms = calloc(ncell + 1, sizeof(*cell_start_ms));
  if (!t->cells || !cell_start_ms) {
    free(cell_start_ms);
    return RIP_ENOMEM;
  }
  {
    uint32_t clock = 0;
    for (i = 0; i < ncell; i++) {
      const uint8_t *c = cells + i * 24;
      unsigned mode = c[0] >> 6;        // 0 not in a block, 1 first, 2 middle, 3 last
      unsigned type = (c[0] >> 4) & 3;  // 1: an angle block
      cell_start_ms[i] = clock;
      // An angle block plays one of its cells: the first angle's.
      if (type == 1 && mode > 1) {
        continue;
      }
      t->cells[t->cell_count].first = vobs_lba + be32(c + 8);
      t->cells[t->cell_count].last = vobs_lba + be32(c + 20);
      t->cells[t->cell_count].ms = time_ms(c + 4);
      t->bytes += ((uint64_t)be32(c + 20) - be32(c + 8) + 1) * SECTOR;
      clock += time_ms(c + 4);
      t->cell_count++;
    }
    cell_start_ms[ncell] = clock;
  }
  // Chapters: the title's parts (PTT), each a program, which starts at a cell.
  if (ptt && ttn) {
    unsigned ntitles = be16(ptt);
    if (ttn <= ntitles && 8 + ttn * 4 <= ptt_len) {
      uint32_t start = be32(ptt + 8 + (ttn - 1) * 4);
      uint32_t end = ttn < ntitles ? be32(ptt + 8 + ttn * 4) : be32(ptt + 4) + 1;
      uint32_t at;
      for (at = start; at + 4 <= end && at + 4 <= ptt_len && t->chapter_count < DVD_MAX_CHAPTERS; at += 4) {
        unsigned pgn = be16(ptt + at + 2);
        if (be16(ptt + at) == pgcn && pgn >= 1 && pgn <= nprog) {
          unsigned cell = progmap[pgn - 1];
          t->chapter_ms[t->chapter_count++] = cell >= 1 && cell <= ncell ? cell_start_ms[cell - 1] : 0;
        }
      }
    }
  }
  if (!t->chapter_count) {
    for (i = 0; i < nprog && i < DVD_MAX_CHAPTERS; i++) {
      unsigned cell = progmap[i];
      t->chapter_ms[t->chapter_count++] = cell >= 1 && cell <= ncell ? cell_start_ms[cell - 1] : 0;
    }
  }
  free(cell_start_ms);

  // Streams: the VTS's attributes, the PGC's controls say which are there
  // and which physical stream each is.
  {
    unsigned naudio = be16(ifo + 0x202);
    unsigned nsub = be16(ifo + 0x254);
    for (i = 0; i < naudio && i < DVD_MAX_AUDIO; i++) {
      uint16_t ctl = be16(pgc + 0x0C + i * 2);
      const uint8_t *a = ifo + 0x204 + i * 8;
      struct dvd_audio *o;
      unsigned phys;
      if (!(ctl & 0x8000)) {
        continue;
      }
      phys = (ctl >> 8) & 7;
      o = &t->audio[t->audio_count++];
      switch (a[0] >> 5) {
      case 0:
        o->format = DVD_AUDIO_AC3;
        o->stream_id = (uint8_t)(0x80 + phys);
        break;
      case 2:
        o->format = DVD_AUDIO_MPEG1;
        o->stream_id = (uint8_t)(0xC0 + phys);
        break;
      case 3:
        o->format = DVD_AUDIO_MPEG2;
        o->stream_id = (uint8_t)(0xC0 + phys);
        break;
      case 4:
        o->format = DVD_AUDIO_LPCM;
        o->stream_id = (uint8_t)(0xA0 + phys);
        break;
      case 6:
        o->format = DVD_AUDIO_DTS;
        o->stream_id = (uint8_t)(0x88 + phys);
        break;
      default:
        o->format = DVD_AUDIO_OTHER;
        o->stream_id = (uint8_t)(0x80 + phys);
        break;
      }
      o->channels = (a[1] & 7) + 1;
      o->sample_rate = ((a[1] >> 4) & 3) == 1 ? 96000 : 48000;
      lang_code(a + 2, ((a[0] >> 2) & 3) == 1, o->lang);
      o->extension = a[5];
    }
    for (i = 0; i < nsub && i < DVD_MAX_SUBS; i++) {
      const uint8_t *ctl = pgc + 0x1C + i * 4;
      const uint8_t *a = ifo + 0x256 + i * 6;
      struct dvd_sub *o;
      if (!(ctl[0] & 0x80)) {
        continue;
      }
      o = &t->subs[t->sub_count++];
      // The 4:3 stream, or the widescreen one on a 16:9 title.
      o->stream_id = (uint8_t)(0x20 + ((t->widescreen ? ctl[1] : ctl[0]) & 0x1F));
      lang_code(a + 2, (a[0] & 3) == 1, o->lang);
      o->extension = a[5];
    }
  }
  return RIP_OK;
}

int dvd_parse(const uint8_t *vmg, size_t vmg_len, const uint8_t *const *vts, const size_t *vts_len,
              const uint32_t *vts_lba, unsigned vts_count, struct dvd_disc *out) {
  const uint8_t *tt;
  unsigned ntitles;
  unsigned i;
  uint32_t longest = 0;

  memset(out, 0, sizeof(*out));
  out->main_title = -1;
  if (vmg_len < 0x100 || memcmp(vmg, "DVDVIDEO-VMG", 12) != 0) {
    RIP_ERROR("dvd: VIDEO_TS.IFO isn't a video manager");
    return RIP_ERR;
  }
  tt = table(vmg, vmg_len, 0xC4, 8);
  if (!tt) {
    RIP_ERROR("dvd: no title table");
    return RIP_ERR;
  }
  ntitles = be16(tt);
  for (i = 0; i < ntitles && i < DVD_MAX_TITLES; i++) {
    const uint8_t *e = tt + 8 + i * 12;
    unsigned vtsn;
    unsigned ttn;
    const uint8_t *ifo;
    size_t len;
    const uint8_t *ptt;
    const uint8_t *pgci;
    struct dvd_title *t;
    unsigned v;
    if ((size_t)(e + 12 - vmg) > vmg_len) {
      break;
    }
    vtsn = e[6];
    ttn = e[7];
    if (vtsn < 1 || vtsn > vts_count || !vts[vtsn - 1]) {
      continue;
    }
    ifo = vts[vtsn - 1];
    len = vts_len[vtsn - 1];
    if (len < 0x400 || memcmp(ifo, "DVDVIDEO-VTS", 12) != 0) {
      continue;
    }
    ptt = table(ifo, len, 0xC8, 8);
    pgci = table(ifo, len, 0xCC, 8);
    if (!ptt || !pgci) {
      continue;
    }
    t = &out->titles[out->title_count];
    memset(t, 0, sizeof(*t));
    t->number = i + 1;
    t->vts = vtsn;
    t->angles = e[1];
    // Video attributes: 16:9, PAL, the picture size.
    v = be16(ifo + 0x200);
    t->widescreen = ((v >> 10) & 3) == 3;
    t->pal = ((v >> 12) & 3) == 1;
    t->height = t->pal ? 576 : 480;
    switch ((v >> 3) & 7) {
    case 1:
      t->width = 704;
      break;
    case 2:
      t->width = 352;
      break;
    case 3:
      t->width = 352;
      t->height /= 2;
      break;
    default:
      t->width = 720;
      break;
    }
    {
      // The title's first part names its program chain.
      unsigned nt = be16(ptt);
      size_t ptt_len = len - (size_t)(ptt - ifo);
      size_t pgci_len = len - (size_t)(pgci - ifo);
      uint32_t vobs = be32(ifo + 0xC4);
      if (ttn < 1 || ttn > nt || 8 + ttn * 4 > ptt_len) {
        continue;
      }
      t->pgcn = be16(ptt + be32(ptt + 8 + (ttn - 1) * 4));
      if (read_pgc(ifo, len, pgci, pgci_len, t->pgcn, vts_lba[vtsn - 1] + vobs, t, ptt, ptt_len, ttn) != RIP_OK ||
          !t->cell_count) {
        free(t->cells);
        t->cells = NULL;
        continue;
      }
    }
    if (t->ms > longest) {
      longest = t->ms;
      out->main_title = (int)out->title_count;
    }
    out->title_count++;
  }
  return out->title_count ? RIP_OK : RIP_ERR;
}

void dvd_free(struct dvd_disc *d) {
  unsigned i;

  for (i = 0; i < d->title_count; i++) {
    free(d->titles[i].cells);
    d->titles[i].cells = NULL;
  }
  d->title_count = 0;
}

int dvd_load(struct rip_reader *r, struct dvd_disc *out) {
  struct udf_fs *fs = malloc(sizeof(*fs));
  struct udf_entry e;
  uint8_t *vmg = NULL;
  size_t vmg_len = 0;
  uint8_t *vts[DVD_MAX_TITLES] = {0};
  size_t vts_len[DVD_MAX_TITLES] = {0};
  uint32_t vts_lba[DVD_MAX_TITLES] = {0};
  unsigned count = 0;
  unsigned i;
  int rc = RIP_ERR;

  memset(out, 0, sizeof(*out));
  out->main_title = -1;
  if (!fs || udf_mount(fs, r) != RIP_OK || udf_lookup(fs, "VIDEO_TS/VIDEO_TS.IFO", &e) != RIP_OK ||
      udf_read_file(fs, &e, MAX_IFO, &vmg, &vmg_len) != RIP_OK) {
    RIP_ERROR("dvd: no VIDEO_TS/VIDEO_TS.IFO");
    goto done;
  }
  if (vmg_len >= 0x40) {
    count = be16(vmg + 0x3E);  // title sets on the disc
  }
  for (i = 0; i < count && i < DVD_MAX_TITLES; i++) {
    char path[48];
    snprintf(path, sizeof(path), "VIDEO_TS/VTS_%02u_0.IFO", i + 1);
    if (udf_lookup(fs, path, &e) != RIP_OK || !e.extent_count ||
        udf_read_file(fs, &e, MAX_IFO, &vts[i], &vts_len[i]) != RIP_OK) {
      RIP_WARN("dvd: %s can't be read", path);
      continue;
    }
    vts_lba[i] = e.extents[0].lba;
  }
  rc = dvd_parse(vmg, vmg_len, (const uint8_t *const *)vts, vts_len, vts_lba, count, out);
  if (rc == RIP_OK) {
    const struct dvd_title *m = out->main_title >= 0 ? &out->titles[out->main_title] : NULL;
    RIP_INFO("dvd: %u title(s)%s", out->title_count, m ? "" : ", no main title");
    if (m) {
      RIP_INFO("dvd: main title %u: %u:%02u:%02u, %u chapter(s), %u audio, %u subtitle(s), %u cell(s), %.1f GB",
               m->number, m->ms / 3600000, m->ms / 60000 % 60, m->ms / 1000 % 60, m->chapter_count, m->audio_count,
               m->sub_count, m->cell_count, (double)m->bytes / 1e9);
    }
  }

done:
  free(vmg);
  for (i = 0; i < DVD_MAX_TITLES; i++) {
    free(vts[i]);
  }
  free(fs);
  return rc;
}
