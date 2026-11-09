// What custom drive firmware reads: Nintendo's scrambling under OmniDrive,
// and the Xbox layout from a security sector.

#include <stdint.h>
#include <string.h>

#include "ripper/common.h"
#include "ripper/omnidrive.h"
#include "ripper/xbox.h"
#include "test.h"

static void test_edc(void) {
  // CRC-32/DVD-ROM-EDC's check value.
  CHECK(dvd_edc((const uint8_t *)"123456789", 9) == 0xB27CE117u);
}

// ECMA-267 table 2: the shift register's preset for each group of 16 sectors.
// The scrambling's first byte in a group is its preset's low byte.
static void test_dvd_presets(void) {
  static const uint16_t presets[16] = {0x0001, 0x5500, 0x0002, 0x2A00, 0x0004, 0x5400, 0x0008, 0x2800,
                                       0x0010, 0x5000, 0x0020, 0x2001, 0x0040, 0x4002, 0x0080, 0x0005};
  uint8_t data[2048];
  unsigned k;

  for (k = 0; k < 16; k++) {
    memset(data, 0, sizeof(data));
    dvd_descramble(data, 0x30000 + k * 16, -1);
    CHECK(data[0] == (uint8_t)presets[k]);
  }
}

// A frame as the drive would hand it over, scrambled with key.
static void make_frame(uint8_t f[DVD_RAW_FRAME], uint32_t psn, const uint8_t *data2048, int key) {
  memset(f, 0, DVD_RAW_FRAME);
  f[1] = (uint8_t)(psn >> 16);
  f[2] = (uint8_t)(psn >> 8);
  f[3] = (uint8_t)psn;
  memcpy(f + 12, data2048, 2048);
  rip_put_be32(f + DVD_RAW_FRAME - 4, dvd_edc(f, DVD_RAW_FRAME - 4));
  dvd_descramble(f + 12, psn, key);  // the scrambling is its own inverse
}

static void test_nintendo_frames(void) {
  uint8_t data[2048];
  uint8_t f[DVD_RAW_FRAME];
  unsigned i;

  for (i = 0; i < sizeof(data); i++) {
    data[i] = (uint8_t)(i * 7 + 3);
  }
  make_frame(f, 0x30000 + 40, data, 9);
  CHECK(dvd_frame_fix(f, 9));
  CHECK_MEM(f + 12, data, 2048);
  make_frame(f, 0x30000 + 40, data, 9);
  CHECK(!dvd_frame_fix(f, 4));  // the wrong key fails the EDC
  make_frame(f, 0x30000 + 40, data, -1);
  CHECK(dvd_frame_fix(f, -1));
  {
    const uint8_t sum8[8] = {0x10, 0x20, 0x30, 0x40, 0, 0, 0, 0};  // 0xA0: (0x0A + 0xA0) & 0xF
    CHECK(nintendo_key_derive(sum8) == 0xA);
  }
}

static void test_bd_scrambler(void) {
  uint8_t frame[2052];
  uint8_t copy[2052];
  unsigned i;

  for (i = 0; i < sizeof(frame); i++) {
    frame[i] = (uint8_t)i;
  }
  memcpy(copy, frame, sizeof(frame));
  bd_descramble(frame, 12345, 1);
  CHECK(memcmp(frame, copy, sizeof(frame)) != 0);
  bd_descramble(frame, 12345, 1);
  CHECK_MEM(frame, copy, sizeof(frame));
}

static void put24(uint8_t *p, int32_t v) {
  p[0] = (uint8_t)((uint32_t)v >> 16);
  p[1] = (uint8_t)((uint32_t)v >> 8);
  p[2] = (uint8_t)v;
}

// An XGD2 disc as Redump sizes it: 3,820,880 sectors, with ranges 0 and 3
// of the drive table kept and the others skipped.
static void test_xbox_layout(void) {
  struct xbox_layout x;
  uint8_t pfi[17];
  const int32_t l0_last = 2110367;
  uint8_t *e;

  memset(&x, 0, sizeof(x));
  memset(pfi, 0, sizeof(pfi));
  pfi[2] = 0x31;  // two layers, opposite track path
  put24(pfi + 5, 0x030000);
  put24(pfi + 9, 0xFCE60F);
  put24(pfi + 13, 0x0333AF);  // the video partition's own layer break, before the game
  x.ss[2] = 0x31;
  put24(x.ss + 5, 0x04FB20);
  put24(x.ss + 9, 0xFB04DF);
  put24(x.ss + 13, l0_last);
  x.ss[0x660] = 4;
  e = x.ss + 0x661;
  put24(e + 3, 0x060600);  // range 0
  put24(e + 6, 0x0607FF);
  put24(e + 9 + 3, 0x070000);  // range 1: not one Redump keeps for XGD2
  put24(e + 9 + 6, 0x0701FF);
  put24(e + 27 + 3, 0xFBFFFF - 0x200);  // range 3, on layer 1
  put24(e + 27 + 6, 0xFBFFFF);
  CHECK(xbox_layout_from(&x, pfi, 1000000, 0) == RIP_OK);
  CHECK(x.version == 2);
  CHECK(x.sectors == 3820880);
  CHECK(x.range_count == 2);
  CHECK(x.ranges[0].start == 0x060600 - 0x30000 && x.ranges[0].end == 0x060800 - 0x30000);
  CHECK(x.ranges[1].end - x.ranges[1].start == 0x201);
  CHECK(x.ranges[1].start > (uint32_t)l0_last);  // past layer 0, counted on
  // Kreon also skips the padding before layer 1's video partition.
  CHECK(xbox_layout_from(&x, pfi, 1000000, 1) == RIP_OK);
  CHECK(x.range_count == 3);
  CHECK(x.ranges[2].start == 1000000 && x.ranges[2].end == 1000000 + 0x04FB20 - 0x0333AF - 1);
  x.ss[15] = 0;  // no known layer break: not an XGD
  CHECK(xbox_layout_from(&x, pfi, 1000000, 0) != RIP_OK);
}

void test_firmware(void) {
  test_edc();
  test_dvd_presets();
  test_nintendo_frames();
  test_bd_scrambler();
  test_xbox_layout();
}
