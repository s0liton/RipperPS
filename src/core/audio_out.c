#include "ripper/audio_out.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "ripper/cdrom.h"
#include "ripper/common.h"
#include "ripper/log.h"
#include "ripper/util.h"

#define SAMPLES_PER_FRAME 588  // stereo samples in a 2352-byte frame

struct audio_out {
  enum audio_codec codec;
  char path[1100];
  void *impl;
};

const char *audio_out_ext(enum audio_codec codec) {
  switch (codec) {
  case AUDIO_FLAC:
    return ".flac";
  case AUDIO_MP3:
    return ".mp3";
  case AUDIO_AAC:
  case AUDIO_ALAC:
    return ".m4a";
  case AUDIO_OPUS:
    return ".opus";
  default:
    return ".wav";
  }
}

#if defined(RIPPER_HAVE_FLAC) || defined(RIPPER_HAVE_FFMPEG)
static int is_png(const uint8_t *b, size_t n) {
  return n >= 8 && !memcmp(b, "\x89PNG", 4);
}
#endif

/* FLAC */

#ifdef RIPPER_HAVE_FLAC

#include <FLAC/metadata.h>
#include <FLAC/stream_encoder.h>

struct flac {
  FLAC__StreamEncoder *enc;
  FLAC__StreamMetadata *meta[3];
  unsigned nmeta;
  FLAC__int32 *buf;
};

static void flac_tag(FLAC__StreamMetadata *m, const char *name, const char *value) {
  FLAC__StreamMetadata_VorbisComment_Entry e;

  if (!value || !value[0]) {
    return;
  }
  if (FLAC__metadata_object_vorbiscomment_entry_from_name_value_pair(&e, name, value)) {
    FLAC__metadata_object_vorbiscomment_append_comment(m, e, false);
  }
}

static void flac_free(struct flac *f) {
  unsigned i;

  if (f->enc) {
    FLAC__stream_encoder_delete(f->enc);
  }
  for (i = 0; i < f->nmeta; i++) {
    FLAC__metadata_object_delete(f->meta[i]);
  }
  free(f->buf);
  free(f);
}

static int flac_open(struct audio_out *o, uint64_t frames, const struct audio_settings *s, const struct audio_tags *t) {
  struct flac *f = calloc(1, sizeof(*f));
  FLAC__StreamMetadata *tags;
  FLAC__StreamMetadata *padding;
  char num[16];
  int level = s->quality;

  if (!f) {
    return RIP_ENOMEM;
  }
  f->enc = FLAC__stream_encoder_new();
  f->buf = malloc(sizeof(FLAC__int32) * SAMPLES_PER_FRAME * 2 * 64);
  tags = FLAC__metadata_object_new(FLAC__METADATA_TYPE_VORBIS_COMMENT);
  padding = FLAC__metadata_object_new(FLAC__METADATA_TYPE_PADDING);
  if (tags) {
    f->meta[f->nmeta++] = tags;
  }
  if (padding) {
    f->meta[f->nmeta++] = padding;
  }
  if (!f->enc || !f->buf || !tags || !padding) {
    flac_free(f);
    return RIP_ENOMEM;
  }
  if (t) {
    flac_tag(tags, "ARTIST", t->artist);
    flac_tag(tags, "ALBUMARTIST", t->artist);
    flac_tag(tags, "ALBUM", t->album);
    flac_tag(tags, "TITLE", t->title);
    flac_tag(tags, "DATE", t->date);
    flac_tag(tags, "MUSICBRAINZ_DISCID", t->mb_discid);
    if (t->track) {
      snprintf(num, sizeof(num), "%u", t->track);
      flac_tag(tags, "TRACKNUMBER", num);
    }
    if (t->total) {
      snprintf(num, sizeof(num), "%u", t->total);
      flac_tag(tags, "TRACKTOTAL", num);
    }
  }
  if (s->cover && s->cover_len) {
    FLAC__StreamMetadata *pic = FLAC__metadata_object_new(FLAC__METADATA_TYPE_PICTURE);
    if (pic) {
      pic->data.picture.type = FLAC__STREAM_METADATA_PICTURE_TYPE_FRONT_COVER;
      if (FLAC__metadata_object_picture_set_mime_type(pic, is_png(s->cover, s->cover_len) ? "image/png" : "image/jpeg",
                                                      true) &&
          FLAC__metadata_object_picture_set_data(pic, (FLAC__byte *)s->cover, (FLAC__uint32)s->cover_len, true)) {
        f->meta[f->nmeta++] = pic;
      } else {
        FLAC__metadata_object_delete(pic);
      }
    }
  }
  padding->length = 4096;  // room to retag without rewriting the file
  FLAC__stream_encoder_set_channels(f->enc, 2);
  FLAC__stream_encoder_set_bits_per_sample(f->enc, 16);
  FLAC__stream_encoder_set_sample_rate(f->enc, 44100);
  FLAC__stream_encoder_set_compression_level(f->enc, (unsigned)(level < 0 ? 5 : level > 8 ? 8 : level));
  FLAC__stream_encoder_set_total_samples_estimate(f->enc, frames * SAMPLES_PER_FRAME);
  FLAC__stream_encoder_set_metadata(f->enc, f->meta, f->nmeta);
  if (FLAC__stream_encoder_init_file(f->enc, o->path, NULL, NULL) != FLAC__STREAM_ENCODER_INIT_STATUS_OK) {
    RIP_ERROR("flac: can't create %s", o->path);
    flac_free(f);
    return RIP_ERR;
  }
  o->impl = f;
  return RIP_OK;
}

