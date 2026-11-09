// A read benchmark for a drive with a disc in it: how fast each way of
// reading goes at a few request sizes, and how fast this CPU compresses, to
// tell a slow drive from a slow rip.

#include "bench.h"

#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <zlib.h>
#include <zstd.h>

#include "drives.h"
#include "platform.h"
#include "ripper/cdrom.h"
#include "ripper/common.h"
#include "ripper/log.h"
#include "ripper/mmc.h"
#include "svc.h"

#define SAMPLE_BYTES (24u << 20)

// Reads SAMPLE_BYTES from lba on in requests of chunk sectors. MB/s, or -1.
static double time_reads(struct rip_reader *r, uint64_t lba, uint32_t chunk, uint8_t *buf, int *errors) {
  uint64_t start = svc_mono_ms();
  uint64_t end_lba = lba + SAMPLE_BYTES / r->sector_size;
  uint64_t ms;

  if (end_lba > r->sector_count) {
    end_lba = r->sector_count;
  }
  while (lba < end_lba) {
    uint32_t n = end_lba - lba < chunk ? (uint32_t)(end_lba - lba) : chunk;
    if (rip_reader_read(r, lba, n, buf) != RIP_OK) {
      (*errors)++;
      return -1;
    }
    lba += n;
  }
  ms = svc_mono_ms() - start;
  return ms ? (double)SAMPLE_BYTES / 1e6 / ((double)ms / 1000.0) : 0;
}

static double time_zlib(const uint8_t *data, size_t len, int level) {
  uint64_t start = svc_mono_ms();
  uint8_t *out = malloc(compressBound(4096));
  z_stream z;
  size_t off;
  uint64_t ms;

  memset(&z, 0, sizeof(z));
  if (!out || deflateInit2(&z, level, Z_DEFLATED, -MAX_WBITS, 8, Z_DEFAULT_STRATEGY) != Z_OK) {
    free(out);
    return -1;
  }
  for (off = 0; off + 4096 <= len; off += 4096) {
    deflateReset(&z);
    z.next_in = (Bytef *)data + off;
    z.avail_in = 4096;
    z.next_out = out;
    z.avail_out = (uInt)compressBound(4096);
    deflate(&z, Z_FINISH);
  }
  deflateEnd(&z);
  free(out);
  ms = svc_mono_ms() - start;
  return ms ? (double)len / 1e6 / ((double)ms / 1000.0) : 0;
}

static double time_zstd(const uint8_t *data, size_t len, int level) {
  uint64_t start = svc_mono_ms();
  ZSTD_CCtx *c = ZSTD_createCCtx();
  size_t cap = ZSTD_compressBound(4096);
  uint8_t *out = malloc(cap);
  size_t off;
  uint64_t ms;

  if (!c || !out) {
    ZSTD_freeCCtx(c);
    free(out);
    return -1;
  }
  ZSTD_CCtx_setParameter(c, ZSTD_c_compressionLevel, level);
  for (off = 0; off + 4096 <= len; off += 4096) {
    ZSTD_compress2(c, out, cap, data + off, 4096);
  }
  ZSTD_freeCCtx(c);
  free(out);
  ms = svc_mono_ms() - start;
  return ms ? (double)len / 1e6 / ((double)ms / 1000.0) : 0;
}

