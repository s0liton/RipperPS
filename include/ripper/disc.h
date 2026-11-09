#ifndef RIPPER_DISC_H_
#define RIPPER_DISC_H_

#include <stdint.h>

#include "ripper/cdrom.h"
#include "ripper/reader.h"
#include "ripper/transport.h"

enum rip_disc_kind {
  RIP_DISC_UNKNOWN,
  RIP_DISC_CD_DATA,
  RIP_DISC_CD_AUDIO,
  RIP_DISC_PS1,
  RIP_DISC_PS2_CD,
  RIP_DISC_PS2_DVD,
  RIP_DISC_PS3_DVD,
  RIP_DISC_PS3_BD,
  RIP_DISC_PS4_BD,
  RIP_DISC_PS5_BD,
  RIP_DISC_DVD_VIDEO,
  RIP_DISC_DVD_DATA,
  RIP_DISC_BD_VIDEO,
  RIP_DISC_BD_DATA,
  // CD systems, through an external drive.
  RIP_DISC_SATURN,
  RIP_DISC_SEGA_CD,
  RIP_DISC_PCE_CD,
  RIP_DISC_PCFX,
  RIP_DISC_NEOGEO_CD,
  RIP_DISC_3DO,
  RIP_DISC_CDI,
  RIP_DISC_VIDEO_CD,
  RIP_DISC_XBOX,      // Xbox or Xbox 360: only the video partition shows without a Kreon drive
  RIP_DISC_XBOX_ONE,  // Xbox One or Series: a Blu-ray whose game packages (MSXC) stay encrypted
  RIP_DISC_XBOX_360,  // told from the Xbox by the security sector, on Kreon or OmniDrive
  RIP_DISC_GAMECUBE,  // Nintendo DVDs: unreadable but through OmniDrive's raw read
  RIP_DISC_WII,
  RIP_DISC_WII_U,  // a Blu-ray in Nintendo's scrambling
  RIP_DISC_DREAMCAST,
  RIP_DISC_KIND_COUNT
};

struct rip_disc {
  enum rip_disc_kind kind;
  uint16_t profile;
  uint32_t sector_size;
  uint64_t sector_count;
  char title_id[32];   // "SLUS-20946", "BLUS-30001"; empty when unknown
  char volume_id[33];  // ISO 9660 (or UDF) volume label
  char title[128];     // the game's or film's name when the disc says it (PARAM.SFO TITLE)
  int has_iso9660;
  int css;  // DVD-Video protected by CSS
  int has_udf;
  // CDs: the TOC as read, and the audio session's MusicBrainz disc ID.
  int has_toc;
  struct cd_toc toc;
  char musicbrainz_id[29];
  // Dreamcast: a GD-ROM (its high-density area starts at GD_HD_LBA), and
  // whether this drive read that area's first sector.
  int gd_rom;
  int gd_hd_readable;
};

#define GD_HD_LBA 45000

// Identifies the disc from the drive profile plus what is on the disc.
// reader may be NULL when the medium cannot be read directly (PS4/PS5 games).
// t may be NULL (image files); it is only used for DVD copyright info.
// mount_root is where the OS mounted the disc ("/mnt/disc" on the PS5), or NULL;
// it is the fallback for title IDs the raw sectors do not expose.
int rip_disc_identify(struct rip_disc *out, uint16_t profile, struct rip_reader *reader, struct rip_transport *t,
                      const char *mount_root);
// The title a PS3/PS4/PS5 disc gives in the system's mount of it, for when the
// system mounted it after rip_disc_identify looked. 1 when d has a title now.
int rip_disc_title_from_mount(struct rip_disc *d, const char *mount_root);

// What only the drive's firmware can tell (Kreon, OmniDrive): Xbox from
// Xbox 360, and a Nintendo disc's ID and name from its header.
struct rip_drive;
void rip_disc_refine(struct rip_disc *d, struct rip_drive *drive);

const char *rip_disc_kind_name(enum rip_disc_kind kind);

// Exposed for tests. "cdrom0:\SLUS_209.46;1" -> "SLUS-20946".
int rip_parse_system_cnf(const char *cnf, size_t len, int *is_ps2, char *serial, size_t serial_len);
// "BLUS30001" -> "BLUS-30001". Leaves IDs that already have a dash alone.
void rip_format_title_id(const char *raw, char *out, size_t out_len);

#endif
