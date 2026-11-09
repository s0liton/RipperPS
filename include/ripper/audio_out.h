#ifndef RIPPER_AUDIO_OUT_H_
#define RIPPER_AUDIO_OUT_H_

#include <stddef.h>
#include <stdint.h>

// CD audio to a file, in the codec asked for: FLAC (libFLAC), MP3 (LAME),
// AAC or ALAC in .m4a and Opus (FFmpeg), WAV. A build has whichever of those
// libraries it was given (the console's SDK has them all); WAV always works.
// Input is CD frames as read: 2352 bytes, 588 stereo samples of 16-bit
// little-endian PCM at 44.1 kHz.

enum audio_codec { AUDIO_FLAC, AUDIO_MP3, AUDIO_AAC, AUDIO_ALAC, AUDIO_OPUS, AUDIO_WAV, AUDIO_CODEC_COUNT };

struct audio_tags {
  const char *artist;
  const char *album;
  const char *title;
  const char *date;
  const char *mb_discid;  // MusicBrainz disc ID
  unsigned track;
  unsigned total;
};

struct audio_settings {
  enum audio_codec codec;
  // FLAC: compression 0-8 (5). MP3: VBR quality 0-9 (2), or a constant bit
  // rate in kbit/s when 32 or more (320). AAC, Opus: kbit/s (256, 160).
  int quality;
  const uint8_t *cover;  // the album's front cover, embedded where the format takes one; may be NULL
  size_t cover_len;
};

struct audio_out;

// ".flac", ".mp3", ".m4a", ".opus", ".wav".
const char *audio_out_ext(enum audio_codec codec);
// Whether this build writes it.
int audio_out_available(enum audio_codec codec);

// frames: how many CD frames will be written (some headers need it).
int audio_out_open(struct audio_out **out, const char *path, uint64_t frames, const struct audio_settings *s,
                   const struct audio_tags *tags);
int audio_out_write(struct audio_out *o, const uint8_t *pcm, uint32_t frames);
// Finishes the file (ok) or removes it.
int audio_out_close(struct audio_out *o, int ok);

#endif
