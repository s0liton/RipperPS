// PS4 and PS5 game dumps (platform.h plat_game_*).
//
// A game disc's files are encrypted with keys only the console holds, and a
// disc game is installed before it runs. While the game is mounted, the
// system shows its files decrypted under /mnt/sandbox/pfsmnt/<title>-app0;
// what is still encrypted there are its executables (eboot.bin, .prx, .sprx:
// SELFs), which idlesauce's ps5-self-pager decrypts through the kernel's own
// SELF pager. The game's metadata (param.sfo, icons) comes from the system's
// copy of it in appmeta. The result is the game folder shadPS4 (PS4) and
// backup loaders (PS5) take.
//
// Mounting needs the game started: done through the shell's launch service,
// the way homebrew launchers do it (the entry points have no SDK header).

#include <dirent.h>
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include "cJSON.h"
#include "platform.h"
#include "ripper/common.h"
#include "ripper/log.h"
#include "ripper/pkg.h"
#include "ripper/sfo.h"
#include "ripper/util.h"
#include "ripperps.h"
#include "selfpager.h"
#include "svc.h"

#define PFSMNT "/mnt/sandbox/pfsmnt"
#define MOUNT_WAIT_SEC 90
#define CLOSE_WAIT_MS 15000
#define LNC_BUSY 0x80940010u  // the system is still closing the last app
#define COPY_CHUNK (4u << 20)
#define SELF_ORBIS_MAGIC 0x1D3D154Fu
#define SELF_PROSPERO_MAGIC 0xEEF51454u

static const char *const APP_ROOTS[] = {"/user/app", "/mnt/ext0/user/app", "/mnt/ext1/user/app", NULL};

/* Launching */

struct launch_ctx {
  uint32_t structsize;
  uint32_t user_id;
  uint32_t app_opt;
  uint64_t crash_report;
  uint32_t check_flag;
};

static pthread_once_t g_once = PTHREAD_ONCE_INIT;
static int (*p_running_bigapp)(void);
static int (*p_app_title)(uint32_t app_id, char *title_id);
static int (*p_kill_app)(int app_id, int how, int reason, int core_dump);
static int (*p_lnc_init)(void);
static int (*p_lnc_launch)(const char *title_id, char **argv, struct launch_ctx *ctx);
static int (*p_launch)(const char *title_id, char **argv, struct launch_ctx *ctx);
static int (*p_user_init)(void *unused);
static int (*p_user_foreground)(uint32_t *user_id);

// Resolved on first use: linked in, a missing library would stop the payload from starting.
static void resolve(void) {
  void *sys = dlopen("libSceSystemService.sprx", RTLD_LAZY);
  void *user = dlopen("libSceUserService.sprx", RTLD_LAZY);

  if (sys) {
    p_running_bigapp = (int (*)(void))dlsym(sys, "sceSystemServiceGetAppIdOfRunningBigApp");
    p_app_title = (int (*)(uint32_t, char *))dlsym(sys, "sceLncUtilGetAppTitleId");
    p_kill_app = (int (*)(int, int, int, int))dlsym(sys, "sceSystemServiceKillApp");
    p_lnc_init = (int (*)(void))dlsym(sys, "sceLncUtilInitialize");
    p_lnc_launch = (int (*)(const char *, char **, struct launch_ctx *))dlsym(sys, "sceLncUtilLaunchApp");
    p_launch = (int (*)(const char *, char **, struct launch_ctx *))dlsym(sys, "sceSystemServiceLaunchApp");
  }
  if (user) {
    p_user_init = (int (*)(void *))dlsym(user, "sceUserServiceInitialize");
    p_user_foreground = (int (*)(uint32_t *))dlsym(user, "sceUserServiceGetForegroundUser");
  }
  if (p_user_init) {
    p_user_init(NULL);  // 0x80960003 when it already was: fine
  }
  RIP_INFO("game dumps: launch service %s", (p_lnc_launch || p_launch) && p_running_bigapp ? "found" : "missing");
}

static int running_app(void) {
  pthread_once(&g_once, resolve);
  return p_running_bigapp ? p_running_bigapp() : -1;
}