static int flac_write(struct audio_out *o, const uint8_t *pcm, uint32_t frames) {
  struct flac *f = o->impl;

  while (frames) {
    uint32_t n = frames < 64 ? frames : 64;
    uint32_t i;
    for (i = 0; i < n * SAMPLES_PER_FRAME * 2; i++) {
      f->buf[i] = (int16_t)rip_le16(pcm + i * 2);
    }
    if (!FLAC__stream_encoder_process_interleaved(f->enc, f->buf, n * SAMPLES_PER_FRAME)) {
      RIP_ERROR("flac: encoding %s failed: %s", o->path,
                FLAC__StreamEncoderStateString[FLAC__stream_encoder_get_state(f->enc)]);
      return RIP_ERR;
    }
    pcm += (size_t)n * CD_FRAME;
    frames -= n;
  }
  return RIP_OK;
}

static int flac_close(struct audio_out *o, int ok) {
  struct flac *f = o->impl;
  int rc = ok ? RIP_OK : RIP_ERR;

  if (!FLAC__stream_encoder_finish(f->enc) && ok) {
    rc = RIP_ERR;
  }
  flac_free(f);
  return rc;
}

#endif

/* MP3 */

#ifdef RIPPER_HAVE_LAME

#include <lame/lame.h>

struct mp3 {
  lame_t gf;
  FILE *f;
  unsigned char *buf;
  size_t cap;
};

