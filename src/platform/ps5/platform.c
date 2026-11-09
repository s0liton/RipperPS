// The service's platform on the PS5.
//
// Drives are the kernel's CAM cd peripherals: cd0 is the console's own drive,
// and any further cdN is taken to be a USB drive (to be checked with one).
// Discs are read through /dev/cdN, with READ(12) through pass-through as the
// fallback. The process list, the single-copy check and notifications follow
// RommPS (platform/console in its repository), itself after John Törnblom's
// ftpsrv and websrv.

#include "platform.h"

#include <arpa/inet.h>
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <ifaddrs.h>
#include <netinet/in.h>
#include <ps5/kernel.h>
#include <ps5/klog.h>
#include <pthread.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/cpuset.h>
#include <sys/disk.h>
#include <sys/file.h>
#include <sys/ioctl.h>
#include <sys/param.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/sysctl.h>
#include <time.h>
#include <ucontext.h>
#include <unistd.h>

#include "cJSON.h"
#include "cam_transport.h"
#include "ripper/common.h"
#include "ripper/log.h"
#include "ripper/util.h"
#include "ripperps.h"
#include "svc.h"

#define DATA_DIR "/data/ripper"
#define PROC_NAME "ripper.elf"
#define MAX_CD_UNITS 4
// The AuthID RommPS runs with: enough for the system services it calls.
#define RIPPER_AUTHID 0x4801000000000013L

const char *plat_name(void) {
  return "ps5";
}

const char *plat_console(void) {
  return "PlayStation 5";
}

/* One copy at a time */

// sysctl(KERN_PROC) read with the console's kinfo_proc offsets (ftpsrv):
// ki_structsize at 0, ki_pid at 72, ki_tdname at 447.
static pid_t find_other(const char *name) {
  int mib[4] = {CTL_KERN, KERN_PROC, KERN_PROC_PROC, 0};
  size_t size = 0;
  uint8_t *buf;
  uint8_t *p;
  pid_t found = -1;

  if (sysctl(mib, 4, NULL, &size, NULL, 0) || size == 0) {
    return -1;
  }
  size += size / 4;
  buf = malloc(size);
  if (!buf || sysctl(mib, 4, buf, &size, NULL, 0)) {
    free(buf);
    return -1;
  }
  for (p = buf; p + 464 <= buf + size;) {
    int structsize = *(int *)p;
    pid_t pid = *(pid_t *)&p[72];
    char tdname[17];
    if (structsize < 464 || p + structsize > buf + size) {
      break;
    }
    memcpy(tdname, &p[447], 16);
    tdname[16] = '\0';
    p += structsize;
    if (pid != getpid() && !strcmp(tdname, name)) {
      found = pid;
      break;
    }
  }
  free(buf);
  return found;
}

// Threads the libraries start (x264's, FFmpeg's) get the system's default
// stack, too small for x264's analysis: every thread gets 2 MB at least. The
// link wraps pthread_create (ps5.mk), so this covers code that isn't ours.
#define MIN_THREAD_STACK (2u << 20)
int __real_pthread_create(pthread_t *thread, const pthread_attr_t *attr, void *(*fn)(void *), void *arg);
int __wrap_pthread_create(pthread_t *thread, const pthread_attr_t *attr, void *(*fn)(void *), void *arg) {
  pthread_attr_t own;
  size_t size = 0;
  int rc;

  if (attr && pthread_attr_getstacksize(attr, &size) == 0 && size >= MIN_THREAD_STACK) {
    return __real_pthread_create(thread, attr, fn, arg);
  }
  if (attr) {
    own = *attr;  // keeps the caller's other settings
  } else if (pthread_attr_init(&own) != 0) {
    return __real_pthread_create(thread, attr, fn, arg);
  }
  pthread_attr_setstacksize(&own, MIN_THREAD_STACK);
  rc = __real_pthread_create(thread, &own, fn, arg);
  if (!attr) {
    pthread_attr_destroy(&own);
  }
  return rc;
}

// A fault is written to crash.log before the process goes: the signal, where,
// and the frame-pointer chain, with this function's own address so the
// addresses can be matched to the ELF (llvm-symbolizer, base = this - its
// address in the ELF).
static int g_crash_fd = -1;

