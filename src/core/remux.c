#include "ripper/remux.h"

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ripper/common.h"
#include "ripper/log.h"
#include "ripper/util.h"

#ifdef RIPPER_HAVE_FFMPEG

#include <libavcodec/avcodec.h>
#include <libavfilter/avfilter.h>
#include <libavfilter/buffersink.h>
#include <libavfilter/buffersrc.h>
#include <libavformat/avformat.h>
#include <libavutil/audio_fifo.h>
#include <libavutil/channel_layout.h>
#include <libavutil/opt.h>
#include <libswresample/swresample.h>

#define SECTOR 2048u
#define READ_SECTORS 32u
#define IO_BUF 65536
#define TICKS 90000  // MPEG-PS clock

/* Reading the title */

// The cells' sectors one after another, as the demuxer's input. A thread reads
// ahead into a ring of chunks so the disc and the encoder work at the same
// time (one after the other, reading took half of each minute). A sector that
// won't read is left out (and counted) rather than ending the title.

#define RING 32  // chunks of READ_SECTORS: 2 MB ahead
#ifndef REMUX_LOG_MS
#define REMUX_LOG_MS 60000  // the rate line's interval
#endif

struct chunk {
  uint8_t data[READ_SECTORS * SECTOR];
  size_t len;
};

struct src {
  struct rip_reader *r;
  const struct dvd_title *t;
  remux_progress_fn progress;
  void *ud;
  uint64_t total;
  // The reader thread's
  unsigned cell;
  uint32_t lba;
  uint64_t bad;
  uint64_t read_ms;
  pthread_t thread;
  int started;
  // Shared, under lock
  pthread_mutex_t lock;
  pthread_cond_t cond;
  struct chunk *ring;
  unsigned head;  // next to fill
  unsigned tail;  // next to take
  unsigned filled;
  int end;   // the reader is past the last cell
  int stop;  // cancelled: the reader stops
  // The demuxer's
  struct chunk *cur;
  size_t pos;
  uint64_t done;
  uint64_t wait_ms;  // the demuxer waiting for the disc
  uint64_t start_ms;
  uint64_t logged_ms;
  uint64_t logged_done;
  uint64_t logged_read_ms;
  uint64_t logged_wait_ms;
};

static void *reader_thread(void *arg) {
  struct src *s = arg;

  for (;;) {
    const struct dvd_cell *c;
    struct chunk *ch;
    uint32_t n;
    uint64_t t0;
    pthread_mutex_lock(&s->lock);
    while (s->filled == RING && !s->stop) {
      pthread_cond_wait(&s->cond, &s->lock);
    }
    if (s->stop || s->cell >= s->t->cell_count) {
      s->end = 1;
      pthread_cond_broadcast(&s->cond);
      pthread_mutex_unlock(&s->lock);
      return NULL;
    }
    ch = &s->ring[s->head];
    pthread_mutex_unlock(&s->lock);

    c = &s->t->cells[s->cell];
    if (s->lba < c->first || s->lba > c->last) {
      s->lba = c->first;
    }
    n = c->last - s->lba + 1 < READ_SECTORS ? c->last - s->lba + 1 : READ_SECTORS;
    ch->len = 0;
    t0 = rip_now_ms();
    if (rip_reader_read(s->r, s->lba, n, ch->data) == RIP_OK) {
      ch->len = (size_t)n * SECTOR;
    } else {
      uint32_t i;
      for (i = 0; i < n; i++) {
        if (rip_reader_read(s->r, s->lba + i, 1, ch->data + ch->len) == RIP_OK) {
          ch->len += SECTOR;
        } else {
          s->bad++;
        }
      }
    }
    s->read_ms += rip_now_ms() - t0;
    s->lba += n;
    if (s->lba > c->last) {
      s->cell++;
      if (s->cell < s->t->cell_count) {
        s->lba = s->t->cells[s->cell].first;
      }
    }
    pthread_mutex_lock(&s->lock);
    s->head = (s->head + 1) % RING;
    s->filled++;
    pthread_cond_broadcast(&s->cond);
    pthread_mutex_unlock(&s->lock);
  }
}

static int src_start(struct src *s) {
  s->ring = calloc(RING, sizeof(*s->ring));
  if (!s->ring) {
    return -1;
  }
  pthread_mutex_init(&s->lock, NULL);
  pthread_cond_init(&s->cond, NULL);
  if (pthread_create(&s->thread, NULL, reader_thread, s) != 0) {
    free(s->ring);
    s->ring = NULL;
    return -1;
  }
  s->started = 1;
  return 0;
}

static void src_stop(struct src *s) {
  if (!s->started) {
    return;
  }
  pthread_mutex_lock(&s->lock);
  s->stop = s->stop ? s->stop : 2;  // 2: done with it, not cancelled
  pthread_cond_broadcast(&s->cond);
  pthread_mutex_unlock(&s->lock);
  pthread_join(s->thread, NULL);
  pthread_mutex_destroy(&s->lock);
  pthread_cond_destroy(&s->cond);
  free(s->ring);
  s->ring = NULL;
  s->started = 0;
}

// The newest timestamp each output stream got, in seconds: the minute's log
// line shows how far apart video and audio are (the muxer holds the gap).
static double g_stream_at[8];

static void log_rate(struct src *s) {
  uint64_t now = rip_now_ms();
  double secs;
  double mb;

  if (!s->start_ms) {
    s->start_ms = s->logged_ms = now;
  }
  if (now - s->logged_ms < REMUX_LOG_MS) {
    return;
  }
  secs = (double)(now - s->logged_ms) / 1000.0;
  mb = (double)(s->done - s->logged_done) / 1e6;
  RIP_INFO(
      "remux: %.1f MB in %.0f s (%.2f MB/s); the disc read for %.0f s, the encoder waited for it %.0f s; "
      "video at %.0f s, audio at %.0f s",
      mb, secs, mb / secs, (double)(s->read_ms - s->logged_read_ms) / 1000.0,
      (double)(s->wait_ms - s->logged_wait_ms) / 1000.0, g_stream_at[0], g_stream_at[1]);
  s->logged_ms = now;
  s->logged_done = s->done;
  s->logged_read_ms = s->read_ms;
  s->logged_wait_ms = s->wait_ms;
}