static int mp3_open(struct audio_out *o, const struct audio_settings *s, const struct audio_tags *t) {
  struct mp3 *m = calloc(1, sizeof(*m));
  int q = s->quality;

  if (!m || !(m->gf = lame_init())) {
    free(m);
    return RIP_ENOMEM;
  }
  m->cap = (size_t)(1.25 * SAMPLES_PER_FRAME * 64) + 7200;
  m->buf = malloc(m->cap);
  m->f = fopen(o->path, "wb+");
  if (!m->buf || !m->f) {
    RIP_ERROR("mp3: can't create %s", o->path);
    if (m->f) {
      fclose(m->f);
    }
    free(m->buf);
    lame_close(m->gf);
    free(m);
    return RIP_ERR;
  }
  lame_set_in_samplerate(m->gf, 44100);
  lame_set_out_samplerate(m->gf, 44100);
  lame_set_num_channels(m->gf, 2);
  if (q >= 32) {
    lame_set_VBR(m->gf, vbr_off);
    lame_set_brate(m->gf, q);
  } else {
    lame_set_VBR(m->gf, vbr_default);
    lame_set_VBR_quality(m->gf, (float)(q < 0 ? 2 : q > 9 ? 9 : q));
  }
  id3tag_init(m->gf);
  id3tag_v2_only(m->gf);
  if (t) {
    char num[24];
    if (t->title && t->title[0]) {
      id3tag_set_title(m->gf, t->title);
    }
    if (t->artist && t->artist[0]) {
      id3tag_set_artist(m->gf, t->artist);
      id3tag_set_fieldvalue(m->gf, "TPE2=");  // cleared, then set below as the album artist
    }
    if (t->album && t->album[0]) {
      id3tag_set_album(m->gf, t->album);
    }
    if (t->date && t->date[0]) {
      char year[8];
      snprintf(year, sizeof(year), "%.4s", t->date);
      id3tag_set_year(m->gf, year);
    }
    if (t->track) {
      snprintf(num, sizeof(num), t->total ? "%u/%u" : "%u", t->track, t->total);
      id3tag_set_track(m->gf, num);
    }
    if (t->artist && t->artist[0]) {
      char tpe2[300];
      snprintf(tpe2, sizeof(tpe2), "TPE2=%s", t->artist);
      id3tag_set_fieldvalue(m->gf, tpe2);
    }
  }
  if (s->cover && s->cover_len) {
    id3tag_set_albumart(m->gf, (const char *)s->cover, s->cover_len);
  }
  if (lame_init_params(m->gf) < 0) {
    RIP_ERROR("mp3: the encoder refused its settings");
    fclose(m->f);
    unlink(o->path);
    free(m->buf);
    lame_close(m->gf);
    free(m);
    return RIP_ERR;
  }
  o->impl = m;
  return RIP_OK;
}

static int mp3_write(struct audio_out *o, const uint8_t *pcm, uint32_t frames) {
  struct mp3 *m = o->impl;

  while (frames) {
    uint32_t n = frames < 64 ? frames : 64;
    // CD audio is little-endian, as the console and PCs are.
    int got = lame_encode_buffer_interleaved(m->gf, (short *)(uintptr_t)pcm, (int)(n * SAMPLES_PER_FRAME), m->buf,
                                             (int)m->cap);
    if (got < 0 || (got > 0 && fwrite(m->buf, 1, (size_t)got, m->f) != (size_t)got)) {
      RIP_ERROR("mp3: writing %s failed", o->path);
      return RIP_ERR;
    }
    pcm += (size_t)n * CD_FRAME;
    frames -= n;
  }
  return RIP_OK;
}

static int mp3_close(struct audio_out *o, int ok) {
  struct mp3 *m = o->impl;
  int rc = ok ? RIP_OK : RIP_ERR;

  if (ok) {
    int got = lame_encode_flush(m->gf, m->buf, (int)m->cap);
    if (got < 0 || (got > 0 && fwrite(m->buf, 1, (size_t)got, m->f) != (size_t)got)) {
      rc = RIP_ERR;
    }
    // The Xing/LAME frame at the start, so players know the length and seek right.
    lame_mp3_tags_fid(m->gf, m->f);
  }
  if (fclose(m->f) != 0) {
    rc = RIP_ERR;
  }
  lame_close(m->gf);
  free(m->buf);
  free(m);
  return rc;
}

#endif

/* AAC, ALAC (.m4a) and Opus, through FFmpeg */

#ifdef RIPPER_HAVE_FFMPEG

#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/audio_fifo.h>
#include <libavutil/channel_layout.h>
#include <libswresample/swresample.h>

struct ff {
  AVFormatContext *fmt;
  AVCodecContext *enc;
  AVStream *st;
  SwrContext *swr;
  AVAudioFifo *fifo;
  AVFrame *frame;
  AVPacket *pkt;
  int frame_size;
  int64_t pts;
};

static const char *ff_encoder(enum audio_codec c) {
  return c == AUDIO_AAC ? "aac" : c == AUDIO_ALAC ? "alac" : "opus";
}

static void ff_free(struct ff *f) {
  if (f->fmt) {
    if (f->fmt->pb) {
      avio_closep(&f->fmt->pb);
    }
    avformat_free_context(f->fmt);
  }
  avcodec_free_context(&f->enc);
  swr_free(&f->swr);
  if (f->fifo) {
    av_audio_fifo_free(f->fifo);
  }
  av_frame_free(&f->frame);
  av_packet_free(&f->pkt);
  free(f);
}

