#include "ripper/diag.h"

#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "ripper/cdrom.h"
#include "ripper/common.h"
#include "ripper/hash.h"
#include "ripper/log.h"
#include "ripper/mmc.h"
#include "ripper/util.h"

#ifndef RIPPER_VERSION
#define RIPPER_VERSION "dev"
#endif

#define BUF_BYTES (1024u * 1024)
#define SHORT_MS 10000u
#define READ_MS 30000u
#define STEPS 11

struct diag {
  struct rip_transport *t;
  const char *dir;
  FILE *rep;
  uint8_t *base;  // page-aligned
  uint8_t *buf;   // where answers land: base, or base + an offset for the page test
  int seq;
  diag_progress_fn progress;
  void *ud;
  const volatile int *cancel;
  int step;
  struct diag_summary *sum;
  // What's been learned, for the findings.
  uint16_t profile;
  uint32_t last_lba;
  uint32_t block_len;
  int is_cd;
  int is_dvd;
  int is_bd;
  struct cd_toc toc;
  int have_toc;
  char vendor[9];
  char product[17];
  int omnidrive;  // OmniDrive firmware, from INQUIRY bytes 74-85
  int lg;         // HL-DT-ST (LG), where the cache tricks live
};

/* Output */

static void say(struct diag *d, const char *fmt, ...) {
  va_list ap;

  va_start(ap, fmt);
  vfprintf(d->rep, fmt, ap);
  va_end(ap);
  fputc('\n', d->rep);
  fflush(d->rep);
}

static void finding(struct diag *d, const char *fmt, ...) {
  struct diag_summary *s = d->sum;
  size_t at = strlen(s->findings);
  va_list ap;

  va_start(ap, fmt);
  vsnprintf(s->findings + at, sizeof(s->findings) - at, fmt, ap);
  va_end(ap);
  at = strlen(s->findings);
  if (at + 1 < sizeof(s->findings)) {
    s->findings[at] = '\n';
    s->findings[at + 1] = '\0';
  }
}

static void section(struct diag *d, const char *title) {
  d->step++;
  say(d, "\n== %s ==", title);
  if (d->progress) {
    d->progress(d->ud, title, d->step, STEPS);
  }
}

static int stopped(const struct diag *d) {
  return d->cancel && *d->cancel;
}

static const char *sense_name(const struct rip_sense *s) {
  static const struct {
    uint8_t key;
    uint8_t asc;
    uint8_t ascq;
    const char *text;
  } names[] = {
      {0x02, 0x04, 0xFF, "not ready"},
      {0x02, 0x30, 0xFF, "incompatible medium"},
      {0x02, 0x3A, 0xFF, "no disc"},
      {0x03, 0x02, 0x00, "no seek complete"},
      {0x03, 0x11, 0x00, "unrecovered read error"},
      {0x03, 0x11, 0x05, "L-EC uncorrectable"},
      {0x03, 0x11, 0x06, "CIRC unrecovered"},
      {0x03, 0x15, 0xFF, "positioning error"},
      {0x03, 0x30, 0xFF, "can't read this medium"},
      {0x03, 0x57, 0x00, "can't recover the TOC"},
      {0x04, 0xFF, 0xFF, "hardware error"},
      {0x05, 0x20, 0x00, "command not supported"},
      {0x05, 0x21, 0x00, "address out of range"},
      {0x05, 0x24, 0x00, "invalid field in the command"},
      {0x05, 0x26, 0xFF, "invalid field in the parameters"},
      {0x05, 0x30, 0x02, "can't read this format"},
      {0x05, 0x64, 0x00, "illegal mode for this track"},
      {0x05, 0x6F, 0x01, "key not present"},
      {0x05, 0x6F, 0x02, "key not established"},
      {0x05, 0x6F, 0x03, "scrambled sector without authentication"},
      {0x05, 0x6F, 0xFF, "copy protection"},
      {0x06, 0x28, 0xFF, "medium changed"},
      {0x06, 0x29, 0xFF, "reset"},
  };
  size_t i;

  for (i = 0; i < RIP_ARRAY_LEN(names); i++) {
    if (names[i].key == s->key && (names[i].asc == 0xFF || names[i].asc == s->asc) &&
        (names[i].ascq == 0xFF || names[i].ascq == s->ascq)) {
      return names[i].text;
    }
  }
  return "";
}

// One command: sent, timed, its outcome a line in the report and, when it
// read something, the answer saved under raw/. RIP_OK when the drive took it.
static int cmd(struct diag *d, const char *name, const uint8_t *cdb, size_t cdb_len, enum rip_dir dir, size_t len,
               unsigned timeout, size_t save, uint64_t *ms_out) {
  struct rip_sense sense;
  uint64_t t0;
  uint64_t ms;
  char hex[64];
  size_t i;
  int rc;

  memset(&sense, 0, sizeof(sense));
  if (len > BUF_BYTES) {
    len = BUF_BYTES;
  }
  if (dir == RIP_DIR_IN) {
    memset(d->buf, 0, len);
  }
  for (i = 0; i < cdb_len && i < 16; i++) {
    snprintf(hex + i * 3, sizeof(hex) - i * 3, "%02x ", cdb[i]);
  }
  t0 = rip_now_ms();
  rc = rip_scsi(d->t, cdb, cdb_len, dir, len ? d->buf : NULL, len, timeout, &sense);
  ms = rip_now_ms() - t0;
  if (ms_out) {
    *ms_out = ms;
  }
  d->sum->commands++;
  if (rc == RIP_OK) {
    d->sum->answered++;
    say(d, "[ok]   %-38s %5llu ms   cdb %s", name, (unsigned long long)ms, hex);
    if (save && dir == RIP_DIR_IN) {
      char path[1200];
      snprintf(path, sizeof(path), "%s/raw/%03d-%s.bin", d->dir, ++d->seq, name);
      for (i = strlen(d->dir) + 5; path[i]; i++) {
        if (path[i] == ' ' || path[i] == '/' || path[i] == '(' || path[i] == ')' || path[i] == ',') {
          path[i] = '_';
        }
      }
      rip_write_file(path, d->buf, save < len ? save : len);
    }
  } else if (rc == RIP_ESENSE) {
    say(d, "[no]   %-38s %5llu ms   cdb %s  sense %02x/%02x/%02x %s", name, (unsigned long long)ms, hex, sense.key,
        sense.asc, sense.ascq, sense_name(&sense));
  } else {
    say(d, "[err]  %-38s %5llu ms   cdb %s  %s", name, (unsigned long long)ms, hex,
        d->t->last_error[0] ? d->t->last_error : "no answer");
  }
  return rc;
}

static void cdb_lba(uint8_t *cdb, int at, uint32_t lba) {
  rip_put_be32(cdb + at, lba);
}

/* The drive */