static int read_cb(void *opaque, uint8_t *dst, int size) {
  struct src *s = opaque;
  int out = 0;

  while (out < size) {
    if (!s->cur || s->pos == s->cur->len) {
      uint64_t t0 = rip_now_ms();
      pthread_mutex_lock(&s->lock);
      if (s->cur) {  // that chunk is used up: its slot is free
        s->tail = (s->tail + 1) % RING;
        s->filled--;
        s->cur = NULL;
        pthread_cond_broadcast(&s->cond);
      }
      while (!s->filled && !s->end && s->stop != 1) {
        pthread_cond_wait(&s->cond, &s->lock);
      }
      if (s->stop == 1 || !s->filled) {
        int stopped = s->stop == 1;
        pthread_mutex_unlock(&s->lock);
        if (stopped) {
          return AVERROR_EXIT;
        }
        break;  // the end of the title
      }
      s->cur = &s->ring[s->tail];
      s->pos = 0;
      pthread_mutex_unlock(&s->lock);
      s->wait_ms += rip_now_ms() - t0;
      s->done += s->cur->len ? s->cur->len : 0;
      log_rate(s);
      if (s->progress && s->progress(s->ud, s->done, s->total)) {
        pthread_mutex_lock(&s->lock);
        s->stop = 1;
        pthread_cond_broadcast(&s->cond);
        pthread_mutex_unlock(&s->lock);
        return AVERROR_EXIT;
      }
      continue;
    }
    {
      size_t take = s->cur->len - s->pos < (size_t)(size - out) ? s->cur->len - s->pos : (size_t)(size - out);
      memcpy(dst + out, s->cur->data + s->pos, take);
      s->pos += take;
      out += (int)take;
    }
  }
  return out ? out : AVERROR_EOF;
}

/* Streams */

// Matroska wants ISO 639-2 (bibliographic): the languages DVDs carry.
static const char *lang3(const char lang[3]) {
  static const char *const map[][2] = {
      {"en", "eng"}, {"fr", "fre"}, {"es", "spa"}, {"de", "ger"}, {"it", "ita"}, {"ja", "jpn"}, {"pt", "por"},
      {"nl", "dut"}, {"sv", "swe"}, {"da", "dan"}, {"no", "nor"}, {"fi", "fin"}, {"ru", "rus"}, {"pl", "pol"},
      {"cs", "cze"}, {"hu", "hun"}, {"el", "gre"}, {"tr", "tur"}, {"zh", "chi"}, {"ko", "kor"}, {"he", "heb"},
      {"ar", "ara"}, {"th", "tha"}, {"hi", "hin"}, {"is", "ice"}, {"ca", "cat"}, {"ro", "rum"}, {"uk", "ukr"},
      {"hr", "hrv"}, {"sk", "slo"}, {"sl", "slv"}, {"bg", "bul"}, {"id", "ind"}, {"ms", "may"}, {"vi", "vie"},
  };
  size_t i;

  for (i = 0; lang[0] && i < sizeof(map) / sizeof(map[0]); i++) {
    if (!strncmp(lang, map[i][0], 2)) {
      return map[i][1];
    }
  }
  return "und";
}

// The demuxer's stream id for an IFO stream id: MPEG audio is 0x1C0+n there.
static int ps_id(uint8_t ifo_id) {
  return ifo_id >= 0xC0 && ifo_id <= 0xC7 ? 0x100 + ifo_id : ifo_id;
}

static AVRational sample_aspect(const struct dvd_title *t) {
  AVRational r;
  if (t->widescreen) {
    r = t->pal ? (AVRational){64, 45} : (AVRational){32, 27};
  } else {
    r = t->pal ? (AVRational){16, 15} : (AVRational){8, 9};
  }
  if (t->width == 704) {
    // 704 wide: the 720 figures scaled.
    r = av_mul_q(r, (AVRational){720, 704});
  }
  return r;
}

static void add_chapters(AVFormatContext *out, const struct dvd_title *t) {
  unsigned i;

  out->chapters = av_calloc(t->chapter_count, sizeof(AVChapter *));
  if (!out->chapters) {
    return;
  }
  for (i = 0; i < t->chapter_count; i++) {
    AVChapter *c = av_mallocz(sizeof(AVChapter));
    char title[32];
    if (!c) {
      break;
    }
    c->id = i;
    c->time_base = (AVRational){1, 1000};
    c->start = t->chapter_ms[i];
    c->end = i + 1 < t->chapter_count ? t->chapter_ms[i + 1] : t->ms;
    snprintf(title, sizeof(title), "Chapter %02u", i + 1);
    av_dict_set(&c->metadata, "title", title, 0);
    out->chapters[out->nb_chapters++] = c;
  }
}

// VobSub's palette and frame size, as Matroska's codec private.
static void vobsub_extradata(AVCodecParameters *par, const struct dvd_title *t) {
  char text[400];
  int n = snprintf(text, sizeof(text), "size: %ux%u\npalette: ", t->width, t->height);
  unsigned i;

  for (i = 0; i < 16; i++) {
    n += snprintf(text + n, sizeof(text) - (size_t)n, "%06x%s", dvd_ycrcb_to_rgb(t->palette[i]), i < 15 ? ", " : "\n");
  }
  par->extradata = av_mallocz((size_t)n + AV_INPUT_BUFFER_PADDING_SIZE);
  if (par->extradata) {
    memcpy(par->extradata, text, (size_t)n);
    par->extradata_size = n;
  }
}

/* Timestamps */

// Cells can restart the clock (a new VOB, the layer break). Each packet is
// placed in its cell by its byte position; when a cell starts, its clock is
// lined up with where the cell begins by the IFO's cell times, for every
// stream at once. Within a cell the disc's own timestamps are kept.
//
// A packet can come out in the new cell on the old cell's clock: an AC-3 frame
// that began at the end of the cell before is only complete once the next
// audio packet is read. On the new cell's clock it would land minutes away
// (TWINE's cell 19: 373 s ahead), and every packet of that stream behind it
// would be dropped until the clock caught up. So each stream keeps the offset
// it's on while its own timestamps carry on (under a second from its last
// packet); a stream that jumps takes the cell's clock. The cell is lined up by
// the first jump of an audio or video stream that was playing as the cell
// began (not a subtitle, or a track coming back from silence, which can jump
// anywhere). A jump that lands outside its cell is a stray, dropped alone.
#define CELL_SLACK (3 * TICKS)