// The CSS handshake's first step: ask the drive for an authentication grant
// ID (REPORT KEY, key format 0). A drive that answers can do the whole dance.
static void probe_css(struct rip_transport *t, cJSON *out) {
  uint8_t cdb[12] = {0xA4, 0, 0, 0, 0, 0, 0, 0, 0, 8, 0x00, 0};
  uint8_t buf[8];
  struct rip_sense sense;
  char text[96];
  int rc;

  memset(buf, 0, sizeof(buf));
  rc = rip_scsi(t, cdb, sizeof(cdb), RIP_DIR_IN, buf, sizeof(buf), 5000, &sense);
  if (rc == RIP_OK) {
    uint8_t agid = buf[7] >> 6;
    uint8_t inval[12] = {0xA4, 0, 0, 0, 0, 0, 0, 0, 0, 0, (uint8_t)((agid << 6) | 0x3F), 0};
    snprintf(text, sizeof(text), "granted AGID %u", agid);
    rip_scsi(t, inval, sizeof(inval), RIP_DIR_NONE, NULL, 0, 5000, NULL);  // give it back
  } else if (rc == RIP_ESENSE) {
    snprintf(text, sizeof(text), "refused, sense %02x/%02x/%02x", sense.key, sense.asc, sense.ascq);
  } else {
    snprintf(text, sizeof(text), "no answer");
  }
  cJSON_AddStringToObject(out, "css_report_key", text);
  RIP_INFO("benchmark: CSS REPORT KEY: %s", text);
}

// Why one sector at lba can't be read through pass-through.
static void probe_sector(struct rip_transport *t, uint64_t lba, cJSON *out) {
  uint8_t buf[2048];
  struct rip_sense sense;
  char text[96];
  int rc = mmc_read12(t, (uint32_t)lba, 1, buf, 2048, &sense);

  if (rc == RIP_OK) {
    snprintf(text, sizeof(text), "LBA %llu reads", (unsigned long long)lba);
  } else if (rc == RIP_ESENSE) {
    snprintf(text, sizeof(text), "LBA %llu: sense %02x/%02x/%02x", (unsigned long long)lba, sense.key, sense.asc,
             sense.ascq);
  } else {
    snprintf(text, sizeof(text), "LBA %llu: no answer", (unsigned long long)lba);
  }
  cJSON_AddStringToObject(out, "sector_probe", text);
  RIP_INFO("benchmark: %s", text);
}

static void sense_text(int rc, const struct rip_sense *sense, char *out, size_t n) {
  if (rc == RIP_OK) {
    snprintf(out, n, "ok");
  } else if (rc == RIP_ESENSE) {
    snprintf(out, n, "sense %02x/%02x/%02x", sense->key, sense->asc, sense->ascq);
  } else {
    snprintf(out, n, "no answer");
  }
}

