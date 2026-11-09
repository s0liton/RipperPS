#include "ripper/cue.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "ripper/common.h"
#include "ripper/log.h"

const char *cue_mode_name(enum cd_track_mode mode) {
  switch (mode) {
  case CD_MODE_AUDIO:
    return "AUDIO";
  case CD_MODE1:
    return "MODE1/2352";
  case CD_MODE2:
  case CD_MODE2_FORMLESS:
    return "MODE2/2352";
  }
  return "MODE1/2352";
}

/* Writing */

struct out_buf {
  char *p;
  size_t len;
  size_t used;
  int full;
};

static void put(struct out_buf *o, const char *fmt, ...) __attribute__((format(printf, 2, 3)));

static void put(struct out_buf *o, const char *fmt, ...) {
  va_list ap;
  int n;

  if (o->full) {
    return;
  }
  va_start(ap, fmt);
  n = vsnprintf(o->p + o->used, o->len - o->used, fmt, ap);
  va_end(ap);
  if (n < 0 || (size_t)n >= o->len - o->used) {
    o->full = 1;
    return;
  }
  o->used += (size_t)n;
}

static void put_msf(struct out_buf *o, const char *label, uint32_t frames) {
  char msf[16];

  cd_msf_str(frames, msf, sizeof(msf));
  put(o, "%s%s\r\n", label, msf);
}

// Names are written between quotes; a quote in one would end it early.
static void put_file(struct out_buf *o, const char *name) {
  char clean[512];
  size_t i;

  snprintf(clean, sizeof(clean), "%s", name);
  for (i = 0; clean[i]; i++) {
    if (clean[i] == '"') {
      clean[i] = '\'';
    }
  }
  put(o, "FILE \"%s\" BINARY\r\n", clean);
}

size_t cue_write(const struct cd_toc *toc, const char *const *names, int per_track, char *out, size_t len) {
  struct out_buf o = {out, len, 0, 0};
  uint32_t first = toc->count ? toc->tracks[0].start : 0;
  unsigned session = 0;
  unsigned i;

  if (len == 0) {
    return 0;
  }
  out[0] = '\0';
  if (!per_track) {
    put_file(&o, names[0]);
  }
  for (i = 0; i < toc->count; i++) {
    const struct cd_track *tr = &toc->tracks[i];
    uint32_t pregap = i == 0 ? 0 : tr->pregap;

    if (toc->sessions > 1 && tr->session != session) {
      session = tr->session;
      put(&o, "REM SESSION %02u\r\n", session);
    }
    if (per_track) {
      put_file(&o, names[i]);
    }
    put(&o, "  TRACK %02u %s\r\n", tr->number, cue_mode_name(tr->mode));
    if (cd_track_is_audio(tr) && (tr->control & (CD_CTRL_COPY | CD_CTRL_PREEMPHASIS | CD_CTRL_4CH))) {
      put(&o, "    FLAGS%s%s%s\r\n", (tr->control & CD_CTRL_COPY) ? " DCP" : "",
          (tr->control & CD_CTRL_4CH) ? " 4CH" : "", (tr->control & CD_CTRL_PREEMPHASIS) ? " PRE" : "");
    }
    if (per_track) {
      if (pregap) {
        put_msf(&o, "    INDEX 00 ", 0);
      }
      put_msf(&o, "    INDEX 01 ", pregap);
    } else {
      if (pregap) {
        put_msf(&o, "    INDEX 00 ", tr->start - pregap - first);
      }
      put_msf(&o, "    INDEX 01 ", tr->start - first);
    }
  }
  return o.full ? 0 : o.used;
}

/* Parsing */

struct parsed_track {
  uint8_t number;
  enum cd_track_mode mode;
  uint8_t control;
  unsigned file;
  int64_t index0;  // frames into the file, -1 if none
  int64_t index1;
  uint32_t silence;
  uint8_t session;
};

static const char *skip_ws(const char *p, const char *end) {
  while (p < end && (*p == ' ' || *p == '\t')) {
    p++;
  }
  return p;
}

// One word, or a quoted string, into out.
static const char *word(const char *p, const char *end, char *out, size_t len) {
  size_t n = 0;

  p = skip_ws(p, end);
  if (p < end && *p == '"') {
    p++;
    while (p < end && *p != '"') {
      if (n + 1 < len) {
        out[n++] = *p;
      }
      p++;
    }
    if (p < end) {
      p++;
    }
  } else {
    while (p < end && *p != ' ' && *p != '\t') {
      if (n + 1 < len) {
        out[n++] = *p;
      }
      p++;
    }
  }
  out[n] = '\0';
  return p;
}