struct stream_clock {
  int64_t raw;     // the last packet's timestamp on the disc's clock, INT64_MIN for none
  int64_t offset;  // and the offset it took
  int64_t at;      // where it went on the title's timeline
  int64_t dts;     // in the stream's time base, for monotonic output
};

struct clock {
  int64_t offset;
  unsigned cell;  // the furthest cell reached
  int pending;    // a new cell, not lined up yet
  int started;
  uint64_t *cell_pos;   // byte position each cell starts at in the stream
  int64_t *cell_ticks;  // where each cell starts on the title's timeline
  unsigned cells;
  struct stream_clock streams[512];  // per demuxer stream
};

static int clock_init(struct clock *c, const struct dvd_title *t) {
  uint64_t pos = 0;
  int64_t ticks = 0;
  unsigned i;

  memset(c, 0, sizeof(*c));
  c->cells = t->cell_count;
  c->cell_pos = calloc(t->cell_count + 1, sizeof(*c->cell_pos));
  c->cell_ticks = calloc(t->cell_count + 1, sizeof(*c->cell_ticks));
  if (!c->cell_pos || !c->cell_ticks) {
    return AVERROR(ENOMEM);
  }
  for (i = 0; i < t->cell_count; i++) {
    c->cell_pos[i] = pos;
    c->cell_ticks[i] = ticks;
    pos += ((uint64_t)t->cells[i].last - t->cells[i].first + 1) * SECTOR;
    ticks += (int64_t)t->cells[i].ms * (TICKS / 1000);
  }
  c->cell_pos[i] = pos;
  c->cell_ticks[i] = ticks;
  for (i = 0; i < 512; i++) {
    c->streams[i].raw = INT64_MIN;
    c->streams[i].dts = INT64_MIN;
  }
  return 0;
}

static void clock_free(struct clock *c) {
  free(c->cell_pos);
  free(c->cell_ticks);
  c->cell_pos = NULL;
  c->cell_ticks = NULL;
}

static unsigned cell_of(const struct clock *c, int64_t pos) {
  unsigned lo = 0;
  unsigned hi = c->cells;

  if (pos < 0) {
    return c->cell;
  }
  while (lo + 1 < hi) {
    unsigned mid = (lo + hi) / 2;
    if ((uint64_t)pos >= c->cell_pos[mid]) {
      lo = mid;
    } else {
      hi = mid;
    }
  }
  return lo;
}

// Whether a jump in this stream's clock may line the cell up: an audio or
// video stream that was playing as the cell began.
static int starts_cell(const struct clock *c, const AVStream *st, const struct stream_clock *sc) {
  enum AVMediaType type = st->codecpar->codec_type;

  if (c->cell == 0) {
    return 1;
  }
  return (type == AVMEDIA_TYPE_VIDEO || type == AVMEDIA_TYPE_AUDIO) && sc && sc->raw != INT64_MIN &&
         sc->at >= c->cell_ticks[c->cell] - TICKS;
}

// 1: drop the packet (before the start, a stray timestamp, or behind what's
// been written).
static int fix_ts(struct clock *c, AVPacket *pkt, const AVStream *st) {
  int64_t ts = pkt->dts != AV_NOPTS_VALUE ? pkt->dts : pkt->pts;
  unsigned cell = cell_of(c, pkt->pos);
  struct stream_clock *sc = pkt->stream_index < 512 ? &c->streams[pkt->stream_index] : NULL;
  int64_t offset, at, dts;
  int carries_on;

  if (!c->started || cell > c->cell) {
    c->pending = 1;
    c->cell = cell;
    c->started = 1;
  }
  if (ts == AV_NOPTS_VALUE) {
    return 0;
  }
  ts = av_rescale_q(ts, st->time_base, (AVRational){1, TICKS});
  carries_on = sc && sc->raw != INT64_MIN && ts >= sc->raw && ts - sc->raw <= TICKS;
  if (!carries_on && c->pending && cell == c->cell && starts_cell(c, st, sc)) {
    int64_t now = ts + c->offset;
    c->pending = 0;
    if (cell == 0 || now < c->cell_ticks[cell] - TICKS || now > c->cell_ticks[cell] + TICKS) {
      if (cell) {
        RIP_INFO("remux: cell %u's clock is %.3f s off; lined up", cell + 1,
                 (double)(now - c->cell_ticks[cell]) / TICKS);
      }
      c->offset = c->cell_ticks[cell] - ts;
    }
  }
  offset = carries_on ? sc->offset : c->offset;
  at = ts + offset;
  if (at < 0 || (!carries_on && (at < c->cell_ticks[cell] - CELL_SLACK || at > c->cell_ticks[cell + 1] + CELL_SLACK))) {
    return 1;
  }
  {
    int64_t off = av_rescale_q(offset, (AVRational){1, TICKS}, st->time_base);
    if (pkt->dts != AV_NOPTS_VALUE) {
      pkt->dts += off;
    }
    if (pkt->pts != AV_NOPTS_VALUE) {
      pkt->pts += off;
    }
  }
  dts = pkt->dts != AV_NOPTS_VALUE ? pkt->dts : pkt->pts;
  if (!sc) {
    return 0;
  }
  if (sc->dts != INT64_MIN && dts <= sc->dts) {
    return 1;
  }
  sc->raw = ts;
  sc->offset = offset;
  sc->at = at;
  sc->dts = dts;
  return 0;
}

/* Shared setup */

struct job {
  struct src src;
  AVFormatContext *in;
  AVFormatContext *out;
  int map[512];  // demuxer stream index -> output stream index, -1 unused
  struct clock clock;
};