static void crash_line(const char *text) {
  if (g_crash_fd >= 0) {
    (void)!write(g_crash_fd, text, strlen(text));
  }
}

static void on_crash(int sig, siginfo_t *si, void *ctx) {
  char line[200];
  // The handler's own frame chain: the signal's trampoline keeps the
  // interrupted code's frame pointer, so the walk goes on into it. (The
  // context's pc reads as garbage on the console.)
  uintptr_t *fp = (uintptr_t *)__builtin_frame_address(0);
  long tid = 0;
  int depth;

  (void)ctx;
  syscall(SYS_thr_self, &tid);
  snprintf(line, sizeof(line), "%ld: signal %d at %p, thread %ld, on_crash %p\n", (long)time(NULL), sig,
           si ? si->si_addr : NULL, tid, (void *)on_crash);
  crash_line(line);
  for (depth = 0; depth < 32 && fp && ((uintptr_t)fp & 7) == 0; depth++) {
    uintptr_t ret = fp[1];
    if (!ret) {
      break;
    }
    snprintf(line, sizeof(line), "  #%d %#lx\n", depth, (unsigned long)ret);
    crash_line(line);
    if ((uintptr_t *)fp[0] <= fp) {
      break;
    }
    fp = (uintptr_t *)fp[0];
  }
  RIP_ERROR("crashed: signal %d (crash.log has where)", sig);
  _exit(128 + sig);
}

static void install_crash_handler(void) {
  static const int sigs[] = {SIGSEGV, SIGBUS, SIGILL, SIGFPE, SIGABRT};
  struct sigaction sa;
  size_t i;

  g_crash_fd = open(DATA_DIR "/crash.log", O_WRONLY | O_CREAT | O_APPEND, 0666);
  memset(&sa, 0, sizeof(sa));
  sa.sa_sigaction = on_crash;
  sa.sa_flags = SA_SIGINFO | SA_RESETHAND;
  for (i = 0; i < sizeof(sigs) / sizeof(sigs[0]); i++) {
    sigaction(sigs[i], &sa, NULL);
  }
}

// A payload starts in a CPU set of 5 of the 16 (9, 11, 13, 14 and 15, shared
// with the system's UI); apps get 0 to 12. Encoding a movie on those 5 crawls,
// so ripper joins the root set, all 16. Idle, its threads use none of them.
static void widen_cpus(void) {
  cpuset_t set;
  int i;
  int n = 0;

  if (cpuset_setid(CPU_WHICH_PID, -1, 0) != 0) {
    RIP_WARN("CPUs: staying in the payload's set (%s)", strerror(errno));
    return;
  }
  CPU_ZERO(&set);
  if (cpuset_getaffinity(CPU_LEVEL_WHICH, CPU_WHICH_PID, -1, 8, &set) == 0) {
    for (i = 0; i < 64; i++) {
      n += CPU_ISSET(i, &set) != 0;
    }
  }
  RIP_INFO("CPUs: %d to work on", n);
}

static void *focus_thread(void *arg);

int plat_init(void) {
  pid_t old;
  int tries = 0;

  signal(SIGPIPE, SIG_IGN);
  kernel_set_ucred_authid(getpid(), RIPPER_AUTHID);
  // The copy sent last wins: stop the one running, then take its name.
  while ((old = find_other(PROC_NAME)) > 0 && tries++ < 10) {
    kill(old, SIGKILL);
    sleep(1);
  }
  syscall(SYS_thr_set_name, -1, PROC_NAME);
  rip_mkdir_p(DATA_DIR);
  install_crash_handler();
  widen_cpus();
  svc_thread(focus_thread, NULL);
  // Held for as long as this copy runs: RipperPS reads it to tell a copy
  // that's starting (or waking) from none at all.
  {
    int lock = open(DATA_DIR "/ripper.lock", O_RDWR | O_CREAT, 0666);
    if (lock >= 0 && flock(lock, LOCK_EX | LOCK_NB) != 0) {
      RIP_WARN("ripper.lock: %s", strerror(errno));
    }
  }
  return 0;
}

const char *plat_data_dir(void) {
  return DATA_DIR;
}