// A JPEG's or PNG's size from its header: the muxer wants it for a cover.
static void image_size(const uint8_t *b, size_t n, int *w, int *h) {
  size_t i = 2;

  *w = *h = 0;
  if (is_png(b, n) && n >= 24) {
    *w = (int)((uint32_t)b[16] << 24 | (uint32_t)b[17] << 16 | (uint32_t)b[18] << 8 | b[19]);
    *h = (int)((uint32_t)b[20] << 24 | (uint32_t)b[21] << 16 | (uint32_t)b[22] << 8 | b[23]);
    return;
  }
  // JPEG: the markers up to a start of frame (C0-CF but C4, C8 and CC).
  while (n >= 4 && i + 9 < n && b[i] == 0xFF) {
    uint8_t m = b[i + 1];
    size_t len = (size_t)b[i + 2] << 8 | b[i + 3];
    if (m >= 0xC0 && m <= 0xCF && m != 0xC4 && m != 0xC8 && m != 0xCC) {
      *h = b[i + 5] << 8 | b[i + 6];
      *w = b[i + 7] << 8 | b[i + 8];
      return;
    }
    i += 2 + len;
  }
}

static void ff_tag(AVDictionary **d, const char *k, const char *v) {
  if (v && v[0]) {
    av_dict_set(d, k, v, 0);
  }
}

// What the encoder has ready, into the file.
static int ff_drain(struct ff *f) {
  int rc;

  while ((rc = avcodec_receive_packet(f->enc, f->pkt)) >= 0) {
    av_packet_rescale_ts(f->pkt, f->enc->time_base, f->st->time_base);
    f->pkt->stream_index = f->st->index;
    rc = av_interleaved_write_frame(f->fmt, f->pkt);
    if (rc < 0) {
      return rc;
    }
  }
  return rc == AVERROR(EAGAIN) || rc == AVERROR_EOF ? 0 : rc;
}

static int ff_encode(struct ff *f, int final) {
  int rc;

  while (av_audio_fifo_size(f->fifo) >= f->frame_size || (final && av_audio_fifo_size(f->fifo) > 0)) {
    int n = av_audio_fifo_size(f->fifo) < f->frame_size ? av_audio_fifo_size(f->fifo) : f->frame_size;
    f->frame->nb_samples = n;
    f->frame->format = f->enc->sample_fmt;
    f->frame->sample_rate = f->enc->sample_rate;
    av_channel_layout_copy(&f->frame->ch_layout, &f->enc->ch_layout);
    if (av_frame_get_buffer(f->frame, 0) < 0) {
      return AVERROR(ENOMEM);
    }
    av_audio_fifo_read(f->fifo, (void **)f->frame->data, n);
    f->frame->pts = f->pts;
    f->pts += n;
    rc = avcodec_send_frame(f->enc, f->frame);
    av_frame_unref(f->frame);
    if (rc < 0 || (rc = ff_drain(f)) < 0) {
      return rc;
    }
  }
  return 0;
}