static int open_input(struct job *j) {
  const AVInputFormat *fmt = av_find_input_format("mpeg");
  uint8_t *io = av_malloc(IO_BUF);
  int rc;

  j->in = avformat_alloc_context();
  if (!io || !j->in) {
    av_free(io);
    return AVERROR(ENOMEM);
  }
  j->in->pb = avio_alloc_context(io, IO_BUF, 0, &j->src, read_cb, NULL, NULL);
  if (!j->in->pb) {
    av_free(io);
    return AVERROR(ENOMEM);
  }
  // Not AVFMT_FLAG_GENPTS: where a cell's clock starts over, it read hundreds
  // of MB ahead looking for timestamps and rescanned them all for each packet
  // (an MP4 got slower and slower). fix_ts() copes with packets without one.
  j->in->flags |= AVFMT_FLAG_CUSTOM_IO;
  j->in->probesize = 32 << 20;
  j->in->max_analyze_duration = 15 * AV_TIME_BASE;
  rc = avformat_open_input(&j->in, NULL, fmt, NULL);
  if (rc < 0) {
    return rc;
  }
  return avformat_find_stream_info(j->in, NULL);
}

static void close_job(struct job *j) {
  if (j->in) {
    AVIOContext *pb = j->in->pb;
    avformat_close_input(&j->in);
    if (pb) {
      av_freep(&pb->buffer);
      avio_context_free(&pb);
    }
  }
  if (j->out) {
    if (j->out->pb) {
      avio_closep(&j->out->pb);
    }
    avformat_free_context(j->out);
    j->out = NULL;
  }
}

static AVStream *find_in(AVFormatContext *in, int id) {
  unsigned i;

  for (i = 0; i < in->nb_streams; i++) {
    if (in->streams[i]->id == id) {
      return in->streams[i];
    }
  }
  return NULL;
}

static const char *err_str(int rc, char *buf, size_t n) {
  av_strerror(rc, buf, n);
  return buf;
}

/* MKV: every stream as it is */

// Packets the demuxer gives a DTS but no PTS (about one in twenty here), which
// Matroska won't take. AVFMT_FLAG_GENPTS worked them out by reading ahead, but
// where a cell's clock starts over it read hundreds of MB ahead. So, after
// fix_ts() made the clock continuous: audio and subtitles show at their DTS;
// an MPEG-2 B-picture too; an I- or P-picture shows when the next of them is
// decoded, so it waits in a short queue for that one's DTS.
#define PTS_HOLD 64

struct pts_fill {
  AVPacket *q[PTS_HOLD];
  int n;
  int64_t step;  // a frame, in the output's time base
};

// 1 I, 2 P, 3 B; 0 when the packet holds no picture header.
static int mpeg2_picture_type(const AVPacket *p) {
  int i;

  for (i = 0; i + 5 < p->size; i++) {
    if (p->data[i] == 0 && p->data[i + 1] == 0 && p->data[i + 2] == 1 && p->data[i + 3] == 0) {
      return (p->data[i + 5] >> 3) & 7;
    }
  }
  return 0;
}

static void write_packet(struct job *j, AVPacket *pkt) {
  int oi = pkt->stream_index;
  int rc;

  if (oi < 8 && pkt->dts != AV_NOPTS_VALUE) {
    g_stream_at[oi] = (double)pkt->dts * av_q2d(j->out->streams[oi]->time_base);
  }
  rc = av_interleaved_write_frame(j->out, pkt);
  if (rc < 0) {
    char e[128];
    RIP_WARN("remux: a packet was refused (%s); carried on", err_str(rc, e, sizeof(e)));
  }
}

// Writes the queue's head while it has its PTS (all: everything, guessing the rest).
static void pts_flush(struct job *j, struct pts_fill *f, int all) {
  int k = 0;

  while (k < f->n && (f->q[k]->pts != AV_NOPTS_VALUE || all)) {
    AVPacket *p = f->q[k];
    if (p->pts == AV_NOPTS_VALUE) {
      p->pts = p->dts != AV_NOPTS_VALUE ? p->dts + f->step : AV_NOPTS_VALUE;
    }
    write_packet(j, p);
    av_packet_free(&p);
    k++;
  }
  if (k) {
    memmove(f->q, f->q + k, (size_t)(f->n - k) * sizeof(f->q[0]));
    f->n -= k;
  }
}

static void pts_video(struct job *j, struct pts_fill *f, AVPacket *pkt) {
  int type = mpeg2_picture_type(pkt);
  AVPacket *held;
  int k;

  if (type == 3 && pkt->pts == AV_NOPTS_VALUE) {
    pkt->pts = pkt->dts;
  }
  if ((type == 1 || type == 2) && pkt->dts != AV_NOPTS_VALUE) {
    // The reference picture before this one shows now.
    for (k = f->n - 1; k >= 0; k--) {
      if (f->q[k]->pts == AV_NOPTS_VALUE) {
        f->q[k]->pts = pkt->dts > f->q[k]->dts ? pkt->dts : f->q[k]->dts + f->step;
        break;
      }
    }
  }
  if (!f->n && pkt->pts != AV_NOPTS_VALUE) {
    write_packet(j, pkt);
    return;
  }
  if (f->n == PTS_HOLD) {
    pts_flush(j, f, 0);
    if (f->n == PTS_HOLD) {  // still stuck: the head goes as best it can
      if (f->q[0]->pts == AV_NOPTS_VALUE) {
        f->q[0]->pts = f->q[0]->dts + f->step;
      }
      pts_flush(j, f, 0);
    }
  }
  held = av_packet_alloc();
  if (!held) {
    return;
  }
  av_packet_move_ref(held, pkt);
  f->q[f->n++] = held;
  pts_flush(j, f, 0);
}