static const char *feature_name(uint16_t code) {
  switch (code) {
  case 0x0000:
    return "Profile List";
  case 0x0001:
    return "Core";
  case 0x0002:
    return "Morphing";
  case 0x0003:
    return "Removable Medium";
  case 0x0004:
    return "Write Protect";
  case 0x0010:
    return "Random Readable";
  case 0x001D:
    return "Multi-Read";
  case 0x001E:
    return "CD Read";
  case 0x001F:
    return "DVD Read";
  case 0x0020:
    return "Random Writable";
  case 0x0021:
    return "Incremental Streaming Writable";
  case 0x0023:
    return "Formattable";
  case 0x0024:
    return "Hardware Defect Management";
  case 0x0026:
    return "Restricted Overwrite";
  case 0x002D:
    return "CD Track at Once";
  case 0x002E:
    return "CD Mastering";
  case 0x002F:
    return "DVD-R/-RW Write";
  case 0x0033:
    return "Layer Jump Recording";
  case 0x0037:
    return "CD-RW Media Write Support";
  case 0x003A:
    return "DVD+RW Dual Layer";
  case 0x003B:
    return "DVD+R Dual Layer";
  case 0x0040:
    return "BD Read";
  case 0x0041:
    return "BD Write";
  case 0x0100:
    return "Power Management";
  case 0x0101:
    return "SMART";
  case 0x0102:
    return "Embedded Changer";
  case 0x0103:
    return "CD Audio External Play";
  case 0x0104:
    return "Microcode Upgrade";
  case 0x0105:
    return "Timeout";
  case 0x0106:
    return "DVD CSS";
  case 0x0107:
    return "Real Time Streaming";
  case 0x0108:
    return "Drive Serial Number";
  case 0x0109:
    return "Media Serial Number";
  case 0x010A:
    return "Disc Control Blocks";
  case 0x010B:
    return "DVD CPRM";
  case 0x010C:
    return "Firmware Information";
  case 0x010D:
    return "AACS";
  case 0x010E:
    return "DVD CSS Managed Recording";
  case 0x0110:
    return "VCPS";
  case 0x0113:
    return "SecurDisc";
  case 0x0142:
    return "OSSC";
  default:
    return code >= 0xFF00 ? "vendor" : "";
  }
}

static void drive_identity(struct diag *d) {
  uint8_t cdb[12];
  uint8_t *b = d->buf;
  char vendor[9];
  char product[17];
  char rev[5];
  char extra[64];
  size_t i;

  section(d, "Drive");
  memset(cdb, 0, sizeof(cdb));
  cdb[0] = 0x12;
  cdb[4] = 104;  // OmniDrive's mark sits at bytes 74-85
  if (cmd(d, "inquiry", cdb, 6, RIP_DIR_IN, 104, SHORT_MS, 104, NULL) == RIP_OK) {
    memcpy(vendor, b + 8, 8);
    vendor[8] = '\0';
    memcpy(product, b + 16, 16);
    product[16] = '\0';
    memcpy(rev, b + 32, 4);
    rev[4] = '\0';
    // Bytes 36 on are the vendor's: firmware dates, sometimes a patched
    // firmware's mark.
    for (i = 0; i < 20 && b[36 + i]; i++) {
      extra[i] = b[36 + i] >= 0x20 && b[36 + i] < 0x7F ? (char)b[36 + i] : '.';
    }
    extra[i] = '\0';
    say(d, "vendor '%s' product '%s' revision '%s' vendor-specific '%s', %u bytes in all", vendor, product, rev, extra,
        b[4] + 5u);
    snprintf(d->vendor, sizeof(d->vendor), "%s", vendor);
    snprintf(d->product, sizeof(d->product), "%s", product);
    d->lg = !strncmp(vendor, "HL-DT-ST", 8);
    // Custom firmware that says so: OmniDrive (redumper's is_omnidrive_firmware)
    // and SpiralDrive. A USB bridge that cuts INQUIRY to 36 bytes hides both.
    if (!memcmp(b + 74, "OmniDrive", 9)) {
      d->omnidrive = 1;
      say(d, "OmniDrive firmware %u.%u.%u", b[83], b[84], b[85]);
      finding(d, "firmware: OmniDrive %u.%u.%u%s", b[83], b[84], b[85],
              (b[83] << 16 | b[84] << 8 | b[85]) < 0x010004 ? " (1.0.4 or newer reads DVDs right)" : "");
    } else if (!memcmp(b + 36, "KREON V1.00", 11)) {
      finding(d, "firmware: Kreon (Xbox and Xbox 360 unlock)");
    } else if (!memcmp(b + 36, "SpiralDrive v", 13)) {
      finding(d, "firmware: SpiralDrive (%.20s)", b + 36);
    } else if (b[4] + 5u < 86) {
      finding(d,
              "firmware: INQUIRY is only %u bytes, too short to carry custom firmware's mark (some USB bridges cut it)",
              b[4] + 5u);
    } else {
      finding(d, "firmware: no OmniDrive or SpiralDrive mark (LibreDrive can't be seen from here)");
    }
    snprintf(d->sum->drive, sizeof(d->sum->drive), "%.8s %.16s %.4s", vendor, product, rev);
  }
  // The serial number, hashed: enough to tell two reports of one drive apart.
  memset(cdb, 0, sizeof(cdb));
  cdb[0] = 0x12;
  cdb[1] = 0x01;
  cdb[2] = 0x80;
  cdb[4] = 64;
  if (cmd(d, "inquiry serial (hashed)", cdb, 6, RIP_DIR_IN, 64, SHORT_MS, 0, NULL) == RIP_OK) {
    struct rip_sha256 ctx;
    uint8_t h[32];
    char hx[17];
    rip_sha256_init(&ctx);
    rip_sha256_update(&ctx, b + 4, b[3] < 60 ? b[3] : 60);
    rip_sha256_final(&ctx, h);
    rip_hex(h, 8, hx);
    say(d, "serial hash %s", hx);
  }
  memset(cdb, 0, sizeof(cdb));
  cdb[0] = 0x12;
  cdb[1] = 0x01;
  cdb[2] = 0x00;
  cdb[4] = 64;
  cmd(d, "inquiry vpd pages", cdb, 6, RIP_DIR_IN, 64, SHORT_MS, 64, NULL);

  // Every feature the drive has, current or not.
  // 32 KiB: some pass-throughs refuse anything near 64 KiB, and no drive's
  // feature list comes close.
  memset(cdb, 0, sizeof(cdb));
  cdb[0] = 0x46;
  rip_put_be16(cdb + 7, 32768);
  if (cmd(d, "get configuration (all)", cdb, 10, RIP_DIR_IN, 32768, SHORT_MS, 32768, NULL) == RIP_OK) {
    uint32_t len = rip_be32(b) + 4;
    uint32_t at = 8;
    if (len > 32768) {
      say(d, "  (the list is %u bytes; the first 32 KiB is here)", len);
      len = 32768;
    }
    say(d, "current profile %04x (%s)", rip_be16(b + 6), mmc_profile_name(rip_be16(b + 6)));
    while (at + 4 <= len) {
      uint16_t code = rip_be16(b + at);
      uint8_t add = b[at + 3];
      say(d, "  feature %04x %-32s v%u%s", code, feature_name(code), (b[at + 2] >> 2) & 0x0F,
          (b[at + 2] & 1) ? " current" : "");
      if (code == 0x0000) {
        uint32_t p;
        for (p = 0; p + 4 <= add; p += 4) {
          uint16_t prof = rip_be16(b + at + 4 + p);
          say(d, "    profile %04x %s%s", prof, mmc_profile_name(prof), (b[at + 4 + p + 2] & 1) ? " (current)" : "");
        }
      }
      if (code == 0x001E && add >= 1) {
        say(d, "    CD Read: C2 %s, CD-Text %s", (b[at + 4] & 0x02) ? "yes" : "no", (b[at + 4] & 0x01) ? "yes" : "no");
        finding(d, "C2 error pointers (feature): %s", (b[at + 4] & 0x02) ? "yes" : "no");
      }
      at += 4 + add;
    }
  }

  // Mode pages: capabilities, error recovery, and all of them raw.
  {
    static const struct {
      uint8_t page;
      const char *name;
    } pages[] = {{0x2A, "mode page 2a capabilities"},
                 {0x01, "mode page 01 error recovery"},
                 {0x0D, "mode page 0d cd parameters"},
                 {0x3F, "mode pages (all)"}};
    for (i = 0; i < RIP_ARRAY_LEN(pages); i++) {
      memset(cdb, 0, sizeof(cdb));
      cdb[0] = 0x5A;
      cdb[1] = 0x08;  // no block descriptors
      cdb[2] = pages[i].page;
      rip_put_be16(cdb + 7, 4096);
      if (cmd(d, pages[i].name, cdb, 10, RIP_DIR_IN, 4096, SHORT_MS, 4096, NULL) == RIP_OK && pages[i].page == 0x2A) {
        const uint8_t *p = b + 8;
        if ((p[0] & 0x3F) == 0x2A && p[1] >= 14) {
          say(d,
              "  reads CD-R %s, CD-RW %s, DVD-ROM %s; CD-DA accurate %s, C2 %s, R-W sub %s; max speed %u kB/s, "
              "buffer %u kB",
              (p[2] & 1) ? "yes" : "no", (p[2] & 2) ? "yes" : "no", (p[2] & 8) ? "yes" : "no",
              (p[5] & 2) ? "yes" : "no", (p[5] & 0x10) ? "yes" : "no", (p[5] & 0x04) ? "yes" : "no", rip_be16(p + 8),
              rip_be16(p + 12));
          finding(d, "accurate audio stream: %s", (p[5] & 2) ? "yes" : "no");
        }
      }
    }
  }

  // Read speeds the drive will do, and its buffer.
  memset(cdb, 0, sizeof(cdb));
  cdb[0] = 0xAC;
  rip_put_be16(cdb + 8, 32);
  cdb[10] = 0x00;  // performance, read
  if (cmd(d, "get performance (read)", cdb, 12, RIP_DIR_IN, 8 + 32 * 16, SHORT_MS, 8 + 32 * 16, NULL) == RIP_OK) {
    uint32_t len = rip_be32(b);
    uint32_t at;
    for (at = 8; at + 16 <= len + 4 && at < 8 + 32 * 16; at += 16) {
      say(d, "  lba %u at %u kB/s .. lba %u at %u kB/s", rip_be32(b + at), rip_be32(b + at + 4), rip_be32(b + at + 8),
          rip_be32(b + at + 12));
    }
  }
  memset(cdb, 0, sizeof(cdb));
  cdb[0] = 0x5C;
  rip_put_be16(cdb + 7, 12);
  if (cmd(d, "read buffer capacity", cdb, 10, RIP_DIR_IN, 12, SHORT_MS, 12, NULL) == RIP_OK) {
    say(d, "  buffer %u bytes, %u free", rip_be32(b + 4), rip_be32(b + 8));
  }
}