const char *plat_rommps_dir(void) {
  return "/data/romm-sync";
}

void plat_log_line(enum rip_log_level level, const char *line) {
  static const char *names[] = {"debug", "info", "warn", "error"};

  klog_printf("[ripper][%s] %s\n", names[level], line);
}

/* Notifications */

// sceNotificationSend draws the toast in the system's own style; it loads in
// the loader's process, not in apps. When it isn't there, the kernel's plain
// notification. Requests are queued for a thread of their own: the shell can
// hang, and nothing should wait on it.
typedef struct notify_request {
  char useless1[45];
  char message[3075];
} notify_request_t;
int sceKernelSendNotificationRequest(int, notify_request_t *, size_t, int);
typedef int (*notification_send_fn)(int user_id, bool logged, const char *json);

#define NOTIFY_QUEUE 8
static pthread_mutex_t g_notify_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_notify_cond = PTHREAD_COND_INITIALIZER;
static char g_queue[NOTIFY_QUEUE][2][256];
static int g_head;
static int g_count;
static int g_started;
static notification_send_fn g_send;

static int toast(const char *title, const char *body) {
  static unsigned serial;
  char created[32];
  char id[24];
  struct timespec ts;
  struct tm tm;
  cJSON *root;
  cJSON *raw;
  cJSON *view;
  cJSON *icon;
  cJSON *plain;
  char *json;
  int rc;

  if (!g_send) {
    return -1;
  }
  clock_gettime(CLOCK_REALTIME, &ts);
  gmtime_r(&ts.tv_sec, &tm);
  snprintf(created, sizeof(created), "%04d-%02d-%02dT%02d:%02d:%02d.%03ldZ", tm.tm_year + 1900, tm.tm_mon + 1,
           tm.tm_mday, tm.tm_hour, tm.tm_min, tm.tm_sec, ts.tv_nsec / 1000000);
  snprintf(id, sizeof(id), "%u", (unsigned)(ts.tv_sec % 100000000) * 10 + (serial++ % 10));
  root = cJSON_CreateObject();
  raw = cJSON_AddObjectToObject(root, "rawData");
  cJSON_AddStringToObject(raw, "viewTemplateType", "InteractiveToastTemplateB");
  cJSON_AddStringToObject(raw, "channelType", "Downloads");
  cJSON_AddStringToObject(raw, "useCaseId", "IDC");
  cJSON_AddStringToObject(raw, "toastOverwriteType", "No");
  cJSON_AddBoolToObject(raw, "isImmediate", 1);
  cJSON_AddNumberToObject(raw, "priority", 100);
  view = cJSON_AddObjectToObject(raw, "viewData");
  icon = cJSON_AddObjectToObject(view, "icon");
  cJSON_AddStringToObject(icon, "type", "Predefined");
  cJSON_AddStringToObject(cJSON_AddObjectToObject(icon, "parameters"), "icon", "download");
  cJSON_AddStringToObject(cJSON_AddObjectToObject(view, "message"), "body", title);
  if (body && body[0]) {
    cJSON_AddStringToObject(cJSON_AddObjectToObject(view, "subMessage"), "body", body);
  }
  plain = cJSON_AddObjectToObject(
      cJSON_AddObjectToObject(cJSON_AddObjectToObject(raw, "platformViews"), "previewDisabled"), "viewData");
  icon = cJSON_AddObjectToObject(plain, "icon");
  cJSON_AddStringToObject(icon, "type", "Predefined");
  cJSON_AddStringToObject(cJSON_AddObjectToObject(icon, "parameters"), "icon", "download");
  cJSON_AddStringToObject(cJSON_AddObjectToObject(plain, "message"), "body", title);
  cJSON_AddStringToObject(root, "createdDateTime", created);
  cJSON_AddStringToObject(root, "localNotificationId", id);
  json = cJSON_PrintUnformatted(root);
  cJSON_Delete(root);
  rc = json ? g_send(0xFE, true, json) : -1;
  free(json);
  return rc < 0 ? -1 : 0;
}