static int write_mkv(struct job *j, const char *path) {
  const struct dvd_title *t = j->src.t;
  AVStream *vin = find_in(j->in, 0x1E0);
  AVStream *o;
  AVPacket *pkt = av_packet_alloc();
  struct pts_fill fill;
  unsigned i;
  int rc;

  memset(&fill, 0, sizeof(fill));
  if (!pkt || !vin) {
    av_packet_free(&pkt);
    RIP_ERROR("remux: no video in this title");
    return AVERROR_INVALIDDATA;
  }
  rc = avformat_alloc_output_context2(&j->out, NULL, "matroska", path);
  if (rc < 0) {
    av_packet_free(&pkt);
    return rc;
  }
  for (i = 0; i < sizeof(j->map) / sizeof(j->map[0]); i++) {
    j->map[i] = -1;
  }
  // Video.
  o = avformat_new_stream(j->out, NULL);
  avcodec_parameters_copy(o->codecpar, vin->codecpar);
  o->codecpar->codec_tag = 0;
  o->codecpar->sample_aspect_ratio = sample_aspect(t);
  o->sample_aspect_ratio = o->codecpar->sample_aspect_ratio;
  o->time_base = vin->time_base;
  j->map[vin->index] = o->index;
  // Audio, in the disc's order: the probed stream when there is one, else
  // what the IFO says.
  for (i = 0; i < t->audio_count; i++) {
    const struct dvd_audio *a = &t->audio[i];
    AVStream *ain = find_in(j->in, ps_id(a->stream_id));
    if (a->format == DVD_AUDIO_LPCM || a->format == DVD_AUDIO_OTHER) {
      RIP_WARN("remux: audio track %u (%s) isn't carried over", i + 1, a->format == DVD_AUDIO_LPCM ? "LPCM" : "?");
      continue;
    }
    o = avformat_new_stream(j->out, NULL);
    if (ain) {
      avcodec_parameters_copy(o->codecpar, ain->codecpar);
      j->map[ain->index] = o->index;
      o->time_base = ain->time_base;
    } else {
      o->codecpar->codec_type = AVMEDIA_TYPE_AUDIO;
      o->codecpar->codec_id = a->format == DVD_AUDIO_AC3   ? AV_CODEC_ID_AC3
                              : a->format == DVD_AUDIO_DTS ? AV_CODEC_ID_DTS
                                                           : AV_CODEC_ID_MP2;
      o->codecpar->sample_rate = (int)a->sample_rate;
      av_channel_layout_default(&o->codecpar->ch_layout, (int)a->channels);
      o->time_base = (AVRational){1, TICKS};
    }
    o->codecpar->codec_tag = 0;
    av_dict_set(&o->metadata, "language", lang3(a->lang), 0);
    if (a->extension == 3 || a->extension == 4) {
      av_dict_set(&o->metadata, "title", "Commentary", 0);
      o->disposition |= AV_DISPOSITION_COMMENT;
    }
    if (i == 0) {
      o->disposition |= AV_DISPOSITION_DEFAULT;
    }
  }
  // Subtitles (VobSub): the IFO's, palette included.
  for (i = 0; i < t->sub_count; i++) {
    const struct dvd_sub *sub = &t->subs[i];
    AVStream *sin = find_in(j->in, sub->stream_id);
    o = avformat_new_stream(j->out, NULL);
    o->codecpar->codec_type = AVMEDIA_TYPE_SUBTITLE;
    o->codecpar->codec_id = AV_CODEC_ID_DVD_SUBTITLE;
    o->codecpar->width = (int)t->width;
    o->codecpar->height = (int)t->height;
    vobsub_extradata(o->codecpar, t);
    o->time_base = (AVRational){1, TICKS};
    av_dict_set(&o->metadata, "language", lang3(sub->lang), 0);
    if (sub->extension == 9) {
      o->disposition |= AV_DISPOSITION_FORCED;
    }
    if (sin) {
      j->map[sin->index] = o->index;
    }  // else matched by its id when it turns up (below)
  }
  add_chapters(j->out, t);
  av_dict_set(&j->out->metadata, "title", "", 0);
  rc = avio_open(&j->out->pb, path, AVIO_FLAG_WRITE);
  if (rc >= 0) {
    rc = avformat_write_header(j->out, NULL);
  }
  while (rc >= 0) {
    AVStream *in_st;
    int oi;
    rc = av_read_frame(j->in, pkt);
    if (rc < 0) {
      break;
    }
    in_st = j->in->streams[pkt->stream_index];
    oi = pkt->stream_index < (int)(sizeof(j->map) / sizeof(j->map[0])) ? j->map[pkt->stream_index] : -1;
    if (oi < 0 && in_st->codecpar->codec_type == AVMEDIA_TYPE_SUBTITLE) {
      // A subtitle stream that turned up after the probe: by its id.
      unsigned k;
      int out_index = 1;
      for (k = 0; k < t->audio_count; k++) {
        out_index += t->audio[k].format != DVD_AUDIO_LPCM && t->audio[k].format != DVD_AUDIO_OTHER;
      }
      for (k = 0; k < t->sub_count; k++) {
        if (t->subs[k].stream_id == in_st->id) {
          oi = out_index + (int)k;
          if (pkt->stream_index < (int)(sizeof(j->map) / sizeof(j->map[0]))) {
            j->map[pkt->stream_index] = oi;
          }
        }
      }
    }
    if (oi < 0 || fix_ts(&j->clock, pkt, in_st)) {
      av_packet_unref(pkt);
      continue;
    }
    av_packet_rescale_ts(pkt, in_st->time_base, j->out->streams[oi]->time_base);
    pkt->stream_index = oi;
    pkt->pos = -1;
    if (in_st->codecpar->codec_id == AV_CODEC_ID_MPEG2VIDEO || in_st->codecpar->codec_id == AV_CODEC_ID_MPEG1VIDEO) {
      if (!fill.step) {
        fill.step = av_rescale_q(1, (AVRational){1001, 30000}, j->out->streams[oi]->time_base);
        fill.step = fill.step > 0 ? fill.step : 1;
      }
      pts_video(j, &fill, pkt);
    } else {
      if (pkt->pts == AV_NOPTS_VALUE) {
        pkt->pts = pkt->dts;
      }
      write_packet(j, pkt);
    }
    av_packet_unref(pkt);
  }
  pts_flush(j, &fill, 1);
  av_packet_free(&pkt);
  if (rc == AVERROR_EOF) {
    rc = 0;
  }
  if (rc >= 0 || j->out->pb) {
    int t2 = av_write_trailer(j->out);
    if (rc >= 0) {
      rc = t2;
    }
  }
  return rc;
}

/* MP4: H.264 and AAC */

struct venc {
  AVCodecContext *dec;
  AVCodecContext *enc;
  AVFilterGraph *graph;
  AVFilterContext *src;
  AVFilterContext *sink;
  AVFrame *frame;
  AVFrame *filtered;
  int out_index;
  AVRational in_tb;
  int64_t last_pts;  // in the encoder's time base: a frame without one follows it
};

struct aenc {
  AVCodecContext *dec;
  AVCodecContext *enc;
  SwrContext *swr;
  AVAudioFifo *fifo;
  AVFrame *frame;
  int64_t next_pts;
  int64_t first_pts;
  int have_first;
  int out_index;
  int in_index;
};