static const char *launch_error(int rc) {
  switch ((unsigned)rc) {
  case 0x8094000Cu:
    return "the console still has this game open; close it and try again";
  case 0x80940005u:
    return "the console refused to start this game";
  case 0x80940003u:
    return "the console doesn't know this game; install it first";
  case 0x80A40009u:
  case 0x80A40027u:
    return "the game wants its disc in the drive";
  case 0x80A40028u:
    return "the console is still reading the disc; try again in a moment";
  default:
    return NULL;
  }
}

// Starts title_id, closing the app running first; closed gets that app's
// title ID ("" if none), to start it again after.
static int launch(const char *title_id, char *closed, size_t cn, char *err, size_t en) {
  struct launch_ctx ctx;
  uint32_t user = 0xFFFFFFFFu;
  char *argv[1] = {NULL};
  int have_user;
  int app;
  int rc = -1;
  uint64_t until;

  pthread_once(&g_once, resolve);
  closed[0] = '\0';
  if (!(p_lnc_launch || p_launch) || !p_running_bigapp) {
    snprintf(err, en, "this console's launch service isn't reachable");
    return RIP_ERR;
  }
  app = p_running_bigapp();
  if (app > 0 && p_kill_app) {
    char tid[32] = "";
    if (p_app_title && p_app_title((uint32_t)app, tid) == 0) {
      snprintf(closed, cn, "%s", tid);
    }
    RIP_INFO("closing the running app %d (%s) to start %s", app, tid[0] ? tid : "?", title_id);
    p_kill_app(app, -1, 0, 0);
    // Gone when the system says nothing runs; started before that, the launch is put off.
    for (until = rip_now_ms() + CLOSE_WAIT_MS; p_running_bigapp() == app && rip_now_ms() < until;) {
      usleep(250000);
    }
    sleep(1);
  }
  memset(&ctx, 0, sizeof(ctx));
  have_user = p_user_foreground && p_user_foreground(&user) == 0 && user != 0xFFFFFFFFu;
  if (have_user) {
    ctx.structsize = sizeof(ctx);
    ctx.user_id = user;
  }
  // The shell's own entry point first: the system service refuses some titles.
  if (p_lnc_launch) {
    if (p_lnc_init) {
      p_lnc_init();
    }
    rc = p_lnc_launch(title_id, argv, have_user ? &ctx : NULL);
  }
  if (rc < 0 && p_launch) {
    rc = p_launch(title_id, argv, have_user ? &ctx : NULL);
  }
  // Still closing the last app: the system starts this one once that's done.
  if ((unsigned)rc == LNC_BUSY) {
    RIP_INFO("%s starts once the app before it has closed", title_id);
    return RIP_OK;
  }
  if (rc < 0) {
    const char *why = launch_error(rc);
    snprintf(err, en, "%s (0x%08x)", why ? why : "the console wouldn't start the game", (unsigned)rc);
    return RIP_ERR;
  }
  RIP_INFO("started %s so the system mounts it", title_id);
  return RIP_OK;
}

/* Status */

static int has_entries(const char *dir) {
  DIR *d = opendir(dir);
  struct dirent *e;
  int found = 0;

  while (d && !found && (e = readdir(d)) != NULL) {
    found = e->d_name[0] != '.';
  }
  if (d) {
    closedir(d);
  }
  return found;
}

// "CUSA-02557" (as ripper shows it) -> "CUSA02557" (as the system names it).
static void system_id(const char *title_id, char *out, size_t n) {
  size_t o = 0;

  for (; *title_id && o + 1 < n; title_id++) {
    if (*title_id != '-') {
      out[o++] = *title_id;
    }
  }
  out[o] = '\0';
}

// Where the title is installed: "/user/app/CUSA06122" (internal) or an extended storage's.
static int app_dir(const char *id, char *out, size_t n) {
  int i;

  for (i = 0; APP_ROOTS[i]; i++) {
    char pkg[SVC_PATH];
    snprintf(out, n, "%s/%s", APP_ROOTS[i], id);
    snprintf(pkg, sizeof(pkg), "%s/app.pkg", out);
    if (rip_file_exists(pkg)) {
      return 1;
    }
  }
  out[0] = '\0';
  return 0;
}

