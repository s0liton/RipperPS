#include "details.h"

#include <ctype.h>
#include <dirent.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "gamedb.h"
#include "http.h"
#include "musicbrainz.h"
#include "ripper/common.h"
#include "ripper/log.h"
#include "ripper/pkg.h"
#include "settings.h"
#include "svc.h"

#define MAX_COVER (4u << 20)

static void take_cover(struct disc_details *d, uint8_t *bytes, size_t len) {
  free(d->cover);
  d->cover = bytes;
  d->cover_len = len;
  if (len >= 8 && !memcmp(bytes, "\x89PNG", 4)) {
    svc_str_copy(d->cover_type, sizeof(d->cover_type), "image/png");
  } else {
    svc_str_copy(d->cover_type, sizeof(d->cover_type), "image/jpeg");
  }
}

static int fetch_cover(struct disc_details *d, const char *url) {
  struct http_req req;
  struct http_resp resp;
  int ok;

  memset(&req, 0, sizeof(req));
  memset(&resp, 0, sizeof(resp));
  req.method = "GET";
  req.url = url;
  req.tls_verify = 1;
  req.timeout_s = 20;
  if (http_do(&req, &resp) != RIP_OK) {
    RIP_INFO("cover: %s didn't answer (%s)", url, resp.error);
    http_resp_free(&resp);
    return 0;
  }
  ok = resp.status == 200 && resp.len > 64 && resp.len < MAX_COVER &&
       ((uint8_t)resp.body[0] == 0xFF || !memcmp(resp.body, "\x89PNG", 4));
  if (ok) {
    uint8_t *bytes = malloc(resp.len);
    if (bytes) {
      memcpy(bytes, resp.body, resp.len);
      take_cover(d, bytes, resp.len);
    }
    ok = bytes != NULL;
  }
  http_resp_free(&resp);
  return ok;
}

// "Syphon Filter (USA) (Disc 1)": the region, and the disc if there is one.
static void redump_tags(const char *name, char *out, size_t n) {
  const char *p = strstr(name, " (");
  out[0] = '\0';
  while (p) {
    const char *end = strchr(p + 2, ')');
    size_t len = strlen(out);
    if (!end) {
      break;
    }
    // Languages ("En,Fr,De") and revisions say little on a card.
    if (strncmp(p + 2, "En,", 3) != 0 && strncmp(p + 2, "Rev ", 4) != 0 && strncmp(p + 2, "v", 1) != 0) {
      snprintf(out + len, n - len, "%s%.*s", len ? " \xC2\xB7 " : "", (int)(end - p - 2), p + 2);
    }
    p = strstr(end, " (");
  }
}

static void audio_cd(const struct rip_disc *disc, struct disc_details *d) {
  char err[256];
  struct rip_audio_meta *m;

  if (!disc->musicbrainz_id[0] || !(m = calloc(1, sizeof(*m)))) {
    return;
  }
  if (mb_lookup(disc->musicbrainz_id, m, err, sizeof(err)) != RIP_OK || !m->album[0]) {
    RIP_INFO("details: %s", err[0] ? err : "MusicBrainz has nothing for this disc");
    free(m);
    return;
  }
  d->audio = m;
  svc_str_copy(d->title, sizeof(d->title), m->album);
  svc_str_copy(d->name, sizeof(d->name), m->album);
  svc_str_copy(d->artist, sizeof(d->artist), m->artist);
  snprintf(d->subtitle, sizeof(d->subtitle), "%.200s%s%.4s", m->artist, m->date[0] ? " \xC2\xB7 " : "", m->date);
  if (m->has_cover && m->release_id[0]) {
    char url[160];
    snprintf(url, sizeof(url), "https://coverartarchive.org/release/%s/front-500", m->release_id);
    fetch_cover(d, url);
  }
}