static int send_encoded(AVFormatContext *out, AVCodecContext *enc, int index, AVPacket *pkt) {
  int rc;

  while ((rc = avcodec_receive_packet(enc, pkt)) >= 0) {
    av_packet_rescale_ts(pkt, enc->time_base, out->streams[index]->time_base);
    pkt->stream_index = index;
    if (index < 8 && pkt->dts != AV_NOPTS_VALUE) {
      g_stream_at[index] = (double)pkt->dts * av_q2d(out->streams[index]->time_base);
    }
    rc = av_interleaved_write_frame(out, pkt);
    if (rc < 0) {
      return rc;
    }
  }
  return rc == AVERROR(EAGAIN) || rc == AVERROR_EOF ? 0 : rc;
}

static int video_setup(struct job *j, AVStream *vin, struct venc *v, const struct remux_opts *opts) {
  const struct dvd_title *t = j->src.t;
  const AVCodec *dec = avcodec_find_decoder(vin->codecpar->codec_id);
  const AVCodec *enc = avcodec_find_encoder_by_name("libx264");
  AVRational sar = sample_aspect(t);
  AVStream *o;
  char args[256];
  int rc;

  if (!dec || !enc) {
    RIP_ERROR("remux: no %s here", !dec ? "MPEG-2 decoder" : "x264 encoder");
    return AVERROR_ENCODER_NOT_FOUND;
  }
  v->dec = avcodec_alloc_context3(dec);
  avcodec_parameters_to_context(v->dec, vin->codecpar);
  v->dec->pkt_timebase = vin->time_base;
  rc = avcodec_open2(v->dec, dec, NULL);
  if (rc < 0) {
    return rc;
  }
  v->in_tb = vin->time_base;
  // yadif only touches frames flagged interlaced; film pulled down on the
  // disc decodes progressive and passes through.
  v->graph = avfilter_graph_alloc();
  snprintf(args, sizeof(args), "video_size=%dx%d:pix_fmt=%d:time_base=%d/%d:pixel_aspect=%d/%d", v->dec->width,
           v->dec->height, v->dec->pix_fmt, vin->time_base.num, vin->time_base.den, sar.num, sar.den);
  rc = avfilter_graph_create_filter(&v->src, avfilter_get_by_name("buffer"), "in", args, NULL, v->graph);
  if (rc >= 0) {
    rc = avfilter_graph_create_filter(&v->sink, avfilter_get_by_name("buffersink"), "out", NULL, NULL, v->graph);
  }
  if (rc >= 0) {
    AVFilterInOut *outputs = avfilter_inout_alloc();
    AVFilterInOut *inputs = avfilter_inout_alloc();
    outputs->name = av_strdup("in");
    outputs->filter_ctx = v->src;
    inputs->name = av_strdup("out");
    inputs->filter_ctx = v->sink;
    rc = avfilter_graph_parse_ptr(v->graph, "yadif=mode=send_frame:deint=interlaced,format=yuv420p", &inputs, &outputs,
                                  NULL);
    avfilter_inout_free(&inputs);
    avfilter_inout_free(&outputs);
  }
  if (rc >= 0) {
    rc = avfilter_graph_config(v->graph, NULL);
  }
  if (rc < 0) {
    return rc;
  }
  v->enc = avcodec_alloc_context3(enc);
  v->enc->width = v->dec->width;
  v->enc->height = v->dec->height;
  v->enc->pix_fmt = AV_PIX_FMT_YUV420P;
  v->enc->sample_aspect_ratio = sar;
  v->enc->time_base = (AVRational){1, TICKS};
  v->enc->framerate = t->pal ? (AVRational){25, 1} : (AVRational){30000, 1001};
  v->enc->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
  // x264 would start one and a half threads a CPU; twelve keep the console's
  // 16 busy (ripper joins all of them, platform.c) without a pile of buffers.
  v->enc->thread_count = 12;
  av_opt_set(v->enc->priv_data, "preset", opts->preset ? opts->preset : "veryfast", 0);
  av_opt_set_int(v->enc->priv_data, "crf", opts->crf ? opts->crf : 20, 0);
  av_opt_set(v->enc->priv_data, "profile", "high", 0);
  rc = avcodec_open2(v->enc, enc, NULL);
  if (rc < 0) {
    return rc;
  }
  o = avformat_new_stream(j->out, NULL);
  avcodec_parameters_from_context(o->codecpar, v->enc);
  o->time_base = v->enc->time_base;
  o->sample_aspect_ratio = sar;
  v->out_index = o->index;
  v->frame = av_frame_alloc();
  v->filtered = av_frame_alloc();
  return v->frame && v->filtered ? 0 : AVERROR(ENOMEM);
}

static int audio_setup(struct job *j, AVStream *ain, const struct dvd_audio *a, struct aenc *e) {
  const AVCodec *dec = avcodec_find_decoder(ain->codecpar->codec_id);
  const AVCodec *enc = avcodec_find_encoder(AV_CODEC_ID_AAC);
  AVStream *o;
  int rc;

  if (!dec || !enc) {
    return AVERROR_DECODER_NOT_FOUND;
  }
  e->dec = avcodec_alloc_context3(dec);
  avcodec_parameters_to_context(e->dec, ain->codecpar);
  e->dec->pkt_timebase = ain->time_base;
  rc = avcodec_open2(e->dec, dec, NULL);
  if (rc < 0) {
    return rc;
  }
  e->enc = avcodec_alloc_context3(enc);
  e->enc->sample_rate = e->dec->sample_rate ? e->dec->sample_rate : 48000;
  av_channel_layout_copy(&e->enc->ch_layout, &e->dec->ch_layout);
  if (!e->enc->ch_layout.nb_channels) {
    av_channel_layout_default(&e->enc->ch_layout, (int)a->channels);
  }
  e->enc->sample_fmt = AV_SAMPLE_FMT_FLTP;
  e->enc->bit_rate = 64000 * e->enc->ch_layout.nb_channels;
  e->enc->time_base = (AVRational){1, e->enc->sample_rate};
  e->enc->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
  rc = avcodec_open2(e->enc, enc, NULL);
  if (rc < 0) {
    return rc;
  }
  rc = swr_alloc_set_opts2(&e->swr, &e->enc->ch_layout, e->enc->sample_fmt, e->enc->sample_rate, &e->dec->ch_layout,
                           e->dec->sample_fmt, e->dec->sample_rate, 0, NULL);
  if (rc < 0 || swr_init(e->swr) < 0) {
    return rc < 0 ? rc : AVERROR(EINVAL);
  }
  e->fifo = av_audio_fifo_alloc(e->enc->sample_fmt, e->enc->ch_layout.nb_channels, e->enc->frame_size * 4);
  e->frame = av_frame_alloc();
  o = avformat_new_stream(j->out, NULL);
  avcodec_parameters_from_context(o->codecpar, e->enc);
  o->time_base = e->enc->time_base;
  av_dict_set(&o->metadata, "language", lang3(a->lang), 0);
  e->out_index = o->index;
  e->in_index = ain->index;
  return e->fifo && e->frame ? 0 : AVERROR(ENOMEM);
}