// app.json (downloads) gives the package's full size. app.xml, PlayGo's record
// of which chunks are on the console, isn't a guide: it's written when a disc
// game becomes playable and not again when the rest has copied (the install
// task says that, install_task()).
static int install_complete(const char *dir, uint64_t size, uint64_t *expected) {
  char path[SVC_PATH];
  char *txt;
  int complete = 1;

  *expected = 0;
  svc_path_join(path, sizeof(path), dir, "app.json");
  txt = svc_read_file(path, NULL);
  if (txt) {
    cJSON *j = cJSON_Parse(txt);
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(j, "originalFileSize");
    if (cJSON_IsNumber(v)) {
      *expected = (uint64_t)v->valuedouble;
    }
    cJSON_Delete(j);
    free(txt);
  }
  if (*expected && size < *expected) {
    complete = 0;
  }
  return complete;
}

// Chunks this console never fetches (other languages or regions): in app.xml
// without req_locus, or left out of its list (chunk_count says how many there
// are). Which chunks are wanted is written when the install starts, so this
// part of the file holds. Their files are in the game's mount with no data,
// and a read of one blocks for good: the game can't close, nor ripper be
// stopped, until the console restarts.
static int chunks_left_out(const char *dir) {
  char path[SVC_PATH];
  char *txt;
  const char *p;
  const char *count;
  int listed = 0;
  int wanted = 0;
  int left_out;

  svc_path_join(path, sizeof(path), dir, "app.xml");
  txt = svc_read_file(path, NULL);
  if (!txt) {
    return 0;
  }
  for (p = strstr(txt, "<chunk "); p; p = strstr(p + 1, "<chunk ")) {
    const char *end = strchr(p, '>');
    const char *req = strstr(p, " req_locus=\"");
    listed++;
    wanted += end && req && req < end;
  }
  count = strstr(txt, " chunk_count=\"");
  left_out = wanted < listed || (count && atoi(count + 14) > listed);
  free(txt);
  return left_out;
}

// The system's install tasks, a folder each, with the record twice (d0.pdb,
// d1.pdb): after 4 bytes, fields of tag, length, length again and value. A
// disc install has one from the start until well after it's done.
#define BGFT_TASKS "/user/bgft/task"
#define TASK_COPIED 0x7A
#define TASK_TO_COPY 0x7B
#define TASK_TITLE_ID 0x7C

static uint64_t task_number(const uint8_t *v, uint32_t n) {
  uint64_t x = 0;

  while (n--) {
    x = x << 8 | v[n];
  }
  return x;
}

// How far the system is with copying id: 1 when it has a task for it.
static int install_task(const char *id, uint64_t *copied, uint64_t *to_copy) {
  DIR *dp = opendir(BGFT_TASKS);
  struct dirent *e;
  int found = 0;

  *copied = *to_copy = 0;
  while (dp && (e = readdir(dp)) != NULL) {
    static const char *const COPIES[] = {"d0.pdb", "d1.pdb"};
    int c;
    if (e->d_name[0] == '.') {
      continue;
    }
    for (c = 0; c < 2; c++) {
      char path[SVC_PATH];
      size_t len = 0;
      size_t o = 4;
      uint8_t *rec;
      uint64_t got = 0;
      uint64_t total = 0;
      int mine = 0;
      snprintf(path, sizeof(path), BGFT_TASKS "/%.64s/%s", e->d_name, COPIES[c]);
      rec = (uint8_t *)svc_read_file(path, &len);
      while (rec && o + 12 <= len) {
        uint32_t tag = (uint32_t)task_number(rec + o, 4);
        uint32_t n = (uint32_t)task_number(rec + o + 4, 4);
        const uint8_t *v = rec + o + 12;
        if (n > len - o - 12) {
          break;
        }
        if (tag == TASK_TITLE_ID) {
          mine = n == strlen(id) && !memcmp(v, id, n);
        } else if (tag == TASK_COPIED && n == 8) {
          got = task_number(v, n);
        } else if (tag == TASK_TO_COPY && n == 8) {
          total = task_number(v, n);
        }
        o += 12 + n;
      }
      free(rec);
      // The two copies are written in turn: the further one is the newer.
      if (mine && got >= *copied) {
        *copied = got;
        *to_copy = total;
        found = 1;
      }
    }
  }
  if (dp) {
    closedir(dp);
  }
  return found;
}