static int parse_msf(const char *s, uint32_t *frames) {
  unsigned m, sec, f;

  if (sscanf(s, "%u:%u:%u", &m, &sec, &f) != 3 || sec > 59 || f > 74) {
    return RIP_ERR;
  }
  *frames = (m * 60 + sec) * CD_FRAMES_PER_SEC + f;
  return RIP_OK;
}

static int parse_mode(const char *s, enum cd_track_mode *mode) {
  if (strcmp(s, "AUDIO") == 0) {
    *mode = CD_MODE_AUDIO;
  } else if (strcmp(s, "MODE1/2352") == 0) {
    *mode = CD_MODE1;
  } else if (strcmp(s, "MODE2/2352") == 0) {
    *mode = CD_MODE2;
  } else {
    return RIP_ERR;
  }
  return RIP_OK;
}

static void resolve(const char *dir, const char *name, char *out, size_t len) {
  if (name[0] == '/' || !dir || !dir[0]) {
    snprintf(out, len, "%s", name);
  } else {
    snprintf(out, len, "%s/%s", dir, name);
  }
}

int cue_parse(const char *text, size_t len, const char *dir, struct cue_sheet *out) {
  struct parsed_track *pt = calloc(CD_MAX_TRACKS, sizeof(*pt));
  uint64_t frames[CD_MAX_TRACKS];
  const char *p = text;
  const char *end = text + len;
  unsigned n = 0;
  unsigned line = 0;
  uint8_t session = 1;
  int64_t base;
  unsigned i;
  int rc = RIP_ERR;

  memset(out, 0, sizeof(*out));
  if (!pt) {
    return RIP_ENOMEM;
  }
  while (p < end) {
    const char *eol = memchr(p, '\n', (size_t)(end - p));
    const char *le = eol ? eol : end;
    char cmd[32];
    char a[512];
    char b[64];
    const char *q;

    line++;
    while (le > p && (le[-1] == '\r' || le[-1] == ' ')) {
      le--;
    }
    q = word(p, le, cmd, sizeof(cmd));
    p = eol ? eol + 1 : end;
    if (strcmp(cmd, "REM") == 0) {
      // Redump marks a second session: "REM SESSION 02".
      q = word(q, le, a, sizeof(a));
      if (strcmp(a, "SESSION") == 0) {
        word(q, le, b, sizeof(b));
        if (atoi(b) > 0 && atoi(b) < 100) {
          session = (uint8_t)atoi(b);
        }
      }
      continue;
    }
    if (!cmd[0]) {
      continue;
    }
    if (strcmp(cmd, "FILE") == 0) {
      struct stat st;
      q = word(q, le, a, sizeof(a));
      word(q, le, b, sizeof(b));
      if (out->file_count >= CD_MAX_TRACKS || (strcmp(b, "BINARY") != 0 && b[0])) {
        RIP_ERROR("cue line %u: only BINARY files are supported", line);
        goto done;
      }
      resolve(dir, a, out->files[out->file_count], sizeof(out->files[0]));
      if (stat(out->files[out->file_count], &st) != 0) {
        RIP_ERROR("cue line %u: %s is missing", line, out->files[out->file_count]);
        goto done;
      }
      frames[out->file_count] = (uint64_t)st.st_size / CD_FRAME;
      out->file_count++;
    } else if (strcmp(cmd, "TRACK") == 0) {
      struct parsed_track *t;
      q = word(q, le, a, sizeof(a));
      word(q, le, b, sizeof(b));
      if (!out->file_count || n >= CD_MAX_TRACKS) {
        RIP_ERROR("cue line %u: TRACK before FILE", line);
        goto done;
      }
      t = &pt[n++];
      t->number = (uint8_t)atoi(a);
      t->session = session;
      t->file = out->file_count - 1;
      t->index0 = -1;
      t->index1 = -1;
      if (parse_mode(b, &t->mode) != RIP_OK) {
        RIP_ERROR("cue line %u: track mode %s is not supported (raw 2352-byte tracks only)", line, b);
        goto done;
      }
      t->control = t->mode == CD_MODE_AUDIO ? 0 : CD_CTRL_DATA;
    } else if (strcmp(cmd, "INDEX") == 0 || strcmp(cmd, "PREGAP") == 0) {
      uint32_t f;
      int is_index = cmd[0] == 'I';
      if (!n) {
        RIP_ERROR("cue line %u: %s outside a track", line, cmd);
        goto done;
      }
      q = word(q, le, a, sizeof(a));
      if (is_index) {
        word(q, le, b, sizeof(b));
      }
      if (parse_msf(is_index ? b : a, &f) != RIP_OK) {
        RIP_ERROR("cue line %u: bad time", line);
        goto done;
      }
      if (!is_index) {
        pt[n - 1].silence = f;
      } else if (atoi(a) == 0) {
        pt[n - 1].index0 = f;
      } else if (atoi(a) == 1) {
        pt[n - 1].index1 = f;
      }
    } else if (strcmp(cmd, "FLAGS") == 0 && n) {
      while (q < le) {
        q = word(q, le, a, sizeof(a));
        if (strcmp(a, "DCP") == 0) {
          pt[n - 1].control |= CD_CTRL_COPY;
        } else if (strcmp(a, "PRE") == 0) {
          pt[n - 1].control |= CD_CTRL_PREEMPHASIS;
        } else if (strcmp(a, "4CH") == 0) {
          pt[n - 1].control |= CD_CTRL_4CH;
        }
      }
    } else if (strcmp(cmd, "CATALOG") == 0) {
      word(q, le, out->catalog, sizeof(out->catalog));
    }
  }
  if (n == 0) {
    RIP_ERROR("cue sheet has no tracks");
    goto done;
  }

  // Lay the files end to end; PREGAP silence pushes everything after it.
  {
    int64_t file_base[CD_MAX_TRACKS];
    int64_t shift = 0;
    unsigned f;

    base = 0;
    for (f = 0; f < out->file_count; f++) {
      unsigned k;
      file_base[f] = base;
      for (k = 0; k < n; k++) {
        if (pt[k].file == f) {
          struct cue_track *t = &out->tracks[k];
          int64_t first;
          if (pt[k].index1 < 0) {
            RIP_ERROR("track %u has no INDEX 01", pt[k].number);
            goto done;
          }
          // Between sessions: lead-out, lead-in and pregap, in no file.
          if (k > 0 && pt[k].session != pt[k - 1].session) {
            shift += 11400;
          }
          shift += pt[k].silence;
          first = pt[k].index0 >= 0 ? pt[k].index0 : pt[k].index1;
          t->number = pt[k].number;
          t->mode = pt[k].mode;
          t->control = pt[k].control;
          t->session = pt[k].session;
          t->file = f;
          t->file_offset = (uint64_t)first * CD_FRAME;
          t->pregap = (uint32_t)(pt[k].index1 - first);
          t->silence = pt[k].silence;
          t->start = (uint32_t)(file_base[f] + shift + pt[k].index1);
        }
      }
      base = file_base[f] + shift + (int64_t)frames[f];
      shift = 0;
    }
  }
  out->count = n;
  out->leadout = (uint32_t)base;
  // Track 1's INDEX 01 is LBA 0 on a disc; anything before it can't be read.
  {
    uint32_t zero = out->tracks[0].start;
    for (i = 0; i < n; i++) {
      out->tracks[i].start -= zero;
    }
    out->leadout -= zero;
    if (out->tracks[0].pregap) {
      out->tracks[0].file_offset += (uint64_t)out->tracks[0].pregap * CD_FRAME;
      out->tracks[0].pregap = 0;
    }
    out->tracks[0].silence = 0;
  }
  for (i = 0; i < n; i++) {
    const struct cue_track *next = i + 1 < n ? &out->tracks[i + 1] : NULL;
    uint32_t end_lba = !next                                     ? out->leadout
                       : next->session != out->tracks[i].session ? next->start - next->pregap - next->silence - 11400
                                                                 : next->start - next->pregap - next->silence;
    out->tracks[i].length = end_lba > out->tracks[i].start ? end_lba - out->tracks[i].start : 0;
  }
  rc = RIP_OK;

done:
  free(pt);
  return rc;
}

int cue_load(const char *path, struct cue_sheet *out) {
  char dir[512];
  char *slash;
  FILE *f = fopen(path, "rb");
  char *text;
  long n;
  int rc;

  if (!f) {
    RIP_ERROR("cannot open %s", path);
    return RIP_ENOENT;
  }
  fseek(f, 0, SEEK_END);
  n = ftell(f);
  fseek(f, 0, SEEK_SET);
  if (n <= 0 || n > 256 * 1024) {
    fclose(f);
    return RIP_ERR;
  }
  text = malloc((size_t)n);
  if (!text || fread(text, 1, (size_t)n, f) != (size_t)n) {
    free(text);
    fclose(f);
    return RIP_ERR;
  }
  fclose(f);
  snprintf(dir, sizeof(dir), "%s", path);
  slash = strrchr(dir, '/');
  if (slash) {
    *slash = '\0';
  } else {
    snprintf(dir, sizeof(dir), ".");
  }
  rc = cue_parse(text, (size_t)n, dir, out);
  free(text);
  return rc;
}