// PS5: app/<title ID>/app_sc.pkg carries icon0.png in the clear.
static void ps5_icon(const char *root, const char *title_id, struct disc_details *d) {
  char path[600];
  struct pkg pkg;
  int fd;
  unsigned i;

  if (!root || !title_id[0]) {
    return;
  }
  snprintf(path, sizeof(path), "%.400s/app/%.12s/app_sc.pkg", root, title_id);
  fd = open(path, O_RDONLY);
  if (fd < 0) {
    return;
  }
  if (pkg_open(&pkg, fd) == RIP_OK) {
    for (i = 0; i < pkg.count; i++) {
      uint8_t *data = NULL;
      size_t len = 0;
      if (!strcmp(pkg.entries[i].name, "icon0.png") && !pkg_entry_encrypted(&pkg.entries[i]) &&
          pkg_read_entry(&pkg, &pkg.entries[i], &data, &len) == RIP_OK) {
        take_cover(d, data, len);
        break;
      }
    }
    pkg_close(&pkg);
  }
  close(fd);
}

// A Blu-ray movie's own title and thumbnail: BDMV/META/DL/bdmt_eng.xml.
static void bd_meta(details_read_fn read, void *ud, struct disc_details *d) {
  uint8_t *xml = NULL;
  size_t len = 0;
  char *text;
  char *p;
  char best[160] = "";

  if (!read || read(ud, "BDMV/META/DL/bdmt_eng.xml", &xml, &len) != 0) {
    return;
  }
  text = malloc(len + 1);
  if (!text) {
    free(xml);
    return;
  }
  memcpy(text, xml, len);
  text[len] = '\0';
  free(xml);
  if ((p = strstr(text, "<di:name>")) != NULL) {
    char *end = strstr(p, "</di:name>");
    if (end) {
      *end = '\0';
      svc_str_copy(d->title, sizeof(d->title), p + 9);
      svc_str_copy(d->name, sizeof(d->name), p + 9);
      *end = '<';
    }
  }
  // The thumbnails: the last listed is usually the largest.
  for (p = text; (p = strstr(p, "<di:thumbnail href=\"")) != NULL; p++) {
    char *q = p + 20;
    char *end = strchr(q, '"');
    if (end && end - q < 120) {
      snprintf(best, sizeof(best), "BDMV/META/DL/%.*s", (int)(end - q), q);
    }
  }
  free(text);
  if (best[0] && read(ud, best, &xml, &len) == 0) {
    take_cover(d, xml, len);
  }
}

static void url_escape(const char *in, char *out, size_t n) {
  static const char hex[] = "0123456789ABCDEF";
  size_t o = 0;

  for (; *in && o + 4 < n; in++) {
    unsigned char c = (unsigned char)*in;
    if (isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') {
      out[o++] = (char)c;
    } else {
      out[o++] = '%';
      out[o++] = hex[c >> 4];
      out[o++] = hex[c & 15];
    }
  }
  out[o] = '\0';
}

// Letters and digits only, lower case: "The Matrix" and "THE_MATRIX" alike.
static void plain_key(const char *in, char *out, size_t n) {
  size_t o = 0;

  for (; *in && o + 1 < n; in++) {
    if (isalnum((unsigned char)*in)) {
      out[o++] = (char)tolower((unsigned char)*in);
    }
  }
  out[o] = '\0';
}

// The first year in text ("2004 DreamWorks Animation film"): four digits on
// their own, starting with 1 or 2. NULL if there's none.
static const char *first_year(const char *text) {
  const char *p;

  for (p = text; *p; p++) {
    size_t n = 0;
    if (p > text && isdigit((unsigned char)p[-1])) {
      continue;
    }
    while (isdigit((unsigned char)p[n])) {
      n++;
    }
    if (n == 4 && (p[0] == '1' || p[0] == '2')) {
      return p;
    }
  }
  return NULL;
}