/* The disc */

static void disc_identity(struct diag *d) {
  uint8_t cdb[12];
  uint8_t *b = d->buf;
  struct rip_sense sense;
  int tries;

  section(d, "Disc");
  for (tries = 0; tries < 20; tries++) {
    memset(&sense, 0, sizeof(sense));
    if (mmc_test_unit_ready(d->t, &sense) == RIP_OK) {
      break;
    }
    if (sense.key == 0x02 && sense.asc == 0x3A) {
      break;  // "no medium": but a drive says that of discs it can't read too
    }
    usleep(1000 * 1000);
  }
  if (mmc_current_profile(d->t, &d->profile, NULL) == RIP_OK) {
    say(d, "profile %04x (%s)", d->profile, mmc_profile_name(d->profile));
  }
  if (sense.key) {
    say(d, "not ready: sense %02x/%02x/%02x %s", sense.key, sense.asc, sense.ascq, sense_name(&sense));
  }
  if (!d->profile && sense.key == 0x02 && sense.asc == 0x3A) {
    say(d, "no disc in the drive");
    finding(d, "disc: none (insert one and test again)");
    return;
  }
  if (sense.key) {
    // A disc the drive sees (it gives a profile) but won't read: a game
    // console's own format, most likely. Everything below is tried anyway.
    if (d->profile) {
      finding(d, "disc: seen as %s, never ready (sense %02x/%02x/%02x %s)", mmc_profile_name(d->profile), sense.key,
              sense.asc, sense.ascq, sense_name(&sense));
    } else {
      finding(d, "disc: rejected outright, no profile (sense %02x/%02x/%02x %s)", sense.key, sense.asc, sense.ascq,
              sense_name(&sense));
    }
  }
  d->is_cd = mmc_profile_is_cd(d->profile);
  d->is_dvd = mmc_profile_is_dvd(d->profile);
  d->is_bd = mmc_profile_is_bd(d->profile);
  if (mmc_read_capacity(d->t, &d->last_lba, &d->block_len) == RIP_OK) {
    say(d, "capacity: last lba %u, %u-byte blocks (%.1f MB)", d->last_lba, d->block_len,
        (double)(d->last_lba + 1) * d->block_len / 1e6);
  }
  memset(cdb, 0, sizeof(cdb));
  cdb[0] = 0x51;
  rip_put_be16(cdb + 7, 2048);
  if (cmd(d, "read disc information", cdb, 10, RIP_DIR_IN, 2048, SHORT_MS, 64, NULL) == RIP_OK) {
    say(d, "  status %u, sessions %u, disc type %02x", b[2] & 3, b[4] | (b[9] << 8), b[8]);
  }
  memset(cdb, 0, sizeof(cdb));
  cdb[0] = 0x52;
  cdb[1] = 0x01;  // by track number
  cdb_lba(cdb, 2, 1);
  rip_put_be16(cdb + 7, 48);
  cmd(d, "read track information 1", cdb, 10, RIP_DIR_IN, 48, SHORT_MS, 48, NULL);
  snprintf(d->sum->disc, sizeof(d->sum->disc), "%s, %.0f MB", d->profile ? mmc_profile_name(d->profile) : "unknown",
           (double)(d->last_lba + 1) * (d->block_len ? d->block_len : 2048) / 1e6);
}

