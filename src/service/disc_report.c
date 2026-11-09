#include "disc_report.h"

#include <ctype.h>
#include <dirent.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "platform.h"
#include "ripper/cdrom.h"
#include "ripper/common.h"
#include "ripper/log.h"
#include "ripper/mmc.h"
#include "ripper/omnidrive.h"
#include "ripper/util.h"
#include "ripper/xbox.h"
#include "ripper/zip.h"
#include "svc.h"

#define FIRST_SECTORS 32
#define READ_CHUNK 16
#define READ_BUDGET_MS 10000  // a disc that reads slowly or not at all doesn't hold the drive long
#define LIST_MAX 150

static void reports_dir(char *out, size_t n) {
  char diag[SVC_PATH];

  svc_path_join(diag, sizeof(diag), plat_data_dir(), "diag");
  svc_path_join(out, n, diag, "discs");
}

// Letters, digits and dashes, for the folder's name.
static void slug(const char *in, char *out, size_t n) {
  size_t k = 0;
  int dash = 0;

  for (; *in && k + 1 < n; in++) {
    if (isalnum((unsigned char)*in)) {
      out[k++] = *in;
      dash = 0;
    } else if (!dash && k) {
      out[k++] = '-';
      dash = 1;
    }
  }
  while (k && out[k - 1] == '-') {
    k--;
  }
  out[k] = '\0';
}

// Lines the same as the one before (zeros, mostly) fold into one "*".
static void hex_dump(FILE *f, const uint8_t *b, size_t n) {
  size_t i, j;
  int folded = 0;

  for (i = 0; i < n; i += 16) {
    if (i && i + 16 <= n && !memcmp(b + i, b + i - 16, 16)) {
      if (!folded) {
        fprintf(f, "  *\n");
      }
      folded = 1;
      continue;
    }
    folded = 0;
    fprintf(f, "  %04zx ", i);
    for (j = 0; j < 16; j++) {
      fprintf(f, i + j < n ? " %02x" : "   ", b[i + j]);
    }
    fprintf(f, "  ");
    for (j = 0; j < 16 && i + j < n; j++) {
      fputc(isprint(b[i + j]) ? b[i + j] : '.', f);
    }
    fputc('\n', f);
  }
}

static void sense_str(int rc, const struct rip_sense *s, const struct rip_transport *t, char *out, size_t n) {
  if (rc == RIP_OK) {
    snprintf(out, n, "ok");
  } else if (rc == RIP_ESENSE) {
    snprintf(out, n, "sense %02x/%02x/%02x", s->key, s->asc, s->ascq);
  } else {
    snprintf(out, n, "failed%s%s", t->last_error[0] ? ": " : "", t->last_error);
  }
}

static void write_raw(const char *dir, const char *name, const void *data, size_t len) {
  char path[SVC_PATH + 64];

  snprintf(path, sizeof(path), "%s/raw/%s", dir, name);
  rip_write_file(path, data, len);
}

static void about_disc(FILE *f, const struct disc_report *r) {
  const struct rip_disc *d = r->disc;
  unsigned i;

  fprintf(f, "\n== Disc\n");
  if (!d) {
    fprintf(f, "not identified: the drive gave profile %s (0x%04x), sense %02x/%02x/%02x\n",
            mmc_profile_name(r->drive->last_profile), r->drive->last_profile, r->drive->last_sense.key,
            r->drive->last_sense.asc, r->drive->last_sense.ascq);
    return;
  }
  fprintf(f, "identified as: %s\n", rip_disc_kind_name(d->kind));
  fprintf(f, "profile: %s (0x%04x)\n", mmc_profile_name(d->profile), d->profile);
  fprintf(f, "size: %llu x %u bytes\n", (unsigned long long)d->sector_count, d->sector_size);
  fprintf(f, "title: '%s'  title id: '%s'  volume: '%s'\n", d->title, d->title_id, d->volume_id);
  fprintf(f, "ISO 9660: %s  UDF: %s  CSS: %s\n", d->has_iso9660 ? "yes" : "no", d->has_udf ? "yes" : "no",
          d->css ? "yes" : "no");
  if (d->has_toc) {
    fprintf(f, "TOC: %u track(s), %u session(s), disc type 0x%02x, lead-out %u\n", d->toc.count, d->toc.sessions,
            (unsigned)d->toc.type, d->toc.leadout);
    for (i = 0; i < d->toc.count && i < CD_MAX_TRACKS; i++) {
      const struct cd_track *tr = &d->toc.tracks[i];
      fprintf(f, "  track %2u  session %u  %s  mode %d  start %u  length %u  control 0x%x\n", tr->number, tr->session,
              cd_track_is_audio(tr) ? "audio" : "data ", (int)tr->mode, tr->start, tr->length, tr->control);
    }
  }
  if (d->musicbrainz_id[0]) {
    fprintf(f, "MusicBrainz disc id: %s\n", d->musicbrainz_id);
  }
  if (d->kind == RIP_DISC_DREAMCAST) {
    fprintf(f, "GD-ROM: %s  high-density area (LBA %u) readable here: %s\n", d->gd_rom ? "yes" : "no (MIL-CD)",
            GD_HD_LBA, d->gd_hd_readable ? "yes" : "no");
  }
}

