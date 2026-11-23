// The service's platform on a PC, for development: real drives through SG_IO
// (/dev/sr0...) and a virtual drive that takes disc images.
//
//   RIPPER_DATA=dir        data folder (default ./host-data)
//   RIPPER_IMAGE=path      an image in the virtual drive at start
//   RIPPER_VIRTUAL=ps5     the virtual drive acts like the PS5's: PlayStation
//   ROMMPS_DATA=dir        RommPS's data folder
//                          profiles, no CD laser, reported as the internal drive

#include "platform.h"

#include <arpa/inet.h>
#include <ifaddrs.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "ripper/common.h"
#include "ripper/image_drive.h"
#include "ripper/mmc.h"
#include "ripper/util.h"
#include "sgio_transport.h"
#include "svc.h"

static char g_data[SVC_PATH];

const char *plat_name(void) {
  return "linux";
}

const char *plat_console(void) {
  return "PC";
}

int plat_init(void) {
  return 0;
}

const char *plat_data_dir(void) {
  if (!g_data[0]) {
    const char *env = getenv("RIPPER_DATA");
    svc_str_copy(g_data, sizeof(g_data), env && env[0] ? env : "./host-data");
    rip_mkdir_p(g_data);
  }
  return g_data;
}

const char *plat_rommps_dir(void) {
  const char *env = getenv("ROMMPS_DATA");
  return env && env[0] ? env : "./host-rommps";
}

void plat_log_line(enum rip_log_level level, const char *line) {
  static const char *names[] = {"debug", "info", "warn", "error"};

  fprintf(stderr, "[ripper][%s] %s\n", names[level], line);
}

void plat_keep_awake(void) {
}

void plat_disc_inserted(void) {
}

int plat_app_in_front(void) {
  return 0;
}

void plat_notify(const char *title, const char *body) {
  fprintf(stderr, "[notify] %s%s%s\n", title, body && body[0] ? " - " : "", body ? body : "");
}

void plat_local_ip(char *buf, size_t n) {
  struct ifaddrs *ifs = NULL;
  struct ifaddrs *i;

  buf[0] = '\0';
  if (getifaddrs(&ifs) != 0) {
    return;
  }
  for (i = ifs; i; i = i->ifa_next) {
    if (i->ifa_addr && i->ifa_addr->sa_family == AF_INET && strncmp(i->ifa_name, "lo", 2) != 0) {
      inet_ntop(AF_INET, &((struct sockaddr_in *)i->ifa_addr)->sin_addr, buf, (socklen_t)n);
      break;
    }
  }
  freeifaddrs(ifs);
}

static int ps5_like(void) {
  const char *v = getenv("RIPPER_VIRTUAL");
  return v && !strcmp(v, "ps5");
}

int plat_drive_list(struct plat_drive *out, int max) {
  int n = 0;
  int i;

  if (n < max) {
    memset(&out[n], 0, sizeof(out[n]));
    snprintf(out[n].id, sizeof(out[n].id), "virtual0");
    snprintf(out[n].label, sizeof(out[n].label), ps5_like() ? "Built-in Blu-ray (virtual)" : "Virtual drive");
    out[n].source = ps5_like() ? RIP_DRIVE_INTERNAL : RIP_DRIVE_EXTERNAL;
    out[n].is_virtual = 1;
    n++;
  }
  for (i = 0; i < 4 && n < max; i++) {
    char dev[32];
    snprintf(dev, sizeof(dev), "/dev/sr%d", i);
    if (access(dev, R_OK) == 0) {
      memset(&out[n], 0, sizeof(out[n]));
      snprintf(out[n].id, sizeof(out[n].id), "sr%d", i);
      snprintf(out[n].label, sizeof(out[n].label), "Drive %s", dev);
      out[n].source = RIP_DRIVE_EXTERNAL;
      n++;
    }
  }
  return n;
}

int plat_drive_open(const char *id, struct rip_transport *t) {
  if (!strcmp(id, "virtual0")) {
    struct rip_image_drive_opts opts;
    const char *image = getenv("RIPPER_IMAGE");
    int rc;
    memset(&opts, 0, sizeof(opts));
    opts.cd = !ps5_like();
    opts.sony = ps5_like();
    rc = rip_image_drive_open(t, &opts);
    if (rc == RIP_OK && image && image[0]) {
      rip_image_drive_load(t, image);
    }
    return rc;
  }
  if (!strncmp(id, "sr", 2)) {
    char dev[32];
    snprintf(dev, sizeof(dev), "/dev/%s", id);
    return sgio_transport_open(t, dev);
  }
  return RIP_ENOENT;
}

int plat_drive_reader(const char *id, struct rip_transport *t, struct rip_reader *r) {
  (void)id;
  return rip_reader_open_mmc(r, t);
}

const char *plat_drive_mount(const char *id) {
  (void)id;
  return NULL;
}

int plat_drive_load(const char *id, struct rip_transport *t, const char *image_path) {
  if (strcmp(id, "virtual0") != 0) {
    return RIP_ENOTSUP;
  }
  if (!image_path[0]) {
    rip_image_drive_eject(t);
    return RIP_OK;
  }
  return rip_image_drive_load(t, image_path);
}

int plat_storage_roots(struct plat_root *out, int max) {
  int n = 0;
  const char *home = getenv("HOME");
  char data[SVC_PATH];

  if (n < max && realpath(plat_data_dir(), data)) {
    svc_str_copy(out[n].path, sizeof(out[n].path), data);
    svc_str_copy(out[n].label, sizeof(out[n].label), "Data folder");
    n++;
  }
  if (n < max && home) {
    svc_str_copy(out[n].path, sizeof(out[n].path), home);
    svc_str_copy(out[n].label, sizeof(out[n].label), "Home");
    n++;
  }
  return n;
}

void plat_game_status(const char *title_id, struct plat_dump_status *st) {
  (void)title_id;
  memset(st, 0, sizeof(*st));
}

int plat_game_list(struct plat_game *out, int max) {
  (void)out;
  (void)max;
  return 0;
}

int plat_game_dump(const char *title_id, const char *out_dir, int complete, unsigned copies, plat_progress_fn progress,
                   void *ud, volatile int *cancel, char *err, size_t en) {
  (void)title_id;
  (void)complete;
  (void)copies;
  (void)out_dir;
  (void)progress;
  (void)ud;
  (void)cancel;
  snprintf(err, en, "game dumps need the console");
  return RIP_ENOTSUP;
}

const char *plat_ca_pem(size_t *len) {
  *len = 0;
  return NULL;
}