void plat_game_status(const char *title_id, struct plat_dump_status *st) {
  char id[16];
  char dir[SVC_PATH];
  char path[SVC_PATH];

  memset(st, 0, sizeof(*st));
  st->supported = 1;
  system_id(title_id, id, sizeof(id));
  st->installing = install_task(id, &st->copied, &st->to_copy) && st->copied < st->to_copy;
  if (app_dir(id, dir, sizeof(dir))) {
    int64_t size;
    st->installed = 1;
    svc_path_join(path, sizeof(path), dir, "app.pkg");
    size = svc_file_size(path);
    st->size = size > 0 ? (uint64_t)size : 0;
    st->complete = !st->installing && install_complete(dir, st->size, &st->expected_size);
    st->partial = chunks_left_out(dir);
    // A disc game has no download record: its disc's package says how big it is.
    if (!st->expected_size) {
      snprintf(path, sizeof(path), "/mnt/disc/app/%s/app.pkg", id);
      size = svc_file_size(path);
      if (size > 0) {
        st->expected_size = (uint64_t)size;
        st->complete = st->complete && st->size >= st->expected_size;
      }
    }
  }
  snprintf(path, sizeof(path), PFSMNT "/%s-app0", id);
  st->mounted = has_entries(path);
  st->running = running_app() > 0;
}

// A PS4 title's name from its package's param.sfo; a PS5 title's from the
// param.json the system keeps.
static void game_name(const char *id, const char *dir, char *out, size_t n, int *app) {
  char path[SVC_PATH];

  out[0] = '\0';
  *app = 0;
  if (!strncmp(id, "CUSA", 4)) {
    struct pkg p;
    int fd;
    svc_path_join(path, sizeof(path), dir, "app.pkg");
    fd = open(path, O_RDONLY);
    if (fd >= 0 && pkg_open(&p, fd) == RIP_OK) {
      const struct pkg_entry *e = pkg_find(&p, PKG_ENTRY_PARAM_SFO);
      uint8_t *sfo;
      size_t len;
      if (e && pkg_read_entry(&p, e, &sfo, &len) == RIP_OK) {
        sfo_get_string(sfo, len, "TITLE", out, n);
        free(sfo);
      }
      pkg_close(&p);
    }
    if (fd >= 0) {
      close(fd);
    }
  } else {
    char *txt;
    snprintf(path, sizeof(path), "/user/appmeta/%s/param.json", id);
    txt = svc_read_file(path, NULL);
    if (txt) {
      cJSON *j = cJSON_Parse(txt);
      const cJSON *loc = cJSON_GetObjectItemCaseSensitive(j, "localizedParameters");
      const char *lang = jstr(loc, "defaultLanguage", "en-US");
      const cJSON *l = cJSON_GetObjectItemCaseSensitive(loc, lang);
      svc_str_copy(out, n, jstr(l, "titleName", ""));
      *app = jint(j, "applicationCategoryType", 0) != 0;  // 0 is a game
      cJSON_Delete(j);
      free(txt);
    }
  }
  if (!out[0]) {
    svc_str_copy(out, n, id);
  }
}

static int valid_id(const char *s) {
  int i;

  if (strlen(s) != 9 || (strncmp(s, "CUSA", 4) && strncmp(s, "PPSA", 4))) {
    return 0;
  }
  for (i = 4; i < 9; i++) {
    if (s[i] < '0' || s[i] > '9') {
      return 0;
    }
  }
  return 1;
}