// OmniDrive's raw read of LBA 0, as it came and descrambled each way.
static void omnidrive_lba0(FILE *f, const struct disc_report *r, const char *dir) {
  int bd = mmc_profile_is_bd(r->profile);
  size_t size = bd ? BD_RAW_FRAME : DVD_RAW_FRAME;
  uint8_t *raw = malloc(size);
  uint8_t *frame = malloc(size);
  struct rip_sense sense = {0, 0, 0};
  char why[96];
  int rc;

  if (!raw || !frame) {
    free(raw);
    free(frame);
    return;
  }
  rc = omni_read(r->t, bd ? OMNI_BD : OMNI_DVD, 0, 0, 1, raw, size, &sense);
  sense_str(rc, &sense, r->t, why, sizeof(why));
  fprintf(f, "C0 raw read, LBA 0: %s\n", why);
  if (rc == RIP_OK) {
    write_raw(dir, "omnidrive-lba0.bin", raw, size);
    if (bd) {
      memcpy(frame, raw, size);
      bd_descramble(frame, 0, 0);
      fprintf(f, "  Blu-ray scrambling: EDC %s\n", dvd_edc(frame, 2048) == rip_be32(frame + 2048) ? "ok" : "bad");
      memcpy(frame, raw, size);
      bd_descramble(frame, 0, 1);
      fprintf(f, "  Nintendo scrambling: EDC %s\n", dvd_edc(frame, 2048) == rip_be32(frame + 2048) ? "ok" : "bad");
      hex_dump(f, frame, 64);
    } else {
      memcpy(frame, raw, size);
      fprintf(f, "  DVD scrambling: EDC %s\n", dvd_frame_fix(frame, -1) ? "ok" : "bad");
      memcpy(frame, raw, size);
      fprintf(f, "  Nintendo scrambling (key 0): EDC %s\n", dvd_frame_fix(frame, 0) ? "ok" : "bad");
      hex_dump(f, frame + 6, 64);
    }
  }
  free(raw);
  free(frame);
}

// What custom firmware reads that a stock drive doesn't.
static void custom_firmware(FILE *f, const struct disc_report *r, const char *dir) {
  const struct rip_disc *d = r->disc;

  if (!d || !(r->drive->caps & (RIP_CAP_OMNIDRIVE | RIP_CAP_XBOX))) {
    return;
  }
  fprintf(f, "\n== Custom firmware\n");
  if ((r->drive->caps & RIP_CAP_OMNIDRIVE) && (mmc_profile_is_dvd(r->profile) || mmc_profile_is_bd(r->profile))) {
    omnidrive_lba0(f, r, dir);
  }
  if (d->kind == RIP_DISC_XBOX || d->kind == RIP_DISC_XBOX_360) {
    struct xbox_layout *x = malloc(sizeof(*x));
    int rc = x ? xbox_read_layout(r->drive, x) : RIP_ENOMEM;
    unsigned i;
    if (rc == RIP_OK) {
      fprintf(f, "XGD%u: %u sectors, %u protected range(s)\n", x->version, x->sectors, x->range_count);
      for (i = 0; i < x->range_count; i++) {
        fprintf(f, "  %u .. %u\n", x->ranges[i].start, x->ranges[i].end);
      }
      write_raw(dir, "security-sector.bin", x->ss, sizeof(x->ss));
      xbox_relock(r->drive, x);
    } else {
      fprintf(f, "Xbox security sector: not read (%d)\n", rc);
    }
    free(x);
  }
}