static void cd_tables(struct diag *d) {
  static const struct {
    uint8_t format;
    uint8_t msf;
    const char *name;
  } tocs[] = {{0, 0, "toc"}, {0, 1, "toc msf"}, {1, 0, "session info"}, {2, 1, "full toc"},
              {3, 1, "pma"}, {4, 1, "atip"},    {5, 0, "cd-text"}};
  uint8_t cdb[12];
  size_t i;

  section(d, "CD tables");
  for (i = 0; i < RIP_ARRAY_LEN(tocs); i++) {
    memset(cdb, 0, sizeof(cdb));
    cdb[0] = 0x43;
    cdb[1] = tocs[i].msf ? 0x02 : 0;
    cdb[2] = tocs[i].format;
    rip_put_be16(cdb + 7, 8192);
    cmd(d, tocs[i].name, cdb, 10, RIP_DIR_IN, 8192, SHORT_MS, 8192, NULL);
  }
  if (cd_read_toc(d->t, &d->toc) == RIP_OK) {
    d->have_toc = 1;
    cd_find_pregaps(d->t, &d->toc);
    say(d, "tracks %u, sessions %u, disc type %02x, lead-out %u", d->toc.count, d->toc.sessions, d->toc.type,
        d->toc.leadout);
    for (i = 0; i < d->toc.count; i++) {
      struct cd_track *t = &d->toc.tracks[i];
      cd_detect_mode(d->t, t);
      say(d, "  track %02u session %u %s ctrl %x start %u pregap %u length %u", t->number, t->session,
          cd_track_is_audio(t)  ? "audio"
          : t->mode == CD_MODE1 ? "mode1"
                                : "mode2",
          t->control, t->start, t->pregap, t->length);
    }
    snprintf(d->sum->disc, sizeof(d->sum->disc), "%s, %u track(s), %u session(s)", mmc_profile_name(d->profile),
             d->toc.count, d->toc.sessions);
  }
}

// DVD and BD: the structures the drive lists for this disc, each read. Key
// material (CSS disc keys, AACS IDs and keys) is left alone.
static void disc_structures(struct diag *d) {
  uint8_t cdb[12];
  uint8_t list[2048];
  uint8_t media = d->is_bd ? 1 : 0;
  uint32_t len;
  uint32_t at;

  section(d, "Disc structures");
  memset(cdb, 0, sizeof(cdb));
  cdb[0] = 0xAD;
  cdb[1] = media;
  cdb[7] = 0xFF;
  rip_put_be16(cdb + 8, sizeof(list));
  if (cmd(d, "structure list", cdb, 12, RIP_DIR_IN, sizeof(list), SHORT_MS, sizeof(list), NULL) != RIP_OK) {
    return;
  }
  memcpy(list, d->buf, sizeof(list));
  len = rip_be16(list) + 2;
  for (at = 4; at + 4 <= len && at + 4 <= sizeof(list); at += 4) {
    uint8_t format = list[at];
    char name[48];
    if (format == 0xFF) {
      continue;
    }
    // CSS disc key (02), AACS (80-86): not read.
    if ((!d->is_bd && format == 0x02) || (format >= 0x80 && format <= 0x86) || (d->is_bd && format == 0x81)) {
      say(d, "  format %02x listed, not read (key material)", format);
      continue;
    }
    snprintf(name, sizeof(name), "structure %02x", format);
    memset(cdb, 0, sizeof(cdb));
    cdb[0] = 0xAD;
    cdb[1] = media;
    cdb[7] = format;
    rip_put_be16(cdb + 8, rip_be16(list + at + 2) ? rip_be16(list + at + 2) : 4100);
    cmd(d, name, cdb, 12, RIP_DIR_IN, rip_be16(list + at + 2) ? rip_be16(list + at + 2) : 4100, SHORT_MS, 4100, NULL);
    if (format == 0x00 && !d->is_bd) {
      const uint8_t *p = d->buf + 4;
      uint32_t start = rip_be32(p + 4) & 0xFFFFFF;
      uint32_t end = rip_be32(p + 8) & 0xFFFFFF;
      uint32_t end0 = rip_be32(p + 12) & 0xFFFFFF;
      say(d, "  physical: book %x v%u, layers %u, %s track path, data %06x..%06x, layer 0 ends %06x", p[0] >> 4,
          p[0] & 0x0F, ((p[2] >> 5) & 3) + 1, (p[2] & 0x10) ? "opposite" : "parallel", start, end, end0);
    }
  }
}

/* Reading */