static int ff_open(struct audio_out *o, const struct audio_settings *s, const struct audio_tags *t) {
  struct ff *f = calloc(1, sizeof(*f));
  const AVCodec *codec = avcodec_find_encoder_by_name(ff_encoder(o->codec));
  AVChannelLayout stereo = AV_CHANNEL_LAYOUT_STEREO;
  char num[24];
  int rc;

  if (!f) {
    return RIP_ENOMEM;
  }
  if (!codec || avformat_alloc_output_context2(&f->fmt, NULL, o->codec == AUDIO_OPUS ? "opus" : "ipod", o->path) < 0) {
    RIP_ERROR("%s: not in this build", ff_encoder(o->codec));
    ff_free(f);
    return RIP_ENOTSUP;
  }
  f->enc = avcodec_alloc_context3(codec);
  f->frame = av_frame_alloc();
  f->pkt = av_packet_alloc();
  if (!f->enc || !f->frame || !f->pkt) {
    ff_free(f);
    return RIP_ENOMEM;
  }
  f->enc->sample_rate = o->codec == AUDIO_OPUS ? 48000 : 44100;
  av_channel_layout_copy(&f->enc->ch_layout, &stereo);
  f->enc->sample_fmt = codec->sample_fmts ? codec->sample_fmts[0] : AV_SAMPLE_FMT_S16;
  if (o->codec != AUDIO_ALAC) {
    f->enc->bit_rate = (int64_t)(s->quality > 0 ? s->quality : o->codec == AUDIO_OPUS ? 160 : 256) * 1000;
  }
  f->enc->time_base = (AVRational){1, f->enc->sample_rate};
  f->enc->strict_std_compliance = FF_COMPLIANCE_EXPERIMENTAL;  // FFmpeg's own Opus encoder
  if (f->fmt->oformat->flags & AVFMT_GLOBALHEADER) {
    f->enc->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
  }
  rc = avcodec_open2(f->enc, codec, NULL);
  f->st = rc >= 0 ? avformat_new_stream(f->fmt, NULL) : NULL;
  if (!f->st || avcodec_parameters_from_context(f->st->codecpar, f->enc) < 0) {
    RIP_ERROR("%s: the encoder wouldn't start", ff_encoder(o->codec));
    ff_free(f);
    return RIP_ERR;
  }
  f->st->time_base = f->enc->time_base;
  f->frame_size = f->enc->frame_size > 0 ? f->enc->frame_size : 4096;
  if (t) {
    // On the file for .m4a, on the stream for Ogg (its comments are the stream's).
    AVDictionary **d = o->codec == AUDIO_OPUS ? &f->st->metadata : &f->fmt->metadata;
    ff_tag(d, "title", t->title);
    ff_tag(d, "artist", t->artist);
    ff_tag(d, "album_artist", t->artist);
    ff_tag(d, "album", t->album);
    ff_tag(d, "date", t->date);
    if (t->track) {
      snprintf(num, sizeof(num), t->total ? "%u/%u" : "%u", t->track, t->total);
      ff_tag(d, "track", num);
    }
  }
  // The cover in an .m4a: a picture stream, written once.
  AVStream *pic = NULL;
  if (o->codec != AUDIO_OPUS && s->cover && s->cover_len) {
    pic = avformat_new_stream(f->fmt, NULL);
    if (pic) {
      pic->disposition = AV_DISPOSITION_ATTACHED_PIC;
      pic->codecpar->codec_type = AVMEDIA_TYPE_VIDEO;
      pic->codecpar->codec_id = is_png(s->cover, s->cover_len) ? AV_CODEC_ID_PNG : AV_CODEC_ID_MJPEG;
      image_size(s->cover, s->cover_len, &pic->codecpar->width, &pic->codecpar->height);
      if (!pic->codecpar->width || !pic->codecpar->height) {
        pic->codecpar->codec_type = AVMEDIA_TYPE_DATA;  // not a picture it can read: no cover, then
        pic->disposition = 0;
      }
    }
  }
  if (avio_open(&f->fmt->pb, o->path, AVIO_FLAG_WRITE) < 0 || avformat_write_header(f->fmt, NULL) < 0) {
    RIP_ERROR("%s: can't create %s", ff_encoder(o->codec), o->path);
    ff_free(f);
    unlink(o->path);
    return RIP_ERR;
  }
  if (pic) {
    AVPacket *p = av_packet_alloc();
    if (p && av_new_packet(p, (int)s->cover_len) == 0) {
      memcpy(p->data, s->cover, s->cover_len);
      p->stream_index = pic->index;
      p->flags |= AV_PKT_FLAG_KEY;
      p->pts = p->dts = 0;
      av_interleaved_write_frame(f->fmt, p);
    }
    av_packet_free(&p);
  }
  if (swr_alloc_set_opts2(&f->swr, &f->enc->ch_layout, f->enc->sample_fmt, f->enc->sample_rate, &stereo,
                          AV_SAMPLE_FMT_S16, 44100, 0, NULL) < 0 ||
      swr_init(f->swr) < 0 || !(f->fifo = av_audio_fifo_alloc(f->enc->sample_fmt, 2, f->frame_size * 4))) {
    ff_free(f);
    unlink(o->path);
    return RIP_ENOMEM;
  }
  o->impl = f;
  return RIP_OK;
}