static void about_offer(FILE *f, const struct disc_report *r) {
  const cJSON *fmt;
  const cJSON *sug = r->offer ? cJSON_GetObjectItemCaseSensitive(r->offer, "suggestion") : NULL;

  fprintf(f, "\n== What ripper offered\n");
  fprintf(f, "%s\n", r->outcome && r->outcome[0] ? r->outcome : "(nothing said)");
  if (!r->offer) {
    return;
  }
  fprintf(f, "system: %s\n", cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(r->offer, "system_name")));
  cJSON_ArrayForEach(fmt, cJSON_GetObjectItemCaseSensitive(r->offer, "formats")) {
    const char *reason = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(fmt, "reason"));
    fprintf(f, "  %-6s %s%s%s\n", cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(fmt, "label")),
            cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(fmt, "availability")),
            reason && reason[0] ? ": " : "", reason ? reason : "");
  }
  if (sug) {
    fprintf(f, "suggested: %s as %s\n", cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(sug, "name")),
            cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(sug, "format")));
  }
}

// The drive's whole feature list and, on a DVD or BD, the physical format.
static void drive_answers(FILE *f, const struct disc_report *r, const char *dir) {
  uint8_t *buf = malloc(8192);
  uint8_t cdb[10] = {0x46, 0, 0, 0, 0, 0, 0, 0x20, 0x00, 0};  // GET CONFIGURATION, all, 8 KiB
  struct rip_sense sense;
  char why[96];
  int rc;

  if (!buf) {
    return;
  }
  fprintf(f, "\n== Drive answers\n");
  memset(buf, 0, 8192);
  rc = rip_scsi(r->t, cdb, sizeof(cdb), RIP_DIR_IN, buf, 8192, MMC_TIMEOUT_MS, &sense);
  sense_str(rc, &sense, r->t, why, sizeof(why));
  if (rc == RIP_OK) {
    size_t len = rip_be32(buf) + 4;
    len = len > 8192 ? 8192 : len;
    write_raw(dir, "get-configuration.bin", buf, len);
    fprintf(f, "GET CONFIGURATION: %zu bytes, current profile 0x%04x (raw/get-configuration.bin)\n", len,
            (unsigned)((buf[6] << 8) | buf[7]));
  } else {
    fprintf(f, "GET CONFIGURATION: %s\n", why);
  }
  if (r->disc ? !mmc_profile_is_cd(r->disc->profile) : !mmc_profile_is_cd(r->drive->last_profile)) {
    int bd = r->disc ? mmc_profile_is_bd(r->disc->profile) : mmc_profile_is_bd(r->drive->last_profile);
    memset(buf, 0, 2052);
    rc = mmc_read_disc_structure(r->t, bd ? 1 : 0, 0, 0, 0, buf, 2052, &sense);
    sense_str(rc, &sense, r->t, why, sizeof(why));
    fprintf(f, "READ DISC STRUCTURE %s format 0: %s\n", bd ? "BD" : "DVD", why);
    if (rc == RIP_OK) {
      write_raw(dir, "structure-00.bin", buf, 2052);
      hex_dump(f, buf, 32);
      // A Nintendo disc names itself in its physical format (redumper's dvd_dump).
      if (!bd && buf[4] == 0xFF) {
        fprintf(f, "looks like: Nintendo (GameCube/Wii), by its physical format\n");
      }
    }
  }
  free(buf);
}

static void name_sector(FILE *f, uint64_t lba, const uint8_t *b) {
  if (lba == 0) {
    if (rip_be32(b + 0x18) == 0x5D1C9EA3) {
      fprintf(f, "looks like: Wii disc header, %.6s '%.64s'\n", b, b + 0x20);
    } else if (rip_be32(b + 0x1C) == 0xC2339F3D) {
      fprintf(f, "looks like: GameCube disc header, %.6s '%.64s'\n", b, b + 0x20);
    } else if (!memcmp(b, "SEGA SEGASATURN ", 16)) {
      fprintf(f, "looks like: Sega Saturn, '%.16s' '%.10s'\n", b + 0x20, b + 0x60);
    } else if (!memcmp(b, "SEGADISCSYSTEM  ", 16) || !memcmp(b, "SEGABOOTDISC    ", 16)) {
      fprintf(f, "looks like: Sega CD\n");
    } else if (!memcmp(b, "SEGA SEGAKATANA ", 16)) {
      fprintf(f, "looks like: Dreamcast\n");
    } else if (b[0] == 0x01 && !memcmp(b + 1, "\x5a\x5a\x5a\x5a\x5a\x01", 6)) {
      fprintf(f, "looks like: 3DO (Opera file system)\n");
    }
  } else if (lba == 16) {
    if (!memcmp(b + 1, "CD001", 5)) {
      fprintf(f, "ISO 9660 volume descriptor: system '%.32s' volume '%.32s'\n", b + 8, b + 40);
    } else if (!memcmp(b + 1, "CD-I ", 5)) {
      fprintf(f, "looks like: CD-i (volume '%.32s')\n", b + 40);
    } else if (!memcmp(b + 1, "BEA01", 5)) {
      fprintf(f, "UDF volume recognition sequence\n");
    }
  } else if (lba == 32 && !memcmp(b, "MICROSOFT*XBOX*MEDIA", 20)) {
    fprintf(f, "looks like: Xbox XDVDFS (game partition readable)\n");
  }
}