static void read_data(struct diag *d) {
  uint8_t cdb[12];
  uint32_t lbas[3];
  uint32_t sizes[] = {16, 30, 32, 64, 128, 256};
  uint32_t best = 0;
  uint64_t ms;
  size_t i;

  section(d, "Reading data");
  if (!d->last_lba) {
    // No capacity: what does it do when asked anyway?
    uint32_t tries[] = {0, 16, 0x1000, 0x10000};
    int any = 0;
    say(d, "no capacity given; reading a few places anyway");
    for (i = 0; i < RIP_ARRAY_LEN(tries); i++) {
      char name[48];
      memset(cdb, 0, sizeof(cdb));
      cdb[0] = 0x28;
      cdb_lba(cdb, 2, tries[i]);
      cdb[8] = 1;
      snprintf(name, sizeof(name), "read10 lba %u (no capacity)", tries[i]);
      any |= cmd(d, name, cdb, 10, RIP_DIR_IN, 2048, READ_MS, 2048, NULL) == RIP_OK;
    }
    memset(cdb, 0, sizeof(cdb));
    cdb[0] = 0x3C;
    cdb[1] = 0x02;
    cdb[8] = 0x08;
    cmd(d, "read buffer data 2048 (no capacity)", cdb, 10, RIP_DIR_IN, 2048, SHORT_MS, 2048, NULL);
    finding(d, "reads without a capacity: %s", any ? "some" : "none");
    return;
  }
  // On a CD, the first 16 sectors and much of a game's middle can be Mode 2
  // Form 2, which plain READ refuses; its frames are tested below instead.
  lbas[0] = d->is_cd ? 16 : 0;
  lbas[1] = d->last_lba / 2;
  lbas[2] = d->last_lba;
  for (i = 0; i < 3; i++) {
    char name[48];
    memset(cdb, 0, sizeof(cdb));
    cdb[0] = 0x28;
    cdb_lba(cdb, 2, lbas[i]);
    cdb[8] = 1;
    snprintf(name, sizeof(name), "read10 lba %u", lbas[i]);
    cmd(d, name, cdb, 10, RIP_DIR_IN, 2048, READ_MS, i == 0 ? 2048 : 0, NULL);
  }
  // How much one command may read.
  for (i = 0; i < RIP_ARRAY_LEN(sizes) && !d->is_cd; i++) {
    char name[48];
    memset(cdb, 0, sizeof(cdb));
    cdb[0] = 0xA8;
    cdb_lba(cdb, 2, 0);
    cdb_lba(cdb, 6, sizes[i]);
    snprintf(name, sizeof(name), "read12 %u sectors", sizes[i]);
    if (cmd(d, name, cdb, 12, RIP_DIR_IN, (size_t)sizes[i] * 2048, READ_MS, 0, NULL) == RIP_OK) {
      best = sizes[i];
    }
  }
  if (!d->is_cd) {
    finding(d, "largest read: %u sectors (%u KiB)", best, best * 2);
  }
  // Past the end: refused by a stock drive; an unlocked one (Kreon,
  // LibreDrive, OmniDrive) may go on into what's hidden.
  {
    uint32_t past[] = {d->last_lba + 1, d->last_lba + 16, d->last_lba + 1000};
    int any = 0;
    for (i = 0; i < 3; i++) {
      char name[48];
      memset(cdb, 0, sizeof(cdb));
      cdb[0] = 0x28;
      cdb_lba(cdb, 2, past[i]);
      cdb[8] = 1;
      snprintf(name, sizeof(name), "read10 past end +%u", past[i] - d->last_lba);
      any |= cmd(d, name, cdb, 10, RIP_DIR_IN, 2048, READ_MS, 2048, NULL) == RIP_OK;
    }
    finding(d, "reads past the end: %s", any ? "yes (unlocked?)" : "no");
  }
  // The drive's cache: READ BUFFER of what the last read left there.
  {
    int ok;
    memset(cdb, 0, sizeof(cdb));
    cdb[0] = 0x28;
    cdb[8] = 1;
    cmd(d, "read10 lba 0 (for the cache)", cdb, 10, RIP_DIR_IN, 2048, READ_MS, 0, NULL);
    memset(cdb, 0, sizeof(cdb));
    cdb[0] = 0x3C;
    cdb[1] = 0x03;  // descriptor
    cdb[8] = 4;
    cmd(d, "read buffer descriptor", cdb, 10, RIP_DIR_IN, 4, SHORT_MS, 4, NULL);
    memset(cdb, 0, sizeof(cdb));
    cdb[0] = 0x3C;
    cdb[1] = 0x02;  // data
    cdb[8] = 0x08;  // 2048 bytes
    ok = cmd(d, "read buffer data 2048", cdb, 10, RIP_DIR_IN, 2048, SHORT_MS, 2048, NULL) == RIP_OK;
    finding(d, "cache read (READ BUFFER): %s", ok ? "yes" : "no");
  }
  // Speed at the start, the middle and the end (CDs: see the frames).
  if (!d->is_cd) {
    uint32_t chunk = best >= 32 ? 32 : best ? best : 16;
    uint32_t total = 8192;  // 16 MB a place
    for (i = 0; i < 3; i++) {
      uint32_t start = i == 0 ? 0 : i == 1 ? d->last_lba / 2 : (d->last_lba > total ? d->last_lba - total : 0);
      uint32_t done = 0;
      uint64_t t0 = rip_now_ms();
      struct rip_sense sense;
      int ok = 1;
      memset(&sense, 0, sizeof(sense));
      while (ok && done < total && start + done + chunk <= d->last_lba + 1 && !stopped(d)) {
        if (mmc_read12(d->t, start + done, chunk, d->buf, 2048, &sense) != RIP_OK) {
          ok = 0;
        }
        done += chunk;
      }
      ms = rip_now_ms() - t0;
      if (!ok && sense.key == 0x05 && sense.asc == 0x6F) {
        // CSS-scrambled sectors need the authentication this test doesn't do.
        say(d, "speed at lba %u: not timed, the sectors there are CSS-scrambled (sense %02x/%02x/%02x)", start,
            sense.key, sense.asc, sense.ascq);
        continue;
      }
      say(d, "speed at lba %u: %u sectors in %llu ms, %.2f MB/s%s", start, done, (unsigned long long)ms,
          ms ? (double)done * 2048 / 1e3 / (double)ms : 0.0, ok ? "" : " (a read failed)");
      if (ok && (i == 0 || i == 1)) {
        finding(d, "read speed at the %s: %.2f MB/s", i ? "middle" : "start",
                ms ? (double)done * 2048 / 1e3 / (double)ms : 0.0);
      }
    }
  }
}

// What the pass-through takes in one command: the same reads of 60 to 64 KiB
// into a page-aligned buffer and one 16 bytes in. If only the aligned ones
// go, the limit is pages mapped (16 of 4 KiB), not bytes.
static void transfer_limits(struct diag *d) {
  static const uint32_t counts[] = {30, 31, 32};  // 2048-byte units: 60, 62, 64 KiB
  uint8_t cdb[12];
  uint32_t lba = d->is_cd ? 16 : 0;
  int k;
  size_t i;
  int ok[2][3];

  section(d, "Transfer limits");
  if (d->is_cd && d->have_toc && d->toc.count && cd_track_is_audio(&d->toc.tracks[0])) {
    say(d, "first track is audio: 2048-byte reads don't apply");
    return;
  }
  for (k = 0; k < 2; k++) {
    d->buf = d->base + (k ? 16 : 0);
    for (i = 0; i < RIP_ARRAY_LEN(counts); i++) {
      char name[64];
      memset(cdb, 0, sizeof(cdb));
      if (d->is_cd) {
        cdb[0] = 0xBE;  // user data only: 2048 bytes a frame
        cdb_lba(cdb, 2, lba);
        cdb[8] = (uint8_t)counts[i];
        cdb[9] = 0x10;
      } else {
        cdb[0] = 0xA8;
        cdb_lba(cdb, 2, lba);
        cdb_lba(cdb, 6, counts[i]);
      }
      snprintf(name, sizeof(name), "%u KiB, buffer %s", counts[i] * 2, k ? "+16" : "aligned");
      ok[k][i] = cmd(d, name, cdb, 12, RIP_DIR_IN, (size_t)counts[i] * 2048, READ_MS, 0, NULL);
    }
  }
  d->buf = d->base;
  // ok, refused by the drive (its own reasons), or too big for the pass-through.
#define T(r) ((r) == RIP_OK ? "ok" : (r) == RIP_ESENSE ? "drive said no" : "too big")
  finding(d, "transfers, aligned: 60 KiB %s, 62 %s, 64 %s; 16 bytes in: 60 %s, 62 %s, 64 %s", T(ok[0][0]), T(ok[0][1]),
          T(ok[0][2]), T(ok[1][0]), T(ok[1][1]), T(ok[1][2]));
#undef T
}

