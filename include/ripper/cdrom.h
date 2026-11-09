#ifndef RIPPER_CDROM_H_
#define RIPPER_CDROM_H_

#include <stddef.h>
#include <stdint.h>

#include "ripper/transport.h"

// Compact discs, through an external drive (the PS5's own drive has no CD
// laser). A CD is one stream of 2352-byte frames; the TOC says where each
// track starts and whether it holds audio or data.

#define CD_FRAME 2352
#define CD_DATA 2048
#define CD_C2 294    // one bit per byte of the frame
#define CD_SUB 96    // raw P-W subchannel
#define CD_PREGAP 150  // LBA 0 is MSF 00:02:00
#define CD_FRAMES_PER_SEC 75
#define CD_MAX_TRACKS 99

// Q-channel control bits (TOC "CONTROL").
#define CD_CTRL_PREEMPHASIS 0x01
#define CD_CTRL_COPY 0x02
#define CD_CTRL_DATA 0x04
#define CD_CTRL_4CH 0x08

enum cd_track_mode {
  CD_MODE_AUDIO,
  CD_MODE1,       // 2048 bytes of data per frame, with ECC
  CD_MODE2,       // XA: form 1 (2048) or form 2 (2324) per sector, mixed
  CD_MODE2_FORMLESS
};

// The disc type the lead-in's A0 point gives (full TOC).
enum cd_disc_type {
  CD_TYPE_DA_ROM = 0x00,
  CD_TYPE_CDI = 0x10,
  CD_TYPE_XA = 0x20
};

struct cd_track {
  uint8_t number;
  uint8_t control;  // CD_CTRL_*
  uint8_t session;
  enum cd_track_mode mode;
  uint32_t start;   // LBA of INDEX 01
  uint32_t pregap;  // frames of INDEX 00 before start (part of this track in a Redump .bin)
  uint32_t length;  // frames from start to the next track's pregap, or to the session's lead-out
};

struct cd_toc {
  uint8_t first;
  uint8_t last;
  unsigned count;
  unsigned sessions;
  enum cd_disc_type type;
  uint32_t leadout;  // LBA of the last session's lead-out: the end of the disc
  struct cd_track tracks[CD_MAX_TRACKS];
};

static inline int cd_track_is_audio(const struct cd_track *t) {
  return !(t->control & CD_CTRL_DATA);
}

// READ TOC: the full TOC (sessions, disc type) when the drive gives it, else
// the plain one. Track lengths stop at each session's lead-out; pregaps are
// left at 150 frames after a data track and 0 otherwise until cd_find_pregaps.
int cd_read_toc(struct rip_transport *t, struct cd_toc *toc);

// Flags for cd_read_frames: what follows each 2352-byte frame in buf.
#define CD_READ_C2 0x01   // + 294 bytes of C2 error pointers
#define CD_READ_SUB 0x02  // + 96 bytes of raw P-W subchannel

// READ CD of whole frames. buf holds count * cd_frame_stride(flags) bytes.
int cd_read_frames(struct rip_transport *t, uint32_t lba, uint32_t count, unsigned flags, void *buf,
                   struct rip_sense *sense);
size_t cd_frame_stride(unsigned flags);

// The mode byte of a data track's first sector (raw header byte 15).
int cd_detect_mode(struct rip_transport *t, struct cd_track *track);

// Q subchannel of one frame (12 bytes, CRC checked). RIP_OK, RIP_ERR on a bad
// CRC or a drive that cannot return it.
int cd_read_q(struct rip_transport *t, uint32_t lba, uint8_t q[12]);
// Raw P-W (96 bytes, one bit of each channel per byte) to the 12 Q bytes.
void cd_deinterleave_q(const uint8_t *pw, uint8_t q[12]);
int cd_q_crc_ok(const uint8_t q[12]);
// Builds a Q block for a position (ADR 1) with its CRC: what the image drive serves.
void cd_make_q(uint8_t q[12], uint8_t control, uint8_t track, uint8_t index, uint32_t rel, uint32_t abs_lba);

// Finds where each track's INDEX 00 begins from the Q subchannel, so tracks
// can be cut where Redump cuts them. Tracks the drive can't tell keep the
// defaults cd_read_toc gave.
void cd_find_pregaps(struct rip_transport *t, struct cd_toc *toc);

// Reads audio frames shifted by the drive's read offset (in samples, as
// AccurateRip lists them), so the bytes land where they are on the disc.
// Positions before LBA 0 or past end_lba read as silence.
int cd_read_audio(struct rip_transport *t, int64_t lba, uint32_t count, int offset_samples, uint32_t end_lba,
                  uint8_t *buf);

// CD-ROM error detection and correction (ECMA-130 annex A and B).
// EDC: the CRC-32 (polynomial 0x8001801B, reflected) each data sector carries.
uint32_t cd_edc(uint32_t edc, const uint8_t *p, size_t len);
// P and Q parity of a Mode 1 sector (bytes 2076..2351), from its header and data.
void cd_ecc_generate(uint8_t sector[CD_FRAME]);
// The sector's parity matches what cd_ecc_generate makes from it.
int cd_ecc_verify(const uint8_t sector[CD_FRAME]);
// A data sector's EDC is right for its mode: 1 yes, 0 no, -1 not a data sector.
int cd_sector_ok(const uint8_t sector[CD_FRAME]);
// Fills in sync, header, EDC and ECC around 2048 bytes already at sector+16:
// a Mode 1 sector at lba, as a drive would read it.
void cd_make_mode1(uint8_t sector[CD_FRAME], uint32_t lba);
extern const uint8_t cd_sync[12];

// Frames <-> "MM:SS:FF".
void cd_msf_str(uint32_t frames, char *out, size_t len);
uint8_t cd_bcd(uint8_t v);
uint8_t cd_unbcd(uint8_t v);

// The MusicBrainz disc ID of the audio session (28 characters + NUL).
void cd_musicbrainz_id(const struct cd_toc *toc, char out[29]);
// The FreeDB/CDDB disc ID, for metadata services that still use it.
uint32_t cd_freedb_id(const struct cd_toc *toc);

#endif