// A filtered frame's time in the encoder's time base. One that has none (the
// filter's last, at the end) or would go backwards follows the one before.
static void frame_pts(struct venc *v, AVFrame *f) {
  int64_t step = av_rescale_q(1, av_inv_q(v->enc->framerate), v->enc->time_base);
  if (f->pts != AV_NOPTS_VALUE) {
    f->pts = av_rescale_q(f->pts, av_buffersink_get_time_base(v->sink), v->enc->time_base);
  }
  if (f->pts == AV_NOPTS_VALUE || (v->last_pts != AV_NOPTS_VALUE && f->pts <= v->last_pts)) {
    f->pts = v->last_pts == AV_NOPTS_VALUE ? 0 : v->last_pts + (step > 0 ? step : 1);
  }
  v->last_pts = f->pts;
}

static int video_frames(struct job *j, struct venc *v, AVPacket *out_pkt, int flush) {
  int rc;

  while ((rc = avcodec_receive_frame(v->dec, v->frame)) >= 0) {
    v->frame->pts = v->frame->best_effort_timestamp;
    rc = av_buffersrc_add_frame_flags(v->src, v->frame, AV_BUFFERSRC_FLAG_KEEP_REF);
    av_frame_unref(v->frame);
    if (rc < 0) {
      return rc;
    }
    while ((rc = av_buffersink_get_frame(v->sink, v->filtered)) >= 0) {
      frame_pts(v, v->filtered);
      v->filtered->pict_type = AV_PICTURE_TYPE_NONE;
      rc = avcodec_send_frame(v->enc, v->filtered);
      av_frame_unref(v->filtered);
      if (rc < 0) {
        return rc;
      }
      rc = send_encoded(j->out, v->enc, v->out_index, out_pkt);
      if (rc < 0) {
        return rc;
      }
    }
  }
  if (flush) {
    if (av_buffersrc_add_frame(v->src, NULL) < 0) {
      return AVERROR(EINVAL);
    }
    while (av_buffersink_get_frame(v->sink, v->filtered) >= 0) {
      frame_pts(v, v->filtered);
      avcodec_send_frame(v->enc, v->filtered);
      av_frame_unref(v->filtered);
      send_encoded(j->out, v->enc, v->out_index, out_pkt);
    }
    avcodec_send_frame(v->enc, NULL);
    return send_encoded(j->out, v->enc, v->out_index, out_pkt);
  }
  return rc == AVERROR(EAGAIN) || rc == AVERROR_EOF ? 0 : rc;
}

// Decoded audio through the resampler into the FIFO, out in AAC-sized frames.
static int audio_frames(struct job *j, struct aenc *e, AVPacket *out_pkt, int flush) {
  AVFrame *in = av_frame_alloc();
  int rc;

  if (!in) {
    return AVERROR(ENOMEM);
  }
  while ((rc = avcodec_receive_frame(e->dec, in)) >= 0) {
    uint8_t **data = NULL;
    int n = swr_get_out_samples(e->swr, in->nb_samples);
    if (!e->have_first && in->best_effort_timestamp != AV_NOPTS_VALUE) {
      e->first_pts = av_rescale_q(in->best_effort_timestamp, e->dec->pkt_timebase, e->enc->time_base);
      e->next_pts = e->first_pts > 0 ? e->first_pts : 0;
      e->have_first = 1;
    }
    if (av_samples_alloc_array_and_samples(&data, NULL, e->enc->ch_layout.nb_channels, n, e->enc->sample_fmt, 0) >= 0) {
      n = swr_convert(e->swr, data, n, (const uint8_t **)in->extended_data, in->nb_samples);
      if (n > 0 && av_audio_fifo_write(e->fifo, (void **)data, n) < n) {
        RIP_WARN("remux: audio dropped (out of memory)");
      }
      av_freep(&data[0]);
      av_freep(&data);
    }
    av_frame_unref(in);
  }
  av_frame_free(&in);
  while (av_audio_fifo_size(e->fifo) >= e->enc->frame_size || (flush && av_audio_fifo_size(e->fifo) > 0)) {
    int n = av_audio_fifo_size(e->fifo) < e->enc->frame_size ? av_audio_fifo_size(e->fifo) : e->enc->frame_size;
    e->frame->nb_samples = n;
    e->frame->format = e->enc->sample_fmt;
    e->frame->sample_rate = e->enc->sample_rate;
    av_channel_layout_copy(&e->frame->ch_layout, &e->enc->ch_layout);
    if (av_frame_get_buffer(e->frame, 0) < 0) {
      return AVERROR(ENOMEM);
    }
    av_audio_fifo_read(e->fifo, (void **)e->frame->data, n);
    e->frame->pts = e->next_pts;
    e->next_pts += n;
    rc = avcodec_send_frame(e->enc, e->frame);
    av_frame_unref(e->frame);
    if (rc < 0) {
      return rc;
    }
    rc = send_encoded(j->out, e->enc, e->out_index, out_pkt);
    if (rc < 0) {
      return rc;
    }
  }
  if (flush) {
    avcodec_send_frame(e->enc, NULL);
    return send_encoded(j->out, e->enc, e->out_index, out_pkt);
  }
  return 0;
}