static void read_cd(struct diag *d) {
  static const struct {
    uint8_t b9;
    uint8_t b10;
    size_t len;
    const char *name;
  } modes[] = {
      {0x10, 0, 2048, "user data"},     {0xF8, 0, 2352, "raw"},
      {0xFA, 0, 2646, "raw + c2"},      {0xFC, 0, 2648, "raw + c2 + block"},
      {0xF8, 1, 2448, "raw + sub p-w"}, {0xF8, 2, 2368, "raw + sub q"},
      {0xF8, 4, 2448, "raw + sub r-w"}, {0xFA, 1, 2742, "raw + c2 + sub"},
  };
  uint8_t cdb[12];
  uint32_t data_lba = 0;
  uint32_t audio_lba = 0;
  int has_data = 0;
  int has_audio = 0;
  int c2 = 0;
  int sub = 0;
  size_t i;

  section(d, "Reading CD frames");
  if (!d->have_toc) {
    say(d, "no TOC: skipped");
    return;
  }
  for (i = 0; i < d->toc.count; i++) {
    if (cd_track_is_audio(&d->toc.tracks[i]) && !has_audio) {
      audio_lba = d->toc.tracks[i].start + 10;
      has_audio = 1;
    }
    if (!cd_track_is_audio(&d->toc.tracks[i]) && !has_data) {
      data_lba = d->toc.tracks[i].start + 16;
      has_data = 1;
    }
  }
  for (i = 0; i < RIP_ARRAY_LEN(modes); i++) {
    int k;
    for (k = 0; k < 2; k++) {
      char name[64];
      int rc;
      if ((k == 0 && !has_data) || (k == 1 && !has_audio)) {
        continue;
      }
      memset(cdb, 0, sizeof(cdb));
      cdb[0] = 0xBE;
      cdb_lba(cdb, 2, k ? audio_lba : data_lba);
      cdb[8] = 1;
      cdb[9] = modes[i].b9;
      cdb[10] = modes[i].b10;
      snprintf(name, sizeof(name), "read cd %s %s", k ? "audio" : "data", modes[i].name);
      rc = cmd(d, name, cdb, 12, RIP_DIR_IN, modes[i].len, READ_MS, modes[i].len, NULL);
      if (rc == RIP_OK && (modes[i].b9 & 0x06)) {
        c2 = 1;
      }
      if (rc == RIP_OK && modes[i].b10 == 1) {
        sub = 1;
      }
    }
  }
  finding(d, "READ CD with C2: %s, raw subchannel: %s", c2 ? "yes" : "no", sub ? "yes" : "no");
  // Frames per command.
  {
    uint32_t counts[] = {24, 26, 27, 32, 64};
    uint32_t best = 0;
    uint32_t start = has_data ? data_lba : audio_lba;
    for (i = 0; i < RIP_ARRAY_LEN(counts); i++) {
      char name[48];
      memset(cdb, 0, sizeof(cdb));
      cdb[0] = 0xBE;
      cdb_lba(cdb, 2, start);
      cdb[8] = (uint8_t)counts[i];
      cdb[9] = 0xF8;
      snprintf(name, sizeof(name), "read cd raw %u frames", counts[i]);
      if (cmd(d, name, cdb, 12, RIP_DIR_IN, (size_t)counts[i] * 2352, READ_MS, 0, NULL) == RIP_OK) {
        best = counts[i];
      }
    }
    finding(d, "largest READ CD: %u frames", best);
  }
  // Overreading: before LBA 0 (the lead-in) and past the lead-out, which
  // offset-corrected audio needs.
  {
    int32_t spots[] = {-1,
                       -150,
                       (int32_t)d->toc.leadout,
                       (int32_t)d->toc.leadout + 1,
                       (int32_t)d->toc.leadout + 10,
                       (int32_t)d->toc.leadout + 100};
    int lead_in = 0;
    int lead_out = 0;
    for (i = 0; i < RIP_ARRAY_LEN(spots); i++) {
      char name[48];
      memset(cdb, 0, sizeof(cdb));
      cdb[0] = 0xBE;
      cdb_lba(cdb, 2, (uint32_t)spots[i]);
      cdb[8] = 1;
      cdb[9] = 0xF8;
      snprintf(name, sizeof(name), "read cd at %d", spots[i]);
      if (cmd(d, name, cdb, 12, RIP_DIR_IN, 2352, READ_MS, 0, NULL) == RIP_OK) {
        if (spots[i] < 0) {
          lead_in = 1;
        } else {
          lead_out = 1;
        }
      }
    }
    finding(d, "lead-in overread: %s, lead-out overread: %s", lead_in ? "yes" : "no", lead_out ? "yes" : "no");
  }
  // Plextor's D8 (raw, scrambled reads) and the sector-type check: data
  // read as CD-DA, which some drives allow (scrambled data).
  {
    int d8 = 0;
    int as_audio = 0;
    // Vendor commands go only to their own vendor's drives.
    if (!strncmp(d->sum->drive, "PLEXTOR", 7)) {
      memset(cdb, 0, sizeof(cdb));
      cdb[0] = 0xD8;
      cdb_lba(cdb, 2, has_data ? data_lba : audio_lba);
      cdb_lba(cdb, 6, 1);
      cdb[10] = 0x00;
      d8 = cmd(d, "plextor d8 raw", cdb, 12, RIP_DIR_IN, 2352, READ_MS, 2352, NULL) == RIP_OK;
      cdb[10] = 0x02;
      cmd(d, "plextor d8 raw + sub", cdb, 12, RIP_DIR_IN, 2448, READ_MS, 2448, NULL);
    }
    if (has_data) {
      memset(cdb, 0, sizeof(cdb));
      cdb[0] = 0xBE;
      cdb[1] = 0x04;  // expect CD-DA
      cdb_lba(cdb, 2, data_lba);
      cdb[8] = 1;
      cdb[9] = 0xF8;
      as_audio = cmd(d, "read cd data track as cd-da", cdb, 12, RIP_DIR_IN, 2352, READ_MS, 2352, NULL) == RIP_OK;
    }
    finding(d, "Plextor D8: %s, data read as audio (scrambled): %s",
            !strncmp(d->sum->drive, "PLEXTOR", 7) ? (d8 ? "yes" : "no") : "not a Plextor", as_audio ? "yes" : "no");
  }
  // Q subchannel at a few places.
  {
    int good = 0;
    uint32_t at[] = {100, d->toc.leadout / 2, d->toc.leadout > 200 ? d->toc.leadout - 100 : 0};
    uint8_t q[12];
    for (i = 0; i < 3; i++) {
      if (cd_read_q(d->t, at[i], q) == RIP_OK) {
        good++;
        say(d, "q at %u: track %x index %x, crc ok", at[i], q[1], q[2]);
      } else {
        say(d, "q at %u: unreadable or bad crc", at[i]);
      }
    }
    finding(d, "Q subchannel: %d of 3 good", good);
  }
  // Speed: 2000 raw frames at the start and near the end, with C2 if it works.
  for (i = 0; i < 2; i++) {
    uint32_t start = i == 0 ? 0 : (d->toc.leadout > 2100 ? d->toc.leadout - 2100 : 0);
    uint32_t done = 0;
    unsigned flags = c2 ? CD_READ_C2 : 0;
    uint32_t chunk = (uint32_t)(57344 / cd_frame_stride(flags));
    uint64_t t0 = rip_now_ms();
    uint64_t ms;
    int ok = 1;
    while (ok && done < 2000 && !stopped(d)) {
      ok = cd_read_frames(d->t, start + done, chunk, flags, d->buf, NULL) == RIP_OK;
      done += chunk;
    }
    ms = rip_now_ms() - t0;
    say(d, "cd speed at %u: %u frames in %llu ms, %.2f MB/s%s%s", start, done, (unsigned long long)ms,
        ms ? (double)done * 2352 / 1e3 / (double)ms : 0.0, c2 ? " (with c2)" : "", ok ? "" : " (a read failed)");
    if (i == 1) {
      finding(d, "CD read speed near the end: %.2f MB/s", ms ? (double)done * 2352 / 1e3 / (double)ms : 0.0);
    }
  }
}

