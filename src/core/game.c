#include "ripper/game.h"

#include <dirent.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "ripper/common.h"
#include "ripper/log.h"

struct walk {
  const struct game_finish *opts;
  game_progress_fn progress;
  void *ud;
  unsigned total;
  unsigned done;
  int stop;
};

// Counts the ELFs (count) or readies each one.
static int visit(struct walk *w, const char *dir, int count) {
  DIR *d = opendir(dir);
  struct dirent *e;
  int rc = RIP_OK;

  if (!d) {
    return RIP_ERR;
  }
  while (rc == RIP_OK && !w->stop && (e = readdir(d)) != NULL) {
    char path[1024];
    struct stat st;
    if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) {
      continue;
    }
    snprintf(path, sizeof(path), "%s/%s", dir, e->d_name);
    if (stat(path, &st) != 0) {
      continue;
    }
    if (S_ISDIR(st.st_mode)) {
      rc = visit(w, path, count);
      continue;
    }
    if (!S_ISREG(st.st_mode)) {
      continue;
    }
    if (w->opts->fself && sce_is_self(path)) {
      // Already fake-signed (an earlier dump): made the asked console's.
      if (count) {
        w->total++;
      } else if (sce_retarget_fself(path, w->opts->target) < 0) {
        RIP_WARN("%s is a SELF RipperPS didn't make, left as it is", path);
      }
      if (!count) {
        w->done++;
        if (w->progress && w->progress(w->ud, w->done, w->total)) {
          w->stop = 1;
        }
      }
      continue;
    }
    if (!sce_is_elf(path)) {
      continue;
    }
    if (count) {
      w->total++;
      continue;
    }
    // Backport first: the FSELF's digests cover the patched bytes.
    if ((w->opts->backport_ps4 || w->opts->backport_ps5) &&
        sce_backport_elf(path, w->opts->backport_ps4, w->opts->backport_ps5) < 0) {
      RIP_WARN("backport failed for %s", path);
    }
    if (w->opts->fself) {
      char tmp[1100];
      snprintf(tmp, sizeof(tmp), "%s.fself", path);
      if (sce_write_fself(path, tmp, w->opts->target) != RIP_OK || rename(tmp, path) != 0) {
        unlink(tmp);
        RIP_ERROR("couldn't fake-sign %s", path);
        rc = RIP_ERR;
        break;
      }
    }
    w->done++;
    if (w->progress && w->progress(w->ud, w->done, w->total)) {
      w->stop = 1;
    }
  }
  closedir(d);
  return w->stop ? RIP_ECANCEL : rc;
}

int game_finish_folder(const char *dir, const struct game_finish *opts, game_progress_fn progress, void *ud,
                       unsigned *elfs) {
  struct walk w = {opts, progress, ud, 0, 0, 0};
  char path[1024];
  int rc;

  if (elfs) {
    *elfs = 0;
  }
  if (!opts->fself && !opts->backport_ps4 && !opts->backport_ps5) {
    return RIP_OK;
  }
  rc = visit(&w, dir, 1);
  if (rc == RIP_OK) {
    rc = visit(&w, dir, 0);
  }
  if (rc != RIP_OK) {
    return rc;
  }
  // The firmware the game says it needs, in its metadata too.
  if (opts->backport_ps4) {
    snprintf(path, sizeof(path), "%s/sce_sys/param.sfo", dir);
    if (access(path, F_OK) == 0 && sce_backport_sfo(path, opts->backport_ps4) < 0) {
      RIP_WARN("couldn't backport %s", path);
    }
  }
  if (opts->backport_ps5) {
    snprintf(path, sizeof(path), "%s/sce_sys/param.json", dir);
    if (access(path, F_OK) == 0 && sce_backport_json(path, opts->backport_ps5) < 0) {
      RIP_WARN("couldn't backport %s", path);
    }
  }
  if (elfs) {
    *elfs = w.done;
  }
  RIP_INFO("game folder readied: %u ELFs%s%s", w.done, opts->fself ? ", fake-signed" : "",
           opts->backport_ps4 || opts->backport_ps5 ? ", backported" : "");
  return RIP_OK;
}