// The first sectors as the platform reads them, and a few known places.
static void first_sectors(FILE *f, const struct disc_report *r, const char *dir) {
  static const uint64_t PROBES[] = {32, 256};
  struct rip_reader *rd = r->reader;
  uint32_t ss = rd ? rd->sector_size : 0;
  uint8_t *buf;
  uint64_t start = rip_now_ms();
  unsigned got = 0;
  unsigned i;

  fprintf(f, "\n== First sectors\n");
  if (!rd || ss == 0 || ss > 2352) {
    fprintf(f, "not readable by sector here\n");
    return;
  }
  buf = calloc(FIRST_SECTORS, ss);
  if (!buf) {
    return;
  }
  for (i = 0; i < FIRST_SECTORS && rip_now_ms() - start < READ_BUDGET_MS; i += READ_CHUNK) {
    unsigned k;
    if (rip_reader_read(rd, i, READ_CHUNK, buf + (size_t)i * ss) == RIP_OK) {
      got += READ_CHUNK;
      continue;
    }
    for (k = i; k < i + READ_CHUNK && rip_now_ms() - start < READ_BUDGET_MS; k++) {
      if (rip_reader_read(rd, k, 1, buf + (size_t)k * ss) == RIP_OK) {
        got++;
      } else if (k == 0) {
        break;  // nothing reads: don't wait on each
      }
    }
  }
  fprintf(f, "sectors 0-%u: %u of %u read, %u bytes each (raw/sectors-0-%u.bin)\n", FIRST_SECTORS - 1, got,
          FIRST_SECTORS, ss, FIRST_SECTORS - 1);
  if (got) {
    char name[64];
    snprintf(name, sizeof(name), "sectors-0-%u.bin", FIRST_SECTORS - 1);
    write_raw(dir, name, buf, (size_t)FIRST_SECTORS * ss);
    fprintf(f, "sector 0:\n");
    hex_dump(f, buf, 256);
    name_sector(f, 0, buf);
    fprintf(f, "sector 16:\n");
    hex_dump(f, buf + 16 * (size_t)ss, 128);
    name_sector(f, 16, buf + 16 * (size_t)ss);
  }
  for (i = 0; got && i < sizeof(PROBES) / sizeof(PROBES[0]) && rip_now_ms() - start < READ_BUDGET_MS; i++) {
    if (PROBES[i] >= rd->sector_count && rd->sector_count) {
      continue;
    }
    if (rip_reader_read(rd, PROBES[i], 1, buf) == RIP_OK) {
      char name[64];
      snprintf(name, sizeof(name), "sector-%llu.bin", (unsigned long long)PROBES[i]);
      write_raw(dir, name, buf, ss);
      fprintf(f, "sector %llu: read (raw/%s)\n", (unsigned long long)PROBES[i], name);
      name_sector(f, PROBES[i], buf);
    } else {
      fprintf(f, "sector %llu: not readable\n", (unsigned long long)PROBES[i]);
    }
  }
  if (rip_now_ms() - start >= READ_BUDGET_MS) {
    fprintf(f, "stopped reading after %d s\n", READ_BUDGET_MS / 1000);
  }
  free(buf);
}

static void list_dir(FILE *f, const char *path, const char *shown, int depth, int *lines) {
  DIR *d = opendir(path);
  struct dirent *e;

  if (!d) {
    return;
  }
  while ((e = readdir(d)) != NULL && *lines < LIST_MAX) {
    char full[SVC_PATH + 256];
    struct stat st;
    if (e->d_name[0] == '.') {
      continue;
    }
    snprintf(full, sizeof(full), "%s/%s", path, e->d_name);
    if (stat(full, &st) != 0) {
      continue;
    }
    (*lines)++;
    if (S_ISDIR(st.st_mode)) {
      char sub[SVC_PATH + 256];
      fprintf(f, "  %s%s/\n", shown, e->d_name);
      snprintf(sub, sizeof(sub), "%s%s/", shown, e->d_name);
      if (depth < 1) {
        list_dir(f, full, sub, depth + 1, lines);
      }
    } else {
      fprintf(f, "  %s%s  %lld\n", shown, e->d_name, (long long)st.st_size);
    }
  }
  closedir(d);
}

