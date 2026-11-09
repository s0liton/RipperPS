// Host tool for development: runs the same core against a PC drive (SG_IO) or
// an image file, so changes can be tried without deploying to a console.

#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "ripper/common.h"
#include "ripper/diag.h"
#include "ripper/disc.h"
#include "ripper/drive.h"
#include "ripper/exfat.h"
#include "ripper/keydb.h"
#include "ripper/log.h"
#include "ripper/mmc.h"
#include "ripper/ps3.h"
#include "ripper/reader.h"
#include "ripper/rip.h"
#include "ripper/ufs2.h"
#include "ripper/util.h"
#include "ripper/zip.h"
#include "sgio_transport.h"

static void usage(void) {
  fprintf(stderr,
          "usage:\n"
          "  ripper-cli probe --device /dev/sr0\n"
          "  ripper-cli rip   --device /dev/sr0 [--blockdev] [options]\n"
          "  ripper-cli rip   --image FILE --profile NAME [options]\n"
          "  ripper-cli ps3-decrypt --in FILE (--key HEX | --d1 HEX)\n"
          "  ripper-cli pack  --in DIR --out IMAGE.ffpkg|IMAGE.exfat [--label NAME]\n"
          "  ripper-cli diag  --device /dev/sr0 [--out DIR]   test the drive and its disc, into a zip\n"
          "\n"
          "options:\n"
          "  --out DIR           output directory (default ./out)\n"
          "  --keys FILE         PS3 key file (see tools/build_ps3_keydb.py)\n"
          "  --keep-encrypted    keep the encrypted PS3 image next to the decrypted one\n"
          "  --verbose\n"
          "\n"
          "profiles for --image: dvd, bd, ps2-dvd, ps3-dvd, ps3-bd\n");
}

static void on_progress(const char *phase, uint64_t done, uint64_t total, void *user) {
  (void)user;
  if (total) {
    fprintf(stderr, "\r%s %3d%%", phase, (int)(done * 100 / total));
    if (done == total) {
      fputc('\n', stderr);
    }
  } else {
    fprintf(stderr, "\r%s %llu MiB", phase, (unsigned long long)(done >> 20));
  }
}

static int pack_progress(void *ud, uint64_t done, uint64_t total) {
  (void)ud;
  on_progress("packing", done, total, NULL);
  return 0;
}

// A game folder into a ShadowMountPlus image, by the image's extension.
static int cmd_pack(const char *dir, const char *image, const char *label) {
  size_t len = strlen(image);
  int rc;

  if (len > 6 && !strcmp(image + len - 6, ".exfat")) {
    rc = exfat_build(dir, image, label, pack_progress, NULL);
  } else if (len > 6 && !strcmp(image + len - 6, ".ffpkg")) {
    rc = ufs2_build(dir, image, label, pack_progress, NULL);
  } else {
    fprintf(stderr, "the image name must end in .ffpkg or .exfat\n");
    return 2;
  }
  return rc == RIP_OK ? 0 : 1;
}

static void diag_progress(void *ud, const char *step, int done, int total) {
  (void)ud;
  fprintf(stderr, "[%d/%d] %s\n", done, total, step);
}

// The drive test, as the console runs it: a folder, zipped beside it.
static int cmd_diag(struct rip_transport *t, const char *out_dir) {
  struct diag_summary sum;
  char dir[1024];
  char zip[1100];
  char stamp[32];
  time_t now = time(NULL);
  struct tm tm;
  int rc;

  localtime_r(&now, &tm);
  strftime(stamp, sizeof(stamp), "%Y%m%d-%H%M%S", &tm);
  snprintf(dir, sizeof(dir), "%s/ripper-diag-pc-%s", out_dir, stamp);
  rc = diag_run(t, dir, diag_progress, NULL, NULL, &sum);
  if (rc != RIP_OK) {
    fprintf(stderr, "the test couldn't write to %s\n", dir);
    return 1;
  }
  snprintf(zip, sizeof(zip), "%s.zip", dir);
  printf("%s, %s: %d of %d commands taken\n%s", sum.drive, sum.disc, sum.answered, sum.commands, sum.findings);
  if (zip_build_dir(dir, zip, NULL, NULL) == RIP_OK) {
    printf("\nsend %s\n", zip);
  } else {
    printf("\nthe report is in %s\n", dir);
  }
  return 0;
}