static int ff_convert(struct ff *f, const uint8_t *pcm, int samples) {
  uint8_t **data = NULL;
  int n = swr_get_out_samples(f->swr, samples);
  int rc = 0;

  if (n <= 0) {
    return 0;
  }
  if (av_samples_alloc_array_and_samples(&data, NULL, 2, n, f->enc->sample_fmt, 0) < 0) {
    return AVERROR(ENOMEM);
  }
  n = swr_convert(f->swr, data, n, pcm ? (const uint8_t **)&pcm : NULL, pcm ? samples : 0);
  if (n > 0 && av_audio_fifo_write(f->fifo, (void **)data, n) < n) {
    rc = AVERROR(ENOMEM);
  }
  av_freep(&data[0]);
  av_freep(&data);
  return rc < 0 ? rc : n;
}

static int ff_write(struct audio_out *o, const uint8_t *pcm, uint32_t frames) {
  struct ff *f = o->impl;

  if (ff_convert(f, pcm, (int)(frames * SAMPLES_PER_FRAME)) < 0 || ff_encode(f, 0) < 0) {
    RIP_ERROR("%s: encoding %s failed", ff_encoder(o->codec), o->path);
    return RIP_ERR;
  }
  return RIP_OK;
}

static int ff_close(struct audio_out *o, int ok) {
  struct ff *f = o->impl;
  int rc = ok ? RIP_OK : RIP_ERR;

  if (ok) {
    while (ff_convert(f, NULL, 0) > 0) {
    }
    if (ff_encode(f, 1) < 0 || avcodec_send_frame(f->enc, NULL) < 0 || ff_drain(f) < 0 ||
        av_write_trailer(f->fmt) < 0) {
      rc = RIP_ERR;
    }
  }
  ff_free(f);
  return rc;
}

#endif

/* WAV: a plain RIFF WAVE; the header's sizes are known before the first sample. */

struct wav {
  int fd;
};