static void *notify_thread(void *arg) {
  void *lib = dlopen("libSceNotification.sprx", RTLD_LAZY);

  (void)arg;
  g_send = lib ? (notification_send_fn)dlsym(lib, "sceNotificationSend") : NULL;
  for (;;) {
    char title[256];
    char body[256];
    pthread_mutex_lock(&g_notify_lock);
    while (!g_count) {
      pthread_cond_wait(&g_notify_cond, &g_notify_lock);
    }
    memcpy(title, g_queue[g_head][0], sizeof(title));
    memcpy(body, g_queue[g_head][1], sizeof(body));
    g_head = (g_head + 1) % NOTIFY_QUEUE;
    g_count--;
    pthread_mutex_unlock(&g_notify_lock);
    if (toast(title, body) != 0) {
      notify_request_t req;
      memset(&req, 0, sizeof(req));
      snprintf(req.message, sizeof(req.message), "%s%s%s", title, body[0] ? "\n" : "", body);
      sceKernelSendNotificationRequest(0, &req, sizeof(req), 0);
    }
  }
  return NULL;
}

// sceSystemServicePowerTick resets the idle timer behind auto rest mode, as
// media players do while they play. Looked up once, at the first call.
void plat_keep_awake(void) {
  static int (*tick)(void);
  static int looked;

  if (!looked) {
    void *lib = dlopen("libSceSystemService.sprx", RTLD_LAZY);
    tick = lib ? (int (*)(void))dlsym(lib, "sceSystemServicePowerTick") : NULL;
    looked = 1;
    if (!tick) {
      RIP_WARN("sceSystemServicePowerTick isn't there: rest mode can cut long rips short");
    }
  }
  if (tick) {
    tick();
  }
}

// A disc going in over RipperPS (in front now, or a moment ago: the system
// often reacts before the drive's next poll) keeps it in front this long.
// The system takes the front more than once: its disc app (NPXS40087) 14 to
// 18 s after the disc goes in, then the home screen some time later.
#define FRONT_GRACE_MS 6000
#define FRONT_GUARD_MS 60000
// Every move of the focus is logged this long after a disc goes in.
#define FOCUS_LOG_MS 90000
#define FOCUS_POLL_MS 250
#define EVENT_FLAG_WAITMODE_OR 2u
#define EVENT_FLAG_EBUSY ((int)0x80020010u)

typedef intptr_t event_flag_t;
int sceKernelOpenEventFlag(event_flag_t *ef, const char *name);
int sceKernelPollEventFlag(event_flag_t ef, uint64_t bits, unsigned int mode, uint64_t *out);

typedef uint32_t (*running_app_fn)(void);
typedef int (*app_title_fn)(uint32_t app_id, char *title_id);
typedef int (*set_focus_fn)(uint32_t app_id, int flags);

static pthread_mutex_t g_focus_lock = PTHREAD_MUTEX_INITIALIZER;
static uint32_t g_front_app;    // RipperPS's app id, when it was last in front
static uint64_t g_front_at;     // when it last was (rip_now_ms)
static uint64_t g_guard_until;  // keep it in front until then
static uint64_t g_log_until;    // log the focus's moves until then
static int g_in_front;          // RipperPS has the focus now

// The focus moved to app_id: whether that's RipperPS. Logged for a while after
// a disc goes in, to see what the system does with the focus.
static int focus_moved(uint32_t app_id, app_title_fn title, running_app_fn running) {
  char tid[32] = "";
  int known = app_id && title(app_id, tid) == 0;

  if (!known) {
    snprintf(tid, sizeof(tid), "%s", app_id ? "no title" : "nobody");
  }
  pthread_mutex_lock(&g_focus_lock);
  if (rip_now_ms() < g_log_until) {
    RIP_INFO("disc in: the focus is on app %#x (%s), running app %#x", app_id, tid, running());
  }
  pthread_mutex_unlock(&g_focus_lock);
  return known && strcmp(tid, RIPPERPS_TITLE) == 0;
}