// The movie on English Wikipedia, for when there's no TMDB key: no account
// needed. Its film articles say "1999 film by ..." in their description and
// lead with the poster. Only an article whose title is the name we have
// (less a " (1999 film)") is taken: a volume label that's not quite a title
// finds nothing rather than the wrong movie.
static void wikipedia(struct disc_details *d) {
  char q[600];
  char url[1200];
  char want[256];
  struct http_req req;
  struct http_resp resp;
  cJSON *j;
  const cJSON *pages;
  const cJSON *p;
  const cJSON *best = NULL;
  int best_index = 1 << 30;

  {
    char search[300];
    snprintf(search, sizeof(search), "%.250s film", d->title);
    url_escape(search, q, sizeof(q));
  }
  plain_key(d->title, want, sizeof(want));
  if (!want[0]) {
    return;
  }
  snprintf(url, sizeof(url),
           "https://en.wikipedia.org/w/api.php?action=query&format=json&formatversion=2&generator=search"
           "&gsrsearch=%s&gsrlimit=8&prop=pageimages%%7Cdescription&piprop=thumbnail&pithumbsize=500&pilicense=any",
           q);
  memset(&req, 0, sizeof(req));
  memset(&resp, 0, sizeof(resp));
  req.method = "GET";
  req.url = url;
  req.tls_verify = 1;
  req.timeout_s = 20;
  if (http_do(&req, &resp) != RIP_OK || resp.status != 200) {
    RIP_INFO("details: Wikipedia didn't answer (%ld %s)", resp.status, resp.error);
    http_resp_free(&resp);
    return;
  }
  j = http_json(&resp);
  http_resp_free(&resp);
  pages = cJSON_GetObjectItemCaseSensitive(cJSON_GetObjectItemCaseSensitive(j, "query"), "pages");
  cJSON_ArrayForEach(p, pages) {
    char title[256];
    char key[256];
    char *paren;
    const char *desc = jstr(p, "description", "");
    if (!strstr(desc, "film") || jint(p, "index", 1 << 30) >= best_index) {
      continue;
    }
    svc_str_copy(title, sizeof(title), jstr(p, "title", ""));
    paren = strstr(title, " (");
    if (paren) {
      *paren = '\0';
    }
    plain_key(title, key, sizeof(key));
    if (!strcmp(key, want)) {
      best = p;
      best_index = jint(p, "index", 1 << 30);
    }
  }
  if (best) {
    const char *desc = jstr(best, "description", "");
    const char *thumb = jstr(cJSON_GetObjectItemCaseSensitive(best, "thumbnail"), "source", "");
    const char *y = first_year(desc);
    char *paren;
    svc_str_copy(d->title, sizeof(d->title), jstr(best, "title", ""));
    paren = strstr(d->title, " (");
    if (paren) {
      *paren = '\0';
    }
    if (y) {
      snprintf(d->subtitle, sizeof(d->subtitle), "%.4s", y);
      snprintf(d->name, sizeof(d->name), "%.240s (%.4s)", d->title, y);
    } else {
      svc_str_copy(d->name, sizeof(d->name), d->title);
    }
    if (thumb[0] && !d->cover) {
      fetch_cover(d, thumb);
    }
  } else {
    RIP_INFO("details: Wikipedia has no film called '%s'", d->title);
  }
  cJSON_Delete(j);
}