// What the drive says about its speeds, and whether it takes a request for more.
static void probe_speed(struct rip_transport *t, uint32_t last_lba, cJSON *out) {
  uint8_t buf[256];
  struct rip_sense sense;
  char text[160];
  int rc;

  {  // GET PERFORMANCE, nominal performance of the whole disc
    uint8_t cdb[12] = {0xAC, 0, 0, 0, 0, 0, 0, 0, 0, 8, 0x00, 0};
    memset(buf, 0, sizeof(buf));
    rc = rip_scsi(t, cdb, sizeof(cdb), RIP_DIR_IN, buf, 8 + 16 * 8, 5000, &sense);
    if (rc == RIP_OK) {
      uint32_t len = rip_be32(buf);
      size_t off;
      size_t used = 0;
      text[0] = '\0';
      for (off = 8; off + 16 <= 8 + 16 * 8 && off + 16 <= len + 4; off += 16) {
        used += (size_t)snprintf(text + used, sizeof(text) - used, "%s%u KB/s@%u-%u KB/s@%u", used ? "; " : "",
                                 rip_be32(buf + off + 4), rip_be32(buf + off), rip_be32(buf + off + 12),
                                 rip_be32(buf + off + 8));
        if (used >= sizeof(text)) {
          break;
        }
      }
      if (!text[0]) {
        snprintf(text, sizeof(text), "no descriptors");
      }
    } else {
      sense_text(rc, &sense, text, sizeof(text));
    }
    cJSON_AddStringToObject(out, "get_performance", text);
    RIP_INFO("benchmark: GET PERFORMANCE: %s", text);
  }
  {  // MODE SENSE(10), page 2A: maximum and current read speed
    uint8_t cdb[10] = {0x5A, 0x08, 0x2A, 0, 0, 0, 0, 0, 255, 0};
    memset(buf, 0, sizeof(buf));
    rc = rip_scsi(t, cdb, sizeof(cdb), RIP_DIR_IN, buf, 255, 5000, &sense);
    if (rc == RIP_OK) {
      const uint8_t *pg = buf + 8 + rip_be16(buf + 6);
      snprintf(text, sizeof(text), "page %02x: max read %u KB/s, current read %u KB/s", pg[0] & 0x3F, rip_be16(pg + 8),
               rip_be16(pg + 14));
    } else {
      sense_text(rc, &sense, text, sizeof(text));
    }
    cJSON_AddStringToObject(out, "mode_page_2a", text);
    RIP_INFO("benchmark: MODE SENSE 2A: %s", text);
  }
  {  // SET STREAMING over the disc, asking for far more than any drive does
    uint8_t cdb[12] = {0xB6, 0, 0, 0, 0, 0, 0, 0, 0, 0, 28, 0};
    uint8_t pd[28];
    memset(pd, 0, sizeof(pd));
    rip_put_be32(pd + 8, last_lba);
    rip_put_be32(pd + 12, 0x10000000u);
    rip_put_be32(pd + 16, 1000);
    rip_put_be32(pd + 20, 0x10000000u);
    rip_put_be32(pd + 24, 1000);
    rc = rip_scsi(t, cdb, sizeof(cdb), RIP_DIR_OUT, pd, sizeof(pd), 5000, &sense);
    sense_text(rc, &sense, text, sizeof(text));
    cJSON_AddStringToObject(out, "set_streaming", text);
    RIP_INFO("benchmark: SET STREAMING: %s", text);
  }
  {  // SET CD SPEED to the maximum
    uint8_t cdb[12] = {0xBB, 0, 0xFF, 0xFF, 0xFF, 0xFF, 0, 0, 0, 0, 0, 0};
    rc = rip_scsi(t, cdb, sizeof(cdb), RIP_DIR_NONE, NULL, 0, 5000, &sense);
    sense_text(rc, &sense, text, sizeof(text));
    cJSON_AddStringToObject(out, "set_cd_speed", text);
    RIP_INFO("benchmark: SET CD SPEED: %s", text);
  }
}

static void add(cJSON *arr, const char *what, double mbs) {
  cJSON *o = cJSON_CreateObject();

  cJSON_AddStringToObject(o, "test", what);
  if (mbs >= 0) {
    cJSON_AddNumberToObject(o, "mb_s", (double)(int)(mbs * 10) / 10);
  } else {
    cJSON_AddStringToObject(o, "error", "failed");
  }
  cJSON_AddItemToArray(arr, o);
  RIP_INFO("benchmark: %s: %.1f MB/s", what, mbs);
}