// Which app has the focus is a flag the shell keeps (low 32 bits: the app id),
// read the way ShadowMountPlus does. Polled all along, so a disc going in
// knows whether RipperPS was in front just before the system's screen came
// up. While a disc's guard is on, the focus goes straight back to RipperPS
// each time the system takes it: one call, once the system has it (calls
// made before that are answered 0 and change nothing).
static void *focus_thread(void *arg) {
  event_flag_t ef = 0;
  void *lib = dlopen("libSceSystemService.sprx", RTLD_LAZY);
  running_app_fn running = lib ? (running_app_fn)dlsym(lib, "sceLncUtilGetAppIdOfRunningBigApp") : NULL;
  app_title_fn title = lib ? (app_title_fn)dlsym(lib, "sceLncUtilGetAppTitleId") : NULL;
  set_focus_fn set_focus = lib ? (set_focus_fn)dlsym(lib, "sceLncUtilSetAppFocus") : NULL;
  uint32_t last = 0;
  int ours = 0;  // the focused app is RipperPS
  (void)arg;

  if (!running || !title || !set_focus || sceKernelOpenEventFlag(&ef, "SceShellCoreUtilAppFocus") != 0) {
    RIP_WARN("the app focus isn't readable: RipperPS isn't kept in front when a disc goes in");
    return NULL;
  }
  for (;;) {
    uint64_t pattern = 0;
    uint64_t now = rip_now_ms();
    uint32_t focus;
    int rc = sceKernelPollEventFlag(ef, UINT64_MAX, EVENT_FLAG_WAITMODE_OR, &pattern);

    focus = rc == 0 ? (uint32_t)pattern : 0;
    if (rc != 0 && rc != EVENT_FLAG_EBUSY) {
      usleep(FOCUS_POLL_MS * 1000u);
      continue;
    }
    if (focus != last) {
      ours = focus_moved(focus, title, running);
      last = focus;
    }
    pthread_mutex_lock(&g_focus_lock);
    g_in_front = ours;
    if (ours) {
      g_front_app = focus;
      g_front_at = now;
    } else if (focus && g_front_app && now < g_guard_until) {
      uint32_t app = g_front_app;
      pthread_mutex_unlock(&g_focus_lock);
      if (running() == app) {
        rc = set_focus(app, 0);
        RIP_INFO("disc in: RipperPS given the focus back from app %#x: %#x", focus, (unsigned)rc);
        usleep(500 * 1000u);  // the change shows on the flag after a moment
      }
      pthread_mutex_lock(&g_focus_lock);
    }
    pthread_mutex_unlock(&g_focus_lock);
    usleep(FOCUS_POLL_MS * 1000u);
  }
  return NULL;
}

void plat_disc_inserted(void) {
  uint64_t now = rip_now_ms();

  pthread_mutex_lock(&g_focus_lock);
  g_log_until = now + FOCUS_LOG_MS;
  if (g_front_app && now - g_front_at < FRONT_GRACE_MS) {
    g_guard_until = now + FRONT_GUARD_MS;
    RIP_INFO("disc in: keeping RipperPS in front");
  }
  pthread_mutex_unlock(&g_focus_lock);
}

int plat_app_in_front(void) {
  int in_front;

  pthread_mutex_lock(&g_focus_lock);
  in_front = g_in_front;
  pthread_mutex_unlock(&g_focus_lock);
  return in_front;
}

void plat_notify(const char *title, const char *body) {
  pthread_mutex_lock(&g_notify_lock);
  if (!g_started) {
    g_started = svc_thread(notify_thread, NULL) == 0;
  }
  if (g_started && g_count < NOTIFY_QUEUE) {
    int slot = (g_head + g_count) % NOTIFY_QUEUE;
    svc_str_copy(g_queue[slot][0], sizeof(g_queue[slot][0]), title);
    svc_str_copy(g_queue[slot][1], sizeof(g_queue[slot][1]), body ? body : "");
    g_count++;
    pthread_cond_signal(&g_notify_cond);
  }
  pthread_mutex_unlock(&g_notify_lock);
}

void plat_local_ip(char *buf, size_t n) {
  struct ifaddrs *ifs = NULL;
  struct ifaddrs *i;
  char ip[INET_ADDRSTRLEN];

  buf[0] = '\0';
  if (getifaddrs(&ifs) != 0) {
    return;
  }
  for (i = ifs; i; i = i->ifa_next) {
    if (!i->ifa_addr || i->ifa_addr->sa_family != AF_INET || (i->ifa_name && !strncmp(i->ifa_name, "lo", 2))) {
      continue;
    }
    if (inet_ntop(AF_INET, &((struct sockaddr_in *)i->ifa_addr)->sin_addr, ip, sizeof(ip)) && strncmp(ip, "0.", 2)) {
      snprintf(buf, n, "%s", ip);
      break;
    }
  }
  freeifaddrs(ifs);
}