// What custom or vendor firmware adds, each only where it belongs: OmniDrive's
// raw read (C0) on OmniDrive firmware, MediaTek's cache read (F1 06) on the
// LG drives redumper lists as MediaTek, and the stock LG raw-DVD trick
// (READ(12) with FUA, then READ BUFFER mode 02 buffer 0) that dumps Nintendo
// discs on some drives. All reads. (Never sent: WRITE BUFFER, READ BUFFER
// modes beyond 02/03 on LG, where 3C 0D is a setting and can save to flash.)
static int mediatek_listed(const struct diag *d) {
  static const char *const products[] = {"BD-RE BP50NB40", "BD-RE BU40N"};
  size_t i;

  for (i = 0; d->lg && i < RIP_ARRAY_LEN(products); i++) {
    if (!strncmp(d->product, products[i], strlen(products[i]))) {
      return 1;
    }
  }
  return 0;
}

static void vendor_firmware(struct diag *d) {
  uint8_t cdb[12];
  uint32_t lba = 0;
  size_t i;

  section(d, "Drive firmware");
  if (!d->omnidrive && !d->lg) {
    say(d, "not an LG or OmniDrive drive: nothing to try");
    return;
  }
  if (d->is_cd && d->have_toc) {
    for (i = 0; i < d->toc.count; i++) {
      if (!cd_track_is_audio(&d->toc.tracks[i])) {
        lba = d->toc.tracks[i].start + 16;
        break;
      }
    }
  }
  if (d->omnidrive && d->profile) {
    // C0: byte 1 disc type (0 CD, 1 DVD, 2 BD), 0x10 descramble (DVD/BD);
    // byte 10 on CDs: 1 raw P-W, 4 C2. DVD frames are 2064 bytes, BD 2070
    // (2072 by the README: room for both).
    int ok = 0;
    memset(cdb, 0, sizeof(cdb));
    cdb[0] = 0xC0;
    if (d->is_cd) {
      cdb_lba(cdb, 2, lba);
      cdb_lba(cdb, 6, 1);
      cdb[10] = 0x05;
      ok = cmd(d, "omnidrive c0 cd raw + sub + c2", cdb, 12, RIP_DIR_IN, 2742, READ_MS, 2742, NULL) == RIP_OK;
    } else {
      cdb[1] = d->is_bd ? 0x02 : 0x01;
      cdb_lba(cdb, 2, 0);
      cdb_lba(cdb, 6, 1);
      ok = cmd(d, d->is_bd ? "omnidrive c0 bd raw" : "omnidrive c0 dvd raw (scrambled)", cdb, 12, RIP_DIR_IN,
               d->is_bd ? 2072 : 2064, READ_MS, d->is_bd ? 2072 : 2064, NULL) == RIP_OK;
      cdb[1] |= 0x10;
      cmd(d, d->is_bd ? "omnidrive c0 bd raw descrambled" : "omnidrive c0 dvd raw descrambled", cdb, 12, RIP_DIR_IN,
          d->is_bd ? 2072 : 2064, READ_MS, d->is_bd ? 2072 : 2064, NULL);
      cdb_lba(cdb, 2, 16);
      cmd(d, "omnidrive c0 raw lba 16 descrambled", cdb, 12, RIP_DIR_IN, d->is_bd ? 2072 : 2064, READ_MS,
          d->is_bd ? 2072 : 2064, NULL);
    }
    finding(d, "OmniDrive raw read (C0): %s", ok ? "yes" : "no");
  }
  // A Nintendo disc names itself in its physical format: the layer
  // descriptor's first byte is FF (redumper's dvd_dump).
  if (d->is_dvd || (d->profile && !d->is_cd && !d->is_bd)) {
    memset(cdb, 0, sizeof(cdb));
    cdb[0] = 0xAD;
    rip_put_be16(cdb + 8, 2052);
    if (cmd(d, "physical format (nintendo check)", cdb, 12, RIP_DIR_IN, 2052, SHORT_MS, 0, NULL) == RIP_OK &&
        d->buf[4] == 0xFF) {
      finding(d, "disc: Nintendo (GameCube/Wii) by its physical format");
    }
  }
  if (mediatek_listed(d) && d->is_cd) {
    // MediaTek cache, 2 entries of 0xB00: main data, sub, C2 of what was read.
    memset(cdb, 0, sizeof(cdb));
    cdb[0] = 0x28;
    cdb_lba(cdb, 2, lba);
    cdb[8] = 1;
    cmd(d, "read10 (fill the cache)", cdb, 10, RIP_DIR_IN, 2048, READ_MS, 0, NULL);
    memset(cdb, 0, sizeof(cdb));
    cdb[0] = 0xF1;
    cdb[1] = 0x06;
    cdb_lba(cdb, 2, 0);
    cdb_lba(cdb, 6, 0x1600);
    finding(
        d, "MediaTek cache read (F1): %s",
        cmd(d, "mediatek f1 06 cache", cdb, 10, RIP_DIR_IN, 0x1600, SHORT_MS, 0x1600, NULL) == RIP_OK ? "yes" : "no");
  }
  if (d->lg && (d->is_dvd || (!d->is_cd && !d->is_bd))) {
    // DiscImageCreator's raw DVD on stock LG firmware: 16 sectors into the
    // cache with FUA, read back as 2384-byte raw frames (scrambled, with ECC).
    int ok;
    memset(cdb, 0, sizeof(cdb));
    cdb[0] = 0xA8;
    cdb[1] = 0x08;
    cdb_lba(cdb, 2, 0);
    cdb_lba(cdb, 6, 16);
    cmd(d, "read12 fua 16 (fill the cache)", cdb, 12, RIP_DIR_IN, 16 * 2048, READ_MS, 0, NULL);
    memset(cdb, 0, sizeof(cdb));
    cdb[0] = 0x3C;
    cdb[1] = 0x02;
    cdb[6] = (uint8_t)((2384 * 16) >> 16);
    cdb[7] = (uint8_t)((2384 * 16) >> 8);
    cdb[8] = (uint8_t)(2384 * 16);
    ok = cmd(d, "read buffer 02 raw dvd frames", cdb, 10, RIP_DIR_IN, 2384 * 16, SHORT_MS, 2384 * 2, NULL) == RIP_OK;
    finding(d, "LG raw DVD through the cache (3C 02): %s", ok ? "yes" : "no");
  }
}