static int parse_profile(const char *name, uint16_t *profile) {
  static const struct {
    const char *name;
    uint16_t profile;
  } map[] = {
      {"dvd", MMC_PROFILE_DVD_ROM},     {"bd", MMC_PROFILE_BD_ROM},     {"ps2-dvd", MMC_PROFILE_PS2_DVD},
      {"ps3-dvd", MMC_PROFILE_PS3_DVD}, {"ps3-bd", MMC_PROFILE_PS3_BD},
  };
  size_t i;

  for (i = 0; i < RIP_ARRAY_LEN(map); i++) {
    if (strcmp(map[i].name, name) == 0) {
      *profile = map[i].profile;
      return RIP_OK;
    }
  }
  return RIP_ERR;
}

static int open_device(const char *device, struct rip_transport *t, struct rip_drive *drive) {
  char caps[128];

  if (sgio_transport_open(t, device) != RIP_OK) {
    fprintf(stderr, "cannot open %s\n", device);
    return RIP_ERR;
  }
  if (rip_drive_probe(drive, t, RIP_DRIVE_EXTERNAL) != RIP_OK) {
    fprintf(stderr, "%s did not answer INQUIRY\n", device);
    rip_transport_close(t);
    return RIP_ERR;
  }
  rip_caps_str(drive->caps, caps, sizeof(caps));
  printf("drive: %s %s %s\ncaps:  %s\n", drive->inquiry.vendor, drive->inquiry.product, drive->inquiry.revision, caps);
  return RIP_OK;
}

static void print_plan(const struct rip_drive *drive, const struct rip_disc *disc) {
  struct rip_choice choices[8];
  size_t n = rip_plan(drive, disc, choices, RIP_ARRAY_LEN(choices));
  size_t i;

  printf("disc:  %s, title '%s', %llu sectors\n", rip_disc_kind_name(disc->kind), disc->title_id,
         (unsigned long long)disc->sector_count);
  for (i = 0; i < n; i++) {
    printf("  - %-32s %s%s%s\n", choices[i].recipe->output, rip_availability_name(choices[i].avail),
           choices[i].reason[0] ? ": " : "", choices[i].reason);
  }
}

static int cmd_ps3_decrypt(const char *in, const char *key_hex, const char *d1_hex) {
  uint8_t key[16];

  if (key_hex && strlen(key_hex) == 32 && rip_unhex(key_hex, key, 16) == 0) {
    // key as given
  } else if (d1_hex && strlen(d1_hex) == 32 && rip_unhex(d1_hex, key, 16) == 0) {
    uint8_t d1[16];
    memcpy(d1, key, 16);
    ps3_key_from_d1(d1, key);
  } else {
    fprintf(stderr, "need --key or --d1 as 32 hex characters\n");
    return 2;
  }
  return ps3_decrypt_file(in, key, NULL, NULL) == RIP_OK ? 0 : 1;
}