// A CD: the TOC, raw reads with and without C2 pointers and subchannel, and
// their speed. Audio can't come through /dev/cdN, so READ CD only.
static void bench_cd(struct rip_transport *t, cJSON *out, cJSON *tests) {
  struct cd_toc toc;
  enum { FRAMES = 2048, BATCH = 16 };  // ~4.8 MB, 16 frames a request
  uint8_t *buf = malloc((size_t)BATCH * cd_frame_stride(CD_READ_C2 | CD_READ_SUB));
  static const struct {
    unsigned flags;
    const char *what;
  } modes[] = {{0, "READ CD, 2352-byte frames"},
               {CD_READ_C2, "READ CD with C2 pointers"},
               {CD_READ_SUB, "READ CD with subchannel"},
               {CD_READ_C2 | CD_READ_SUB, "READ CD with C2 and subchannel"}};
  char text[160];
  uint32_t lba;
  size_t m;

  if (!buf) {
    return;
  }
  if (cd_read_toc(t, &toc) != RIP_OK) {
    cJSON_AddStringToObject(out, "toc", "unreadable");
    free(buf);
    return;
  }
  snprintf(text, sizeof(text), "%u tracks (%u to %u), %u session(s), lead-out %u, first track %s", toc.count, toc.first,
           toc.last, toc.sessions, toc.leadout, cd_track_is_audio(&toc.tracks[0]) ? "audio" : "data");
  cJSON_AddStringToObject(out, "toc", text);
  RIP_INFO("benchmark: TOC: %s", text);
  lba = toc.leadout / 3;
  for (m = 0; m < RIP_ARRAY_LEN(modes); m++) {
    uint64_t start = svc_mono_ms();
    struct rip_sense sense;
    uint32_t done = 0;
    int rc = RIP_OK;
    while (done < FRAMES && rc == RIP_OK) {
      rc = cd_read_frames(t, lba + done, BATCH, modes[m].flags, buf, &sense);
      done += BATCH;
    }
    if (rc == RIP_OK) {
      uint64_t ms = svc_mono_ms() - start;
      add(tests, modes[m].what, ms ? (double)FRAMES * CD_FRAME / 1e6 / ((double)ms / 1000.0) : 0);
      if (modes[m].flags & CD_READ_SUB) {
        uint8_t q[12];
        cd_deinterleave_q(buf + CD_FRAME + ((modes[m].flags & CD_READ_C2) ? CD_C2 : 0), q);
        snprintf(text, sizeof(text), "%s, track %u index %u", cd_q_crc_ok(q) ? "Q CRC ok" : "Q CRC bad", cd_unbcd(q[1]),
                 cd_unbcd(q[2]));
        cJSON_AddStringToObject(out, modes[m].flags & CD_READ_C2 ? "subchannel_with_c2" : "subchannel", text);
        RIP_INFO("benchmark: subchannel: %s", text);
      }
    } else {
      sense_text(rc, &sense, text, sizeof(text));
      {
        cJSON *o = cJSON_CreateObject();
        cJSON_AddStringToObject(o, "test", modes[m].what);
        cJSON_AddStringToObject(o, "error", text);
        cJSON_AddItemToArray(tests, o);
      }
      RIP_INFO("benchmark: %s: %s", modes[m].what, text);
    }
    lba += FRAMES + 75;
  }
  free(buf);
}

cJSON *bench_drive(const char *id, char *err, size_t en) {
  static const uint32_t sizes[] = {32, 64, 256, 512};  // sectors: 64 KiB to 1 MiB
  struct drive_claim claim;
  struct rip_reader fdr;
  struct rip_reader mmc;
  uint8_t *buf = malloc(SAMPLE_BYTES);
  cJSON *out;
  cJSON *tests;
  uint64_t lba;
  int errors = 0;
  size_t i;

  if (!buf) {
    snprintf(err, en, "out of memory");
    return NULL;
  }
  if (drives_claim(id, 0, 0, &claim, err, en) != RIP_OK) {
    free(buf);
    return NULL;
  }
  out = cJSON_CreateObject();
  tests = cJSON_AddArrayToObject(out, "tests");
  pthread_mutex_lock(claim.io);
  if (mmc_profile_is_cd(claim.disc.profile)) {
    cJSON_AddStringToObject(out, "profile", mmc_profile_name(claim.disc.profile));
    bench_cd(claim.t, out, tests);
    probe_speed(claim.t, claim.disc.has_toc ? claim.disc.toc.leadout - 1 : 0, out);
    pthread_mutex_unlock(claim.io);
    drives_release(claim.id, 0, "");
    free(buf);
    return out;
  }
  probe_css(claim.t, out);
  if (plat_drive_reader(claim.id, claim.t, &fdr) == RIP_OK) {
    char what[64];
    cJSON_AddNumberToObject(out, "sectors", (double)fdr.sector_count);
    cJSON_AddNumberToObject(out, "reader_max_sectors", fdr.max_sectors);
    // The middle of the disc, each test on fresh sectors so the drive's cache doesn't help.
    probe_speed(claim.t, (uint32_t)(fdr.sector_count - 1), out);
    lba = fdr.sector_count / 3;
    probe_sector(claim.t, lba, out);
    // Inner, middle and outer edge: a drive spinning at a constant rate reads
    // faster further out.
    {
      static const unsigned at[] = {2, 50, 90};
      for (i = 0; i < RIP_ARRAY_LEN(at); i++) {
        uint64_t where = fdr.sector_count * at[i] / 100;
        if (where + SAMPLE_BYTES / 2048 > fdr.sector_count) {
          where = fdr.sector_count - SAMPLE_BYTES / 2048;
        }
        snprintf(what, sizeof(what), "default reader at %u%% of the disc", at[i]);
        add(tests, what, time_reads(&fdr, where, 64, buf, &errors));
      }
    }
    lba = fdr.sector_count / 4;
    for (i = 0; i < 2; i++) {
      snprintf(what, sizeof(what), "default reader, %u KiB requests", sizes[i + 1] * 2);
      add(tests, what, time_reads(&fdr, lba, sizes[i + 1], buf, &errors));
      lba += SAMPLE_BYTES / 2048 + 2048;
    }
    if (rip_reader_open_mmc(&mmc, claim.t) == RIP_OK) {
      for (i = 0; i < 2; i++) {
        snprintf(what, sizeof(what), "READ(12) pass-through, %u KiB requests", sizes[i] * 2);
        add(tests, what, time_reads(&mmc, lba, sizes[i], buf, &errors));
        lba += SAMPLE_BYTES / 2048 + 2048;
      }
      rip_reader_close(&mmc);
    }
    rip_reader_close(&fdr);
  }
  pthread_mutex_unlock(claim.io);
  drives_release(claim.id, 0, "");
  // Compression of whatever the disc held, as a rip would see it.
  add(tests, "zlib level 6 (CHD Deflate)", time_zlib(buf, SAMPLE_BYTES, 6));
  add(tests, "zlib level 1", time_zlib(buf, SAMPLE_BYTES, 1));
  add(tests, "zstd level 7 (CHD Zstandard)", time_zstd(buf, SAMPLE_BYTES, 7));
  add(tests, "zstd level 3", time_zstd(buf, SAMPLE_BYTES, 3));
  free(buf);
  return out;
}