static void protection(struct diag *d) {
  uint8_t cdb[12];

  section(d, "Copy protection");
  if (d->is_dvd) {
    uint8_t cps = 0;
    if (mmc_dvd_copyright(d->t, &cps, NULL) == RIP_OK) {
      say(d, "copyright: %s", cps == 1 ? "CSS/CPPM" : cps ? "other" : "none");
      finding(d, "DVD copy protection: %s", cps == 1 ? "CSS" : cps ? "other" : "none");
    }
    // An authentication grant, given straight back: no key is exchanged.
    memset(cdb, 0, sizeof(cdb));
    cdb[0] = 0xA4;
    cdb[9] = 8;
    cdb[10] = 0x00;  // AGID for CSS
    if (cmd(d, "report key agid (css)", cdb, 12, RIP_DIR_IN, 8, SHORT_MS, 0, NULL) == RIP_OK) {
      uint8_t agid = d->buf[7] >> 6;
      memset(cdb, 0, sizeof(cdb));
      cdb[0] = 0xA4;
      cdb[10] = (uint8_t)((agid << 6) | 0x3F);  // invalidate it
      cmd(d, "invalidate agid", cdb, 12, RIP_DIR_NONE, 0, SHORT_MS, 0, NULL);
    }
  } else if (d->is_bd) {
    memset(cdb, 0, sizeof(cdb));
    cdb[0] = 0xA4;
    cdb[7] = 0x02;  // AACS key class
    cdb[9] = 8;
    cdb[10] = 0x00;
    if (cmd(d, "report key agid (aacs)", cdb, 12, RIP_DIR_IN, 8, SHORT_MS, 0, NULL) == RIP_OK) {
      uint8_t agid = d->buf[7] >> 6;
      memset(cdb, 0, sizeof(cdb));
      cdb[0] = 0xA4;
      cdb[7] = 0x02;
      cdb[10] = (uint8_t)((agid << 6) | 0x3F);
      cmd(d, "invalidate agid", cdb, 12, RIP_DIR_NONE, 0, SHORT_MS, 0, NULL);
      finding(d, "AACS authentication offered: yes");
    } else {
      finding(d, "AACS authentication offered: no");
    }
  } else {
    say(d, "nothing to check for this disc");
  }
}

// Nintendo optical discs, read the way normal discs are: a stock drive
// refuses, which is the answer; one that answers shows the header.
static void game_headers(struct diag *d) {
  uint8_t cdb[12];

  section(d, "Game disc headers");
  if (!d->is_dvd && !d->is_bd && d->profile) {
    say(d, "not a DVD or BD");
    return;
  }
  memset(cdb, 0, sizeof(cdb));
  cdb[0] = 0x28;
  cdb[8] = 1;
  if (cmd(d, "read10 lba 0 (headers)", cdb, 10, RIP_DIR_IN, 2048, READ_MS, 2048, NULL) == RIP_OK) {
    const uint8_t *b = d->buf;
    if (rip_be32(b + 0x18) == 0x5D1C9EA3) {
      finding(d, "Wii disc header readable: %.6s '%.32s'", b, b + 0x20);
    } else if (rip_be32(b + 0x1C) == 0xC2339F3D) {
      finding(d, "GameCube disc header readable: %.6s '%.32s'", b, b + 0x20);
    } else if (!memcmp(b, "MICROSOFT*XBOX*MEDIA", 20)) {
      finding(d, "Xbox XDVDFS at sector 0");
    } else {
      say(d, "sector 0 is no Nintendo or Xbox header");
    }
  }
}

int diag_run(struct rip_transport *t, const char *out_dir, diag_progress_fn progress, void *ud,
             const volatile int *cancel, struct diag_summary *summary) {
  struct diag d;
  char path[1100];

  memset(&d, 0, sizeof(d));
  memset(summary, 0, sizeof(*summary));
  d.t = t;
  d.dir = out_dir;
  d.progress = progress;
  d.ud = ud;
  d.cancel = cancel;
  d.sum = summary;
  snprintf(path, sizeof(path), "%s/raw", out_dir);
  if (rip_mkdir_p(path) != 0) {
    RIP_ERROR("diag: can't create %s", path);
    return RIP_ERR;
  }
  snprintf(path, sizeof(path), "%s/report.txt", out_dir);
  d.rep = fopen(path, "w");
  if (posix_memalign((void **)&d.base, 4096, BUF_BYTES + 4096) != 0) {
    d.base = NULL;
  }
  d.buf = d.base;
  if (!d.rep || !d.base) {
    if (d.rep) {
      fclose(d.rep);
    }
    free(d.base);
    return RIP_ERR;
  }
  say(&d, "ripper drive diagnostic, ripper %s", RIPPER_VERSION);
  say(&d, "[ok] the drive took it, [no] it answered with sense data (refused), [err] no answer at all");

  drive_identity(&d);
  if (!stopped(&d)) {
    disc_identity(&d);
  }
  if (!stopped(&d) && d.is_cd) {
    cd_tables(&d);
  } else {
    d.step++;
  }
  if (!stopped(&d) && (d.is_dvd || d.is_bd)) {
    disc_structures(&d);
  } else {
    d.step++;
  }
  if (!stopped(&d) && d.profile) {
    read_data(&d);
  } else {
    d.step++;
  }
  if (!stopped(&d) && d.profile) {
    transfer_limits(&d);
  } else {
    d.step++;
  }
  if (!stopped(&d) && d.is_cd) {
    read_cd(&d);
  } else {
    d.step++;
  }
  if (!stopped(&d)) {
    vendor_firmware(&d);
  }
  if (!stopped(&d) && d.profile) {
    protection(&d);
  }
  if (!stopped(&d) && d.profile) {
    game_headers(&d);
  }
  say(&d, "\n== Findings ==\n%s", summary->findings);
  say(&d, "%d commands, %d taken", summary->commands, summary->answered);
  fclose(d.rep);
  free(d.base);
  if (progress) {
    progress(ud, "done", STEPS, STEPS);
  }
  return stopped(&d) ? RIP_ECANCEL : RIP_OK;
}