static int wav_open(struct audio_out *o, uint64_t frames) {
  struct wav *w = calloc(1, sizeof(*w));
  uint64_t data = frames * CD_FRAME;
  uint8_t h[44];

  if (!w) {
    return RIP_ENOMEM;
  }
  if (data > 0xFFFFFFFFu - 36) {
    free(w);
    RIP_ERROR("wav: %s would be over 4 GiB", o->path);
    return RIP_ERR;
  }
  w->fd = open(o->path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
  if (w->fd < 0) {
    RIP_ERROR("wav: can't create %s: %s", o->path, strerror(errno));
    free(w);
    return RIP_ERR;
  }
  memcpy(h, "RIFF", 4);
  rip_put_le32(h + 4, (uint32_t)(36 + data));
  memcpy(h + 8, "WAVEfmt ", 8);
  rip_put_le32(h + 16, 16);
  rip_put_le16(h + 20, 1);  // PCM
  rip_put_le16(h + 22, 2);
  rip_put_le32(h + 24, 44100);
  rip_put_le32(h + 28, 44100 * 4);
  rip_put_le16(h + 32, 4);
  rip_put_le16(h + 34, 16);
  memcpy(h + 36, "data", 4);
  rip_put_le32(h + 40, (uint32_t)data);
  if (rip_write_all(w->fd, h, sizeof(h)) != 0) {
    close(w->fd);
    free(w);
    return RIP_ERR;
  }
  o->impl = w;
  return RIP_OK;
}

static int wav_write(struct audio_out *o, const uint8_t *pcm, uint32_t frames) {
  struct wav *w = o->impl;
  return rip_write_all(w->fd, pcm, (size_t)frames * CD_FRAME) == 0 ? RIP_OK : RIP_ERR;
}

static int wav_close(struct audio_out *o, int ok) {
  struct wav *w = o->impl;
  int rc = ok ? RIP_OK : RIP_ERR;

  if (close(w->fd) != 0) {
    rc = RIP_ERR;
  }
  free(w);
  return rc;
}

/* The codec's backend */

#ifdef RIPPER_HAVE_FLAC
#define HAVE_FLAC 1
#else
#define HAVE_FLAC 0
#endif
#ifdef RIPPER_HAVE_LAME
#define HAVE_LAME 1
#else
#define HAVE_LAME 0
#endif

int audio_out_available(enum audio_codec codec) {
  if (codec == AUDIO_FLAC) {
    return HAVE_FLAC;
  }
  if (codec == AUDIO_MP3) {
    return HAVE_LAME;
  }
  if (codec == AUDIO_AAC || codec == AUDIO_ALAC || codec == AUDIO_OPUS) {
#ifdef RIPPER_HAVE_FFMPEG
    return avcodec_find_encoder_by_name(ff_encoder(codec)) != NULL;
#else
    return 0;
#endif
  }
  return codec == AUDIO_WAV;
}

int audio_out_open(struct audio_out **out, const char *path, uint64_t frames, const struct audio_settings *s,
                   const struct audio_tags *tags) {
  struct audio_out *o;
  int rc = RIP_ENOTSUP;

  *out = NULL;
  if (!audio_out_available(s->codec)) {
    RIP_ERROR("audio: %s isn't in this build", audio_out_ext(s->codec));
    return RIP_ENOTSUP;
  }
  o = calloc(1, sizeof(*o));
  if (!o) {
    return RIP_ENOMEM;
  }
  o->codec = s->codec;
  snprintf(o->path, sizeof(o->path), "%s", path);
  switch (s->codec) {
#ifdef RIPPER_HAVE_FLAC
  case AUDIO_FLAC:
    rc = flac_open(o, frames, s, tags);
    break;
#endif
#ifdef RIPPER_HAVE_LAME
  case AUDIO_MP3:
    rc = mp3_open(o, s, tags);
    break;
#endif
#ifdef RIPPER_HAVE_FFMPEG
  case AUDIO_AAC:
  case AUDIO_ALAC:
  case AUDIO_OPUS:
    rc = ff_open(o, s, tags);
    break;
#endif
  case AUDIO_WAV:
    rc = wav_open(o, frames);
    break;
  default:
    break;
  }
  (void)tags;
  if (rc != RIP_OK) {
    free(o);
    return rc;
  }
  *out = o;
  return RIP_OK;
}

int audio_out_write(struct audio_out *o, const uint8_t *pcm, uint32_t frames) {
  switch (o->codec) {
#ifdef RIPPER_HAVE_FLAC
  case AUDIO_FLAC:
    return flac_write(o, pcm, frames);
#endif
#ifdef RIPPER_HAVE_LAME
  case AUDIO_MP3:
    return mp3_write(o, pcm, frames);
#endif
#ifdef RIPPER_HAVE_FFMPEG
  case AUDIO_AAC:
  case AUDIO_ALAC:
  case AUDIO_OPUS:
    return ff_write(o, pcm, frames);
#endif
  case AUDIO_WAV:
    return wav_write(o, pcm, frames);
  default:
    return RIP_ENOTSUP;
  }
}

int audio_out_close(struct audio_out *o, int ok) {
  int rc = ok ? RIP_OK : RIP_ERR;

  if (!o) {
    return rc;
  }
  switch (o->codec) {
#ifdef RIPPER_HAVE_FLAC
  case AUDIO_FLAC:
    rc = flac_close(o, ok);
    break;
#endif
#ifdef RIPPER_HAVE_LAME
  case AUDIO_MP3:
    rc = mp3_close(o, ok);
    break;
#endif
#ifdef RIPPER_HAVE_FFMPEG
  case AUDIO_AAC:
  case AUDIO_ALAC:
  case AUDIO_OPUS:
    rc = ff_close(o, ok);
    break;
#endif
  case AUDIO_WAV:
    rc = wav_close(o, ok);
    break;
  default:
    break;
  }
  if (rc != RIP_OK) {
    unlink(o->path);
  }
  free(o);
  return rc;
}
