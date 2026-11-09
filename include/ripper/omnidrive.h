#ifndef RIPPER_OMNIDRIVE_H_
#define RIPPER_OMNIDRIVE_H_

#include <stddef.h>
#include <stdint.h>

#include "ripper/reader.h"
#include "ripper/transport.h"

// OmniDrive firmware's raw read (C0) and what reading Nintendo discs through
// it takes: the DVD and Blu-ray scramblers, Nintendo's own seeds, and the EDC
// that tells a good frame. The same reads and the same math as redumper.

#define DVD_RAW_FRAME 2064  // ID 4, IED 2, CPR_MAI 6, data 2048, EDC 4
#define BD_RAW_FRAME 2070   // data 2048, EDC 4, and 18 more bytes from the drive
#define DVD_ECC_FRAMES 16   // sectors per DVD ECC block

enum omni_disc { OMNI_CD, OMNI_DVD, OMNI_BD };

// address is an LBA, or a PSN with raw_addressing.
int omni_read(struct rip_transport *t, enum omni_disc type, int raw_addressing, int32_t address, uint32_t count,
              void *buf, size_t frame_size, struct rip_sense *sense);

// CRC-32/DVD-ROM-EDC (poly 0x80000011, no reflection).
uint32_t dvd_edc(const uint8_t *p, size_t n);
// The 2048 data bytes of a DVD frame at physical sector psn. nintendo_key
// is -1 for a standard disc.
void dvd_descramble(uint8_t *data, uint32_t psn, int nintendo_key);
// Descrambles frame in place and checks its EDC.
int dvd_frame_fix(uint8_t frame[DVD_RAW_FRAME], int nintendo_key);
// Nintendo's key: from LBA 0's CPR_MAI and first two data bytes, descrambled with key 0.
int nintendo_key_derive(const uint8_t bytes[8]);
// The 2052 bytes (data and EDC) of a Blu-ray frame.
void bd_descramble(uint8_t *frame, int32_t lba, int nintendo);

// GameCube and Wii (DVD) or Wii U (Blu-ray) through C0, descrambled and
// checked, as 2048-byte sectors. sectors is the disc's length.
int rip_reader_open_nintendo(struct rip_reader *r, struct rip_transport *t, int bd, uint64_t sectors);

#endif