int plat_game_list(struct plat_game *out, int max) {
  int n = 0;
  int r;

  for (r = 0; APP_ROOTS[r] && n < max; r++) {
    DIR *d = opendir(APP_ROOTS[r]);
    struct dirent *e;
    while (d && n < max && (e = readdir(d)) != NULL) {
      char dir[SVC_PATH];
      char pkg[SVC_PATH];
      int64_t size;
      int k;
      int dup = 0;
      if (!valid_id(e->d_name)) {
        continue;
      }
      for (k = 0; k < n; k++) {
        dup |= !strcmp(out[k].title_id, e->d_name);
      }
      svc_path_join(dir, sizeof(dir), APP_ROOTS[r], e->d_name);
      svc_path_join(pkg, sizeof(pkg), dir, "app.pkg");
      size = svc_file_size(pkg);
      if (dup || size <= 0) {
        continue;
      }
      memset(&out[n], 0, sizeof(out[n]));
      svc_str_copy(out[n].title_id, sizeof(out[n].title_id), e->d_name);
      out[n].ps5 = e->d_name[0] == 'P';
      out[n].size = (uint64_t)size;
      game_name(e->d_name, dir, out[n].name, sizeof(out[n].name), &out[n].app);
      n++;
    }
    if (d) {
      closedir(d);
    }
  }
  return n;
}

/* Copying */

struct dump {
  plat_progress_fn progress;
  void *ud;
  volatile int *cancel;
  uint64_t total;
  uint64_t done;
  unsigned selfs;
  unsigned failed;
  char *err;
  size_t en;
};

static int stopped(struct dump *d) {
  return d->cancel && *d->cancel;
}

static void tick(struct dump *d, const char *phase) {
  if (d->progress && d->progress(d->ud, phase, d->done, d->total)) {
    if (d->cancel) {
      *d->cancel = 1;
    }
  }
}

static uint64_t tree_size(const char *dir) {
  DIR *dd = opendir(dir);
  struct dirent *e;
  uint64_t total = 0;

  while (dd && (e = readdir(dd)) != NULL) {
    char path[SVC_PATH];
    struct stat st;
    if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) {
      continue;
    }
    svc_path_join(path, sizeof(path), dir, e->d_name);
    if (stat(path, &st) != 0) {
      continue;
    }
    total += S_ISDIR(st.st_mode) ? tree_size(path) : (uint64_t)st.st_size;
  }
  if (dd) {
    closedir(dd);
  }
  return total;
}

static int copy_plain(struct dump *d, int in, const char *to, uint64_t size) {
  uint8_t *buf = malloc(COPY_CHUNK);
  int out = open(to, O_WRONLY | O_CREAT | O_TRUNC, 0644);
  uint64_t left = size;
  int rc = RIP_OK;

  if (!buf || out < 0) {
    rc = RIP_ERR;
  }
  while (rc == RIP_OK && left && !stopped(d)) {
    long n = rip_read_full(in, buf, left < COPY_CHUNK ? (size_t)left : COPY_CHUNK);
    if (n <= 0 || rip_write_all(out, buf, (size_t)n) != 0) {
      rc = RIP_ERR;
      break;
    }
    left -= (uint64_t)n;
    d->done += (uint64_t)n;
    tick(d, "copying");
  }
  free(buf);
  if (out >= 0 && close(out) != 0) {
    rc = RIP_ERR;
  }
  return stopped(d) ? RIP_ECANCEL : rc;
}

