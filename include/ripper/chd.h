#ifndef RIPPER_CHD_H_
#define RIPPER_CHD_H_

#include <stddef.h>
#include <stdint.h>

#include "ripper/cdrom.h"

// CHD v5 writer, the format MAME's chdman makes and emulators read through
// libchdr (DuckStation, PCSX2, RetroArch cores, ...). Images are written in
// one pass as the disc is read: hunks go out compressed as they fill, the map
// and the hashes at the end.
//
//   DVD  2048-byte units, 4 KiB hunks, "DVD " metadata      (chdman createdvd)
//   CD   2448-byte frames (2352 + 96 subchannel), 8 per hunk, one CHT2 entry
//        per track, each track padded to 4 frames             (chdman createcd)

enum chd_codec {
  CHD_ZLIB,  // "zlib" / "cdzl": every libchdr reads it
  CHD_ZSTD   // "zstd" / "cdzs": smaller and faster, libchdr from 2023 on
};

struct chd_cd_track {
  enum cd_track_mode mode;
  uint32_t frames;  // frames written for the track, the pregap included
  uint32_t pregap;  // of those, the INDEX 00 frames at its start
};

struct chd_writer;

int chd_create_dvd(struct chd_writer **out, const char *path, uint64_t sectors, enum chd_codec codec);
// has_sub: the frames carry real subchannel (else it is stored as zeros).
int chd_create_cd(struct chd_writer **out, const char *path, const struct chd_cd_track *tracks, unsigned count,
                  enum chd_codec codec, int has_sub);

// DVD: 2048-byte sectors, in order.
int chd_write_sectors(struct chd_writer *w, const uint8_t *data, uint32_t count);
// CD: frames in disc order, track after track; sub (96 bytes per frame) may
// be NULL. Track padding is added by the writer.
int chd_write_frames(struct chd_writer *w, const uint8_t *frames, const uint8_t *sub, uint32_t count);

// Writes the map and header. sha1 gets the data SHA-1 (chdman's "Data SHA1").
int chd_finish(struct chd_writer *w, uint8_t sha1[20]);
// Stops and deletes the file.
void chd_abort(struct chd_writer *w);

#endif
