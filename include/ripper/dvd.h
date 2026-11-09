#ifndef RIPPER_DVD_H_
#define RIPPER_DVD_H_

#include <stddef.h>
#include <stdint.h>

#include "ripper/reader.h"

// DVD-Video's structure from its IFO files: the titles, what each plays (its
// cells, as disc sectors, in order), its chapters, audio tracks and
// subtitles. What a remux to MKV/MP4 needs to follow a title the way a
// player would. The IFOs aren't scrambled; the VOB sectors the cells point to
// are (CSS), and are read through the CSS reader.

#define DVD_MAX_TITLES 99
#define DVD_MAX_AUDIO 8
#define DVD_MAX_SUBS 32
#define DVD_MAX_CHAPTERS 99

enum dvd_audio_format { DVD_AUDIO_AC3, DVD_AUDIO_MPEG1, DVD_AUDIO_MPEG2, DVD_AUDIO_LPCM, DVD_AUDIO_DTS, DVD_AUDIO_OTHER };

struct dvd_audio {
  enum dvd_audio_format format;
  unsigned channels;
  unsigned sample_rate;
  char lang[3];          // ISO 639-1, "" if not given
  unsigned extension;    // 1 normal, 2 visually impaired, 3/4 director's comments
  uint8_t stream_id;     // in the MPEG program stream: 0x80+n (AC-3), 0x88+n (DTS), 0xA0+n (LPCM), 0xC0+n (MPEG)
};

struct dvd_sub {
  char lang[3];
  unsigned extension;    // 1 normal, 2 large, 3 children, 5 closed captions, 9 forced, 13 director's comments...
  uint8_t stream_id;     // 0x20+n in private stream 1 (the widescreen one when it differs)
};

struct dvd_cell {
  uint32_t first;        // disc sectors, inclusive
  uint32_t last;
  uint32_t ms;           // playback time
};

struct dvd_title {
  unsigned number;       // 1-based, as the disc's title table numbers them
  unsigned vts;
  unsigned pgcn;
  unsigned angles;
  uint32_t ms;           // playback time
  unsigned cell_count;
  struct dvd_cell *cells;
  unsigned chapter_count;
  uint32_t chapter_ms[DVD_MAX_CHAPTERS];  // where each chapter starts
  unsigned audio_count;
  struct dvd_audio audio[DVD_MAX_AUDIO];
  unsigned sub_count;
  struct dvd_sub subs[DVD_MAX_SUBS];
  uint32_t palette[16];  // 0x00YYCrCb, as the IFO has it
  unsigned width;        // 720, 704, 352
  unsigned height;       // 480 (NTSC) or 576 (PAL)
  int widescreen;        // 16:9
  int pal;
  uint64_t bytes;        // what its cells add up to
};

struct dvd_disc {
  unsigned title_count;
  struct dvd_title titles[DVD_MAX_TITLES];
  int main_title;        // index of the longest title, -1 if none
};

// Reads VIDEO_TS/*.IFO through UDF. RIP_OK, or an error with a log line.
int dvd_load(struct rip_reader *r, struct dvd_disc *out);
void dvd_free(struct dvd_disc *d);

// The parsers themselves, for tests: the VMG's title table, and one VTS IFO
// (lba: where that IFO sits on the disc) adding the titles that use it.
int dvd_parse(const uint8_t *vmg, size_t vmg_len, const uint8_t *const *vts, const size_t *vts_len,
              const uint32_t *vts_lba, unsigned vts_count, struct dvd_disc *out);

// 0x00YYCrCb to 0x00RRGGBB (for VobSub's palette).
uint32_t dvd_ycrcb_to_rgb(uint32_t ycrcb);

#endif