static int write_mp4(struct job *j, const char *path, const struct remux_opts *opts) {
  const struct dvd_title *t = j->src.t;
  AVStream *vin = find_in(j->in, 0x1E0);
  AVStream *ain = NULL;
  const struct dvd_audio *a = NULL;
  struct venc v;
  struct aenc e;
  AVPacket *pkt = av_packet_alloc();
  AVPacket *out_pkt = av_packet_alloc();
  AVDictionary *mux = NULL;
  unsigned i;
  int rc;

  memset(&v, 0, sizeof(v));
  memset(&e, 0, sizeof(e));
  v.last_pts = AV_NOPTS_VALUE;
  e.in_index = -1;
  if (!pkt || !out_pkt || !vin) {
    rc = vin ? AVERROR(ENOMEM) : AVERROR_INVALIDDATA;
    goto done;
  }
  // The first audio track that isn't a commentary and the demuxer found.
  for (i = 0; i < t->audio_count && !ain; i++) {
    if (t->audio[i].extension == 3 || t->audio[i].extension == 4) {
      continue;
    }
    ain = find_in(j->in, ps_id(t->audio[i].stream_id));
    a = &t->audio[i];
  }
  rc = avformat_alloc_output_context2(&j->out, NULL, "mp4", path);
  if (rc < 0) {
    goto done;
  }
  rc = video_setup(j, vin, &v, opts);
  if (rc >= 0 && ain) {
    rc = audio_setup(j, ain, a, &e);
  }
  if (rc < 0) {
    goto done;
  }
  add_chapters(j->out, t);
  av_dict_set(&mux, "movflags", "+faststart", 0);
  rc = avio_open(&j->out->pb, path, AVIO_FLAG_WRITE);
  if (rc >= 0) {
    rc = avformat_write_header(j->out, &mux);
  }
  while (rc >= 0) {
    AVStream *st;
    rc = av_read_frame(j->in, pkt);
    if (rc < 0) {
      break;
    }
    st = j->in->streams[pkt->stream_index];
    if ((st->index != vin->index && st->index != e.in_index) || fix_ts(&j->clock, pkt, st)) {
      av_packet_unref(pkt);
      continue;
    }
    if (st->index == vin->index) {
      rc = avcodec_send_packet(v.dec, pkt);
      av_packet_unref(pkt);
      rc = rc < 0 && rc != AVERROR_INVALIDDATA ? rc : video_frames(j, &v, out_pkt, 0);
    } else {
      rc = avcodec_send_packet(e.dec, pkt);
      av_packet_unref(pkt);
      rc = rc < 0 && rc != AVERROR_INVALIDDATA ? rc : audio_frames(j, &e, out_pkt, 0);
    }
  }
  if (rc == AVERROR_EOF) {
    rc = 0;
  }
  // Cancelled: what's still in the encoder is thrown away, not drained.
  if (rc >= 0 && j->src.stop != 1) {
    avcodec_send_packet(v.dec, NULL);
    rc = video_frames(j, &v, out_pkt, 1);
    if (rc >= 0 && e.dec) {
      avcodec_send_packet(e.dec, NULL);
      rc = audio_frames(j, &e, out_pkt, 1);
    }
  }
  if (j->out && j->out->pb) {
    int t2 = av_write_trailer(j->out);
    if (rc >= 0) {
      rc = t2;
    }
  }

done:
  av_dict_free(&mux);
  av_packet_free(&pkt);
  av_packet_free(&out_pkt);
  avcodec_free_context(&v.dec);
  avcodec_free_context(&v.enc);
  avfilter_graph_free(&v.graph);
  av_frame_free(&v.frame);
  av_frame_free(&v.filtered);
  avcodec_free_context(&e.dec);
  avcodec_free_context(&e.enc);
  swr_free(&e.swr);
  if (e.fifo) {
    av_audio_fifo_free(e.fifo);
  }
  av_frame_free(&e.frame);
  return rc;
}

int remux_available(enum remux_format format) {
  if (format == REMUX_MP4) {
    return avcodec_find_encoder_by_name("libx264") != NULL;
  }
  return av_guess_format("matroska", NULL, NULL) != NULL;
}

int remux_title(struct rip_reader *r, const struct dvd_title *t, const char *out_path, const struct remux_opts *opts) {
  struct job *j = calloc(1, sizeof(*j));
  char e[128];
  int rc;

  if (!j) {
    return RIP_ENOMEM;
  }
  j->src.r = r;
  j->src.t = t;
  j->src.lba = t->cell_count ? t->cells[0].first : 0;
  j->src.total = t->bytes;
  j->src.progress = opts->progress;
  j->src.ud = opts->ud;
  memset(g_stream_at, 0, sizeof(g_stream_at));
  if (src_start(&j->src) != 0) {
    free(j);
    return RIP_ENOMEM;
  }
  av_log_set_level(AV_LOG_ERROR);
  RIP_INFO("remux: title %u, %u:%02u:%02u, %.1f GB, to %s", t->number, t->ms / 3600000, t->ms / 60000 % 60,
           t->ms / 1000 % 60, (double)t->bytes / 1e9, opts->format == REMUX_MP4 ? "MP4 (H.264, AAC)" : "MKV");
  rc = clock_init(&j->clock, t);
  if (rc >= 0) {
    rc = open_input(j);
  }
  if (rc >= 0) {
    rc = opts->format == REMUX_MP4 ? write_mp4(j, out_path, opts) : write_mkv(j, out_path);
  }
  close_job(j);
  src_stop(&j->src);
  if (j->src.bad) {
    RIP_WARN("remux: %llu sector(s) couldn't be read and were left out", (unsigned long long)j->src.bad);
  }
  clock_free(&j->clock);
  if (j->src.stop == 1) {
    remove(out_path);
    free(j);
    return RIP_ECANCEL;
  }
  if (rc < 0) {
    RIP_ERROR("remux: %s", err_str(rc, e, sizeof(e)));
    remove(out_path);
    free(j);
    return RIP_ERR;
  }
  free(j);
  return RIP_OK;
}

#else

int remux_available(enum remux_format format) {
  (void)format;
  return 0;
}

int remux_title(struct rip_reader *r, const struct dvd_title *t, const char *out_path, const struct remux_opts *opts) {
  (void)r;
  (void)t;
  (void)out_path;
  (void)opts;
  RIP_ERROR("remux: this build has no FFmpeg");
  return RIP_ENOTSUP;
}

#endif