// What to call the disc in the folder's name.
static const char *disc_label(const struct rip_disc *d) {
  if (!d) {
    return "unreadable";
  }
  if (d->title[0]) {
    return d->title;
  }
  if (d->title_id[0]) {
    return d->title_id;
  }
  return d->volume_id[0] ? d->volume_id : rip_disc_kind_name(d->kind);
}

void disc_report_write(const struct disc_report *r) {
  char dir[SVC_PATH];
  char base[SVC_PATH];
  char name[160];
  char what[41];  // a short name: the folder's is the time, the drive and this
  char stamp[32];
  char path[SVC_PATH + 64];
  char caps[128];
  const char *label;
  time_t now = time(NULL);
  struct tm tm;
  FILE *f;

  localtime_r(&now, &tm);
  strftime(stamp, sizeof(stamp), "%Y%m%d-%H%M%S", &tm);
  label = disc_label(r->disc);
  slug(label, what, sizeof(what));
  snprintf(name, sizeof(name), "%s-%s-%s", stamp, r->drive_id, what[0] ? what : "disc");
  reports_dir(base, sizeof(base));
  svc_path_join(dir, sizeof(dir), base, name);
  snprintf(path, sizeof(path), "%s/raw", dir);
  if (rip_mkdir_p(path) != RIP_OK) {
    RIP_WARN("disc report: can't make %s", path);
    return;
  }
  snprintf(path, sizeof(path), "%s/report.txt", dir);
  f = fopen(path, "w");
  if (!f) {
    RIP_WARN("disc report: can't write %s", path);
    return;
  }
  rip_caps_str(r->drive->caps, caps, sizeof(caps));
  fprintf(f, "ripper %s (build %s), %s\n", RIPPER_VERSION, RIPPER_BUILD, stamp);
  fprintf(f, "drive: %s (%s): %s %s %s, %s, reads %s\n", r->drive_id, r->drive_label, r->drive->inquiry.vendor,
          r->drive->inquiry.product, r->drive->inquiry.revision, rip_drive_source_name(r->drive->source), caps);
  about_disc(f, r);
  about_offer(f, r);
  drive_answers(f, r, dir);
  first_sectors(f, r, dir);
  custom_firmware(f, r, dir);
  if (r->mount_root && svc_dir_exists(r->mount_root)) {
    int lines = 0;
    fprintf(f, "\n== Files, as the system mounted the disc (%s)\n", r->mount_root);
    list_dir(f, r->mount_root, "", 0, &lines);
  }
  fclose(f);
  RIP_INFO("disc report: %s", name);
}

char *reports_bundle(size_t *len) {
  static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
  char diag[SVC_PATH];
  char from[SVC_PATH];
  char to[SVC_PATH + 32];
  char zip[SVC_PATH];
  char about[256];
  char *data = NULL;
  int n;

  pthread_mutex_lock(&lock);
  svc_path_join(diag, sizeof(diag), plat_data_dir(), "diag");
  rip_mkdir_p(diag);
  // The whole log (the ring has only the last lines) and a crash, if there was one.
  svc_path_join(from, sizeof(from), plat_data_dir(), "ripper.log");
  snprintf(to, sizeof(to), "%s/ripper-log.txt", diag);
  svc_copy_file(from, to, NULL, NULL);
  svc_path_join(from, sizeof(from), plat_data_dir(), "crash.log");
  snprintf(to, sizeof(to), "%s/crash-log.txt", diag);
  if (access(from, F_OK) == 0) {
    svc_copy_file(from, to, NULL, NULL);
  }
  snprintf(to, sizeof(to), "%s/about.txt", diag);
  n = snprintf(about, sizeof(about), "ripper %s (build %s) on %s\n", RIPPER_VERSION, RIPPER_BUILD, plat_name());
  rip_write_file(to, about, (size_t)n);
  svc_path_join(zip, sizeof(zip), plat_data_dir(), "ripper-reports.zip");
  if (zip_build_dir(diag, zip, NULL, NULL) == RIP_OK) {
    data = svc_read_file(zip, len);
  }
  unlink(zip);
  pthread_mutex_unlock(&lock);
  return data;
}