/* Probe: each basic command once, raw. For discs that don't come up. */

static void probe_cmd(struct rip_transport *t, cJSON *out, const char *name, const uint8_t *cdb, size_t cdb_len,
                      size_t len) {
  uint8_t buf[256];
  struct rip_sense sense;
  cJSON *o = cJSON_CreateObject();
  char hex[64 * 3 + 1];
  size_t i;
  int rc;

  memset(buf, 0, sizeof(buf));
  memset(&sense, 0, sizeof(sense));
  rc = rip_scsi(t, cdb, cdb_len, len ? RIP_DIR_IN : RIP_DIR_NONE, len ? buf : NULL, len, 10000, &sense);
  cJSON_AddStringToObject(o, "result", rc == RIP_OK ? "ok" : rc == RIP_ESENSE ? "sense" : "error");
  if (rc == RIP_ESENSE) {
    char s[32];
    snprintf(s, sizeof(s), "%02x/%02x/%02x", sense.key, sense.asc, sense.ascq);
    cJSON_AddStringToObject(o, "sense", s);
  }
  if (t->last_error[0]) {
    cJSON_AddStringToObject(o, "platform", t->last_error);
  }
  if (rc == RIP_OK && len) {
    for (i = 0; i < len && i < 64; i++) {
      snprintf(hex + i * 3, 4, "%02x ", buf[i]);
    }
    hex[(i ? i * 3 - 1 : 0)] = '\0';
    cJSON_AddStringToObject(o, "data", hex);
  }
  cJSON_AddItemToObject(out, name, o);
  RIP_INFO("probe: %s: %s %s", name, rc == RIP_OK ? "ok" : rc == RIP_ESENSE ? "sense" : "error", t->last_error);
}