// A movie on The Movie Database, by the name it has so far (the disc's own
// title, or its volume label tidied): the first match's title, year and
// poster. With a key in Settings (a v3 API key, or a v4 read token); without
// one, Wikipedia.
static void movie(struct disc_details *d) {
  char key[300];
  char q[600];
  char url[1024];
  struct http_req req;
  struct http_resp resp;
  cJSON *j;
  const cJSON *hit;

  settings_tmdb_key(key, sizeof(key));
  if (!d->title[0]) {
    return;
  }
  if (!key[0]) {
    wikipedia(d);
    return;
  }
  url_escape(d->title, q, sizeof(q));
  memset(&req, 0, sizeof(req));
  memset(&resp, 0, sizeof(resp));
  req.method = "GET";
  req.tls_verify = 1;
  req.timeout_s = 20;
  if (strlen(key) > 40) {
    req.bearer = key;  // a v4 read token
    snprintf(url, sizeof(url), "https://api.themoviedb.org/3/search/movie?query=%s", q);
  } else {
    snprintf(url, sizeof(url), "https://api.themoviedb.org/3/search/movie?query=%s&api_key=%s", q, key);
  }
  req.url = url;
  if (http_do(&req, &resp) != RIP_OK || resp.status != 200) {
    RIP_INFO("details: TMDB didn't answer (%ld %s)", resp.status, resp.error);
    http_resp_free(&resp);
    return;
  }
  j = http_json(&resp);
  http_resp_free(&resp);
  hit = cJSON_GetArrayItem(cJSON_GetObjectItemCaseSensitive(j, "results"), 0);
  if (hit && jstr(hit, "title", "")[0]) {
    const char *date = jstr(hit, "release_date", "");
    const char *poster = jstr(hit, "poster_path", "");
    svc_str_copy(d->title, sizeof(d->title), jstr(hit, "title", ""));
    if (strlen(date) >= 4) {
      snprintf(d->subtitle, sizeof(d->subtitle), "%.4s", date);
      snprintf(d->name, sizeof(d->name), "%.240s (%.4s)", d->title, date);
    } else {
      svc_str_copy(d->name, sizeof(d->name), d->title);
    }
    if (poster[0] == '/' && !d->cover) {
      snprintf(url, sizeof(url), "https://image.tmdb.org/t/p/w500%s", poster);
      fetch_cover(d, url);
    }
  } else {
    RIP_INFO("details: TMDB knows no movie called '%s'", d->title);
  }
  cJSON_Delete(j);
}

void details_lookup(const char *system, const struct rip_disc *disc, const char *mount_root, details_read_fn read,
                    void *ud, const char *hint, struct disc_details *d) {
  char redump[256];

  memset(d, 0, sizeof(*d));
  system = system ? system : "";
  if (disc->kind == RIP_DISC_CD_AUDIO) {
    audio_cd(disc, d);
  } else if (disc->title_id[0] && gamedb_lookup(system, disc->title_id, redump, sizeof(redump))) {
    char tags[160];
    gamedb_plain_title(redump, d->title, sizeof(d->title));
    redump_tags(redump, tags, sizeof(tags));
    snprintf(d->subtitle, sizeof(d->subtitle), "%s%s%s", disc->title_id, tags[0] ? " \xC2\xB7 " : "", tags);
    svc_str_copy(d->name, sizeof(d->name), redump);
  }
  if (!d->title[0] && disc->title[0]) {  // the disc's own (PARAM.SFO, param.json)
    svc_str_copy(d->title, sizeof(d->title), disc->title);
    svc_str_copy(d->subtitle, sizeof(d->subtitle), disc->title_id);
  }
  // Covers
  if (!strcmp(system, "ps1") || !strcmp(system, "ps2")) {
    char url[200];
    snprintf(url, sizeof(url), "https://raw.githubusercontent.com/xlenore/%s-covers/main/covers/default/%s.jpg",
             !strcmp(system, "ps1") ? "psx" : "ps2", disc->title_id);
    if (disc->title_id[0]) {
      fetch_cover(d, url);
    }
  } else if (!strcmp(system, "ps3") && read) {
    uint8_t *png = NULL;
    size_t len = 0;
    if (read(ud, "PS3_GAME/ICON0.PNG", &png, &len) == 0) {
      take_cover(d, png, len);
    }
  } else if (!strcmp(system, "ps5")) {
    ps5_icon(mount_root, disc->title_id, d);
  } else if (disc->kind == RIP_DISC_BD_VIDEO) {
    bd_meta(read, ud, d);
  }
  if (!d->title[0] && disc->volume_id[0]) {
    svc_pretty_label(disc->volume_id, d->title, sizeof(d->title));
  }
  if (disc->kind == RIP_DISC_DVD_VIDEO || disc->kind == RIP_DISC_BD_VIDEO) {
    if (hint && hint[0]) {
      svc_str_copy(d->title, sizeof(d->title), hint);
      svc_str_copy(d->name, sizeof(d->name), hint);
    }
    movie(d);
  }
  RIP_INFO("details: '%s' (%s), cover %zu bytes", d->title, d->subtitle, d->cover_len);
}

void details_free(struct disc_details *d) {
  free(d->cover);
  free(d->audio);
  memset(d, 0, sizeof(*d));
}