// One file: a SELF comes out decrypted (a plain ELF), anything else as it is.
static int dump_file(struct dump *d, const char *from, const char *to, uint64_t size) {
  uint32_t magic = 0;
  int in = open(from, O_RDONLY);
  int rc;

  if (in < 0) {
    snprintf(d->err, d->en, "can't open %s: %s", from, strerror(errno));
    return RIP_ERR;
  }
  if (rip_read_full(in, &magic, sizeof(magic)) == (long)sizeof(magic) &&
      (magic == SELF_ORBIS_MAGIC || magic == SELF_PROSPERO_MAGIC)) {
    char *elf = NULL;
    uint64_t elf_size = 0;
    int res;
    lseek(in, 0, SEEK_SET);
    res = decrypt_self(in, &elf, &elf_size);
    close(in);
    d->selfs++;
    if (res != 0) {
      // Kept encrypted rather than lost: the dump says which ones in the log.
      d->failed++;
      RIP_WARN("couldn't decrypt %s (%d); copied as it is", from, res);
      in = open(from, O_RDONLY);
      if (in < 0) {
        return RIP_ERR;
      }
      rc = copy_plain(d, in, to, size);
      close(in);
      return rc;
    }
    {
      int out = open(to, O_WRONLY | O_CREAT | O_TRUNC, 0644);
      rc = out >= 0 && rip_write_all(out, elf, (size_t)elf_size) == 0 ? RIP_OK : RIP_ERR;
      if (out >= 0 && close(out) != 0) {
        rc = RIP_ERR;
      }
    }
    munmap(elf, elf_size);
    d->done += size;
    tick(d, "decrypting");
    return rc;
  }
  lseek(in, 0, SEEK_SET);
  rc = copy_plain(d, in, to, size);
  close(in);
  return rc;
}

static int dump_tree(struct dump *d, const char *from, const char *to) {
  DIR *dd = opendir(from);
  struct dirent *e;
  int rc = RIP_OK;

  if (!dd || rip_mkdir_p(to) != 0) {
    if (dd) {
      closedir(dd);
    }
    snprintf(d->err, d->en, "can't read %s", from);
    return RIP_ERR;
  }
  while (rc == RIP_OK && !stopped(d) && (e = readdir(dd)) != NULL) {
    char src[SVC_PATH];
    char dst[SVC_PATH];
    struct stat st;
    if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) {
      continue;
    }
    svc_path_join(src, sizeof(src), from, e->d_name);
    svc_path_join(dst, sizeof(dst), to, e->d_name);
    if (stat(src, &st) != 0) {
      continue;
    }
    rc = S_ISDIR(st.st_mode) ? dump_tree(d, src, dst) : dump_file(d, src, dst, (uint64_t)st.st_size);
  }
  closedir(dd);
  return stopped(d) ? RIP_ECANCEL : rc;
}

// sce_sys from the system's own copy of the game's metadata, for whatever
// the mounted files leave out.
static void copy_metadata(const char *id, const char *out_dir) {
  char dir[SVC_PATH];
  static const char *const user_files[] = {"param.sfo", "param.json", "icon0.png", "pic0.png",
                                           "pic1.png",  "snd0.at9",   NULL};
  static const char *const priv_files[] = {"nptitle.dat", "npbind.dat", NULL};
  char sce_sys[SVC_PATH];
  int i;

  svc_path_join(sce_sys, sizeof(sce_sys), out_dir, "sce_sys");
  rip_mkdir_p(sce_sys);
  // A PS4 package carries sce_sys in the clear: the real param.sfo, icons,
  // trophies (the system keeps only a param.json of its own for PS4 titles).
  if (app_dir(id, dir, sizeof(dir))) {
    char pkg_path[SVC_PATH];
    struct pkg p;
    int fd;
    svc_path_join(pkg_path, sizeof(pkg_path), dir, "app.pkg");
    fd = open(pkg_path, O_RDONLY);
    if (fd >= 0 && pkg_open(&p, fd) == RIP_OK) {
      int n = pkg_extract_named(&p, sce_sys);
      RIP_INFO("sce_sys: %d files from %s", n, pkg_path);
      pkg_close(&p);
    }
    if (fd >= 0) {
      close(fd);
    }
  }
  for (i = 0; user_files[i]; i++) {
    char from[SVC_PATH];
    char to[SVC_PATH];
    snprintf(from, sizeof(from), "/user/appmeta/%s/%s", id, user_files[i]);
    svc_path_join(to, sizeof(to), sce_sys, user_files[i]);
    if (!rip_file_exists(to) && rip_file_exists(from)) {
      svc_copy_file(from, to, NULL, NULL);
    }
  }
  for (i = 0; priv_files[i]; i++) {
    char from[SVC_PATH];
    char to[SVC_PATH];
    snprintf(from, sizeof(from), "/system_data/priv/appmeta/%s/%s", id, priv_files[i]);
    svc_path_join(to, sizeof(to), sce_sys, priv_files[i]);
    if (!rip_file_exists(to) && rip_file_exists(from)) {
      svc_copy_file(from, to, NULL, NULL);
    }
  }
}