int main(int argc, char **argv) {
  const char *cmd;
  const char *device = NULL;
  const char *image = NULL;
  const char *profile_name = NULL;
  const char *out_dir = "out";
  const char *keys_path = NULL;
  const char *in = NULL;
  const char *label = NULL;
  const char *key_hex = NULL;
  const char *d1_hex = NULL;
  int keep_encrypted = 0;
  int blockdev = 0;
  struct rip_transport t;
  struct rip_drive drive;
  struct rip_drive *drive_ptr = NULL;
  struct rip_reader reader;
  struct rip_disc disc;
  struct rip_keydb keys;
  uint16_t profile = 0;
  int i;
  int rc;

  if (argc < 2) {
    usage();
    return 2;
  }
  cmd = argv[1];
  for (i = 2; i < argc; i++) {
    const char *a = argv[i];
    const char *next = i + 1 < argc ? argv[i + 1] : NULL;

    if (strcmp(a, "--device") == 0 && next) {
      device = argv[++i];
    } else if (strcmp(a, "--image") == 0 && next) {
      image = argv[++i];
    } else if (strcmp(a, "--profile") == 0 && next) {
      profile_name = argv[++i];
    } else if (strcmp(a, "--out") == 0 && next) {
      out_dir = argv[++i];
    } else if (strcmp(a, "--keys") == 0 && next) {
      keys_path = argv[++i];
    } else if (strcmp(a, "--in") == 0 && next) {
      in = argv[++i];
    } else if (strcmp(a, "--label") == 0 && next) {
      label = argv[++i];
    } else if (strcmp(a, "--key") == 0 && next) {
      key_hex = argv[++i];
    } else if (strcmp(a, "--d1") == 0 && next) {
      d1_hex = argv[++i];
    } else if (strcmp(a, "--keep-encrypted") == 0) {
      keep_encrypted = 1;
    } else if (strcmp(a, "--blockdev") == 0) {
      blockdev = 1;
    } else if (strcmp(a, "--verbose") == 0) {
      rip_log_set_level(RIP_LOG_DEBUG);
    } else {
      usage();
      return 2;
    }
  }

  if (strcmp(cmd, "ps3-decrypt") == 0) {
    if (!in) {
      usage();
      return 2;
    }
    return cmd_ps3_decrypt(in, key_hex, d1_hex);
  }
  if (strcmp(cmd, "pack") == 0) {
    if (!in || !out_dir) {
      usage();
      return 2;
    }
    return cmd_pack(in, out_dir, label);
  }
  if (strcmp(cmd, "diag") == 0) {
    if (!device) {
      usage();
      return 2;
    }
    if (open_device(device, &t, &drive) != RIP_OK) {
      return 1;
    }
    rc = cmd_diag(&t, out_dir);
    rip_transport_close(&t);
    return rc;
  }
  if (strcmp(cmd, "probe") != 0 && strcmp(cmd, "rip") != 0) {
    usage();
    return 2;
  }

  if (device) {
    enum rip_media_state state;

    if (open_device(device, &t, &drive) != RIP_OK) {
      return 1;
    }
    drive_ptr = &drive;
    state = rip_drive_media(&drive, &profile);
    if (state != RIP_MEDIA_PRESENT) {
      printf("no readable disc (state %d)\n", (int)state);
      rip_transport_close(&t);
      return state == RIP_MEDIA_EMPTY ? 0 : 1;
    }
    if (blockdev) {
      int fd = open(device, O_RDONLY);
      off_t size = fd >= 0 ? lseek(fd, 0, SEEK_END) : -1;
      rc = size > 0 ? rip_reader_open_fd(&reader, fd, 2048, (uint64_t)size, 1) : RIP_ERR;
    } else {
      rc = rip_reader_open_mmc(&reader, &t);
    }
  } else if (image) {
    struct stat st;
    int fd;

    if (!profile_name || parse_profile(profile_name, &profile) != RIP_OK) {
      fprintf(stderr, "--image needs --profile\n");
      return 2;
    }
    fd = open(image, O_RDONLY);
    if (fd < 0 || fstat(fd, &st) != 0) {
      fprintf(stderr, "cannot open %s\n", image);
      return 1;
    }
    rc = rip_reader_open_fd(&reader, fd, 2048, (uint64_t)st.st_size, 1);
  } else {
    usage();
    return 2;
  }
  if (rc != RIP_OK) {
    fprintf(stderr, "cannot read the disc\n");
    return 1;
  }

  rip_disc_identify(&disc, profile, &reader, drive_ptr ? &t : NULL, NULL);
  print_plan(drive_ptr, &disc);

  rc = 0;
  if (strcmp(cmd, "rip") == 0) {
    const struct rip_recipe *recipe = rip_default_recipe(drive_ptr, &disc);
    struct rip_job_opts opts;
    struct rip_job_result result;

    memset(&keys, 0, sizeof(keys));
    if (keys_path && rip_keydb_load(&keys, keys_path) != RIP_OK) {
      fprintf(stderr, "cannot load %s\n", keys_path);
    }
    memset(&opts, 0, sizeof(opts));
    opts.out_dir = out_dir;
    opts.ps3_keys = keys.count ? &keys : NULL;
    opts.ps3_keep_encrypted = keep_encrypted;
    opts.progress = on_progress;
    if (!recipe) {
      fprintf(stderr, "nothing can rip this disc with this drive yet\n");
      rc = 1;
    } else if (rip_run(recipe, drive_ptr, &reader, &disc, &opts, &result) != RIP_OK) {
      rc = 1;
    } else {
      printf("image:    %s\nmanifest: %s\n", result.image_path, result.manifest_path);
    }
    rip_keydb_free(&keys);
  }

  rip_reader_close(&reader);
  if (drive_ptr) {
    rip_transport_close(&t);
  }
  return rc;
}