cJSON *bench_probe(const char *id, char *err, size_t en) {
  static const uint8_t tur[6] = {0x00, 0, 0, 0, 0, 0};
  static const uint8_t profile[10] = {0x46, 0x02, 0, 0, 0, 0, 0, 0, 8, 0};
  static const uint8_t features[10] = {0x46, 0x01, 0, 0, 0, 0, 0, 0, 255, 0};
  static const uint8_t capacity[10] = {0x25, 0, 0, 0, 0, 0, 0, 0, 0, 0};
  static const uint8_t disc_info[10] = {0x51, 0, 0, 0, 0, 0, 0, 0, 34, 0};
  static const uint8_t bd_struct[12] = {0xAD, 0x01, 0, 0, 0, 0, 0, 0x00, 0, 64, 0, 0};
  static const uint8_t event[10] = {0x4A, 0x01, 0, 0, 0x10, 0, 0, 0, 8, 0};  // media class, polled
  static const uint8_t mech[12] = {0xBD, 0, 0, 0, 0, 0, 0, 0, 0, 8, 0, 0};
  struct drive_claim claim;
  struct rip_reader r;
  cJSON *out;

  if (drives_claim(id, 0, 0, &claim, err, en) != RIP_OK) {
    return NULL;
  }
  out = cJSON_CreateObject();
  pthread_mutex_lock(claim.io);
  probe_cmd(claim.t, out, "test_unit_ready", tur, sizeof(tur), 0);
  probe_cmd(claim.t, out, "current_profile", profile, sizeof(profile), 8);
  probe_cmd(claim.t, out, "current_features", features, sizeof(features), 255);
  probe_cmd(claim.t, out, "read_capacity", capacity, sizeof(capacity), 8);
  probe_cmd(claim.t, out, "disc_information", disc_info, sizeof(disc_info), 34);
  probe_cmd(claim.t, out, "bd_disc_structure", bd_struct, sizeof(bd_struct), 64);
  probe_cmd(claim.t, out, "media_event", event, sizeof(event), 8);
  probe_cmd(claim.t, out, "mechanism_status", mech, sizeof(mech), 8);
  {
    // What the system shows of the disc where it mounts it, two levels deep.
    const char *mount = plat_drive_mount(claim.id);
    cJSON *list = cJSON_AddArrayToObject(out, "mount");
    DIR *d = mount ? opendir(mount) : NULL;
    struct dirent *e;
    while (d && (e = readdir(d)) != NULL && cJSON_GetArraySize(list) < 200) {
      char path[SVC_PATH];
      struct stat st;
      char line[SVC_PATH + 32];
      if (e->d_name[0] == '.') {
        continue;
      }
      svc_path_join(path, sizeof(path), mount, e->d_name);
      if (stat(path, &st) != 0) {
        continue;
      }
      snprintf(line, sizeof(line), "%s%s %lld", e->d_name, S_ISDIR(st.st_mode) ? "/" : "", (long long)st.st_size);
      cJSON_AddItemToArray(list, cJSON_CreateString(line));
      if (S_ISDIR(st.st_mode)) {
        DIR *sub = opendir(path);
        struct dirent *se;
        while (sub && (se = readdir(sub)) != NULL && cJSON_GetArraySize(list) < 200) {
          char sp[SVC_PATH];
          struct stat sst;
          if (se->d_name[0] == '.') {
            continue;
          }
          svc_path_join(sp, sizeof(sp), path, se->d_name);
          if (stat(sp, &sst) == 0) {
            snprintf(line, sizeof(line), "%s/%s%s %lld", e->d_name, se->d_name, S_ISDIR(sst.st_mode) ? "/" : "",
                     (long long)sst.st_size);
            cJSON_AddItemToArray(list, cJSON_CreateString(line));
          }
        }
        if (sub) {
          closedir(sub);
        }
      }
    }
    if (d) {
      closedir(d);
    }
  }
  if (plat_drive_reader(claim.id, claim.t, &r) == RIP_OK) {
    cJSON_AddNumberToObject(out, "reader_sectors", (double)r.sector_count);
    rip_reader_close(&r);
  } else {
    cJSON_AddStringToObject(out, "reader_sectors", "no reader");
  }
  pthread_mutex_unlock(claim.io);
  drives_release(claim.id, 0, "");
  return out;
}