// After a dump: the game, if it was only started to be read, closed again;
// RipperPS, if the start closed it, started again.
static void put_back(int launched, const char *closed) {
  char err[160];
  char none[32];
  uint64_t until;

  if (launched && p_kill_app && running_app() > 0) {
    int app = running_app();
    p_kill_app(app, -1, 0, 0);
    for (until = rip_now_ms() + CLOSE_WAIT_MS; running_app() == app && rip_now_ms() < until;) {
      usleep(250000);
    }
    sleep(1);
  }
  if (!strcmp(closed, RIPPERPS_TITLE) && launch(RIPPERPS_TITLE, none, sizeof(none), err, sizeof(err)) != RIP_OK) {
    RIP_WARN("starting RipperPS again: %s", err);
  }
}

int plat_game_dump(const char *title_id, const char *out_dir, plat_progress_fn progress, void *ud, volatile int *cancel,
                   char *err, size_t en) {
  struct plat_dump_status st;
  struct dump d;
  char id[16];
  char mount[SVC_PATH];
  char closed[32] = "";
  int launched = 0;
  int rc;
  int i;

  plat_game_status(title_id, &st);
  system_id(title_id, id, sizeof(id));
  if (st.installing) {
    snprintf(err, en, "%s is still installing (%.1f of %.1f GB): rip it once it's done", id, (double)st.copied / 1e9,
             (double)st.to_copy / 1e9);
    return RIP_ERR;
  }
  if (!st.installed) {
    snprintf(err, en, "%s isn't installed: install it from the disc first, then rip again", id);
    return RIP_ERR;
  }
  if (!st.complete) {
    snprintf(err, en, "%s is still installing (%.1f of %.1f GB): rip it once it's done", id, (double)st.size / 1e9,
             (double)st.expected_size / 1e9);
    return RIP_ERR;
  }
  if (st.partial) {
    snprintf(err, en,
             "parts of %s were never installed on this console (other languages, most likely), and reading them "
             "would hang it: RipperPS can't dump this game yet",
             id);
    return RIP_ERR;
  }
  snprintf(mount, sizeof(mount), PFSMNT "/%s-app0", id);
  if (!st.mounted) {
    if (progress) {
      progress(ud, "starting the game", 0, 0);
    }
    if (launch(id, closed, sizeof(closed), err, en) != RIP_OK) {
      put_back(0, closed);
      return RIP_ERR;
    }
    launched = 1;
    for (i = 0; i < MOUNT_WAIT_SEC && !has_entries(mount); i++) {
      if (cancel && *cancel) {
        put_back(launched, closed);
        return RIP_ECANCEL;
      }
      sleep(1);
    }
    if (!has_entries(mount)) {
      snprintf(err, en, "the game started, but its files never appeared under %s", PFSMNT);
      put_back(launched, closed);
      return RIP_ERR;
    }
    sleep(2);  // let the mount settle before reading it whole
  }

  memset(&d, 0, sizeof(d));
  d.progress = progress;
  d.ud = ud;
  d.cancel = cancel;
  d.err = err;
  d.en = en;
  d.total = tree_size(mount);
  RIP_INFO("dumping %s: %llu MiB from %s to %s", id, (unsigned long long)(d.total >> 20), mount, out_dir);
  rc = dump_tree(&d, mount, out_dir);
  if (rc == RIP_OK) {
    copy_metadata(id, out_dir);
    RIP_INFO("dumped %s: %u executables, %u left encrypted", id, d.selfs, d.failed);
    if (d.failed) {
      snprintf(err, en, "%u of %u executables couldn't be decrypted (this firmware may not be supported)", d.failed,
               d.selfs);
      rc = RIP_ERR;
    }
  }
  put_back(launched, closed);
  return rc;
}