/* Drives */

static int unit_of(const char *id) {
  if (strncmp(id, "cd", 2) != 0 || id[2] < '0' || id[2] > '9' || id[3]) {
    return -1;
  }
  return id[2] - '0';
}

int plat_drive_list(struct plat_drive *out, int max) {
  int n = 0;
  int unit;

  for (unit = 0; unit < MAX_CD_UNITS && n < max; unit++) {
    char dev[32];
    struct stat st;
    snprintf(dev, sizeof(dev), "/dev/cd%d", unit);
    if (stat(dev, &st) != 0) {
      continue;
    }
    memset(&out[n], 0, sizeof(out[n]));
    snprintf(out[n].id, sizeof(out[n].id), "cd%d", unit);
    if (unit == 0) {
      snprintf(out[n].label, sizeof(out[n].label), "Built-in Blu-ray");
      out[n].source = RIP_DRIVE_INTERNAL;
    } else {
      snprintf(out[n].label, sizeof(out[n].label), unit == 1 ? "USB drive" : "USB drive %d", unit);
      out[n].source = RIP_DRIVE_EXTERNAL;
    }
    n++;
  }
  return n;
}

int plat_drive_open(const char *id, struct rip_transport *t) {
  int unit = unit_of(id);

  return unit < 0 ? RIP_ENOENT : cam_transport_open(t, "cd", (unsigned)unit);
}

// The kernel's block device is the path known to work for PS3 discs on the
// internal drive; READ(12) through pass-through is the fallback.
int plat_drive_reader(const char *id, struct rip_transport *t, struct rip_reader *r) {
  char dev[32];
  off_t media = 0;
  u_int sector = 0;
  int fd;

  snprintf(dev, sizeof(dev), "/dev/%s", id);
  fd = open(dev, O_RDONLY);
  if (fd >= 0) {
    if (ioctl(fd, DIOCGMEDIASIZE, &media) == 0 && media > 0 && ioctl(fd, DIOCGSECTORSIZE, &sector) == 0 && sector > 0 &&
        rip_reader_open_fd(r, fd, sector, (uint64_t)media, 1) == RIP_OK) {
      return RIP_OK;
    }
    close(fd);
  }
  return rip_reader_open_mmc(r, t);
}

const char *plat_drive_mount(const char *id) {
  return !strcmp(id, "cd0") ? "/mnt/disc" : NULL;
}

int plat_drive_load(const char *id, struct rip_transport *t, const char *image_path) {
  (void)id;
  (void)t;
  (void)image_path;
  return RIP_ENOTSUP;
}

int plat_storage_roots(struct plat_root *out, int max) {
  int n = 0;
  int i;

  if (n < max) {
    svc_str_copy(out[n].path, sizeof(out[n].path), "/data");
    svc_str_copy(out[n].label, sizeof(out[n].label), "Console storage (/data)");
    n++;
  }
  for (i = 0; i < 8 && n < max; i++) {
    char path[32];
    snprintf(path, sizeof(path), "/mnt/usb%d", i);
    if (svc_dir_exists(path)) {
      svc_str_copy(out[n].path, sizeof(out[n].path), path);
      snprintf(out[n].label, sizeof(out[n].label), "USB drive %d (%s)", i + 1, path);
      n++;
    }
  }
  for (i = 0; i < 2 && n < max; i++) {
    char path[32];
    snprintf(path, sizeof(path), "/mnt/ext%d", i);
    if (svc_dir_exists(path)) {
      svc_str_copy(out[n].path, sizeof(out[n].path), path);
      snprintf(out[n].label, sizeof(out[n].label), "Extended storage (%s)", path);
      n++;
    }
  }
  return n;
}

const char *plat_ca_pem(size_t *len) {
  *len = 0;
  return NULL;
}
