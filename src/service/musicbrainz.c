#include "musicbrainz.h"

#include <stdio.h>
#include <string.h>

#include "http.h"
#include "ripper/common.h"
#include "ripper/log.h"
#include "svc.h"

// An artist credit as MusicBrainz writes it: names joined by their phrases.
static void credit(const cJSON *ac, char *out, size_t n) {
  const cJSON *c;
  size_t at = 0;

  out[0] = '\0';
  cJSON_ArrayForEach(c, ac) {
    int w = snprintf(out + at, n > at ? n - at : 0, "%s%s", jstr(c, "name", ""), jstr(c, "joinphrase", ""));
    if (w > 0) {
      at += (size_t)w;
    }
  }
}

int mb_lookup(const char *discid, struct rip_audio_meta *meta, char *err, size_t en) {
  struct http_req req;
  struct http_resp resp;
  const char *ua[] = {"User-Agent: ripper/" RIPPER_VERSION " ( PS5 disc ripper )", NULL};
  const cJSON *release;
  const cJSON *medium;
  const cJSON *m;
  cJSON *j;
  char url[256];
  int found = 0;

  memset(meta, 0, sizeof(*meta));
  if (!discid || !discid[0]) {
    snprintf(err, en, "no disc ID");
    return RIP_ERR;
  }
  snprintf(url, sizeof(url), "https://musicbrainz.org/ws/2/discid/%s?inc=recordings+artist-credits&fmt=json", discid);
  memset(&req, 0, sizeof(req));
  req.method = "GET";
  req.url = url;
  req.headers = ua;
  req.tls_verify = 1;
  req.timeout_s = 20;
  if (http_do(&req, &resp) != RIP_OK) {
    snprintf(err, en, "MusicBrainz didn't answer: %s", resp.error);
    http_resp_free(&resp);
    return RIP_ERR;
  }
  if (resp.status == 404) {
    snprintf(err, en, "MusicBrainz doesn't know this disc");
    http_resp_free(&resp);
    return RIP_ENOENT;
  }
  j = http_json(&resp);
  http_resp_free(&resp);
  // The first release with that disc; one with a front cover beats one without.
  release = cJSON_GetArrayItem(cJSON_GetObjectItemCaseSensitive(j, "releases"), 0);
  {
    const cJSON *r;
    cJSON_ArrayForEach(r, cJSON_GetObjectItemCaseSensitive(j, "releases")) {
      if (cJSON_IsTrue(
              cJSON_GetObjectItemCaseSensitive(cJSON_GetObjectItemCaseSensitive(r, "cover-art-archive"), "front"))) {
        release = r;
        break;
      }
    }
  }
  if (!release) {
    snprintf(err, en, "MusicBrainz has no release for this disc");
    cJSON_Delete(j);
    return RIP_ENOENT;
  }
  svc_str_copy(meta->album, sizeof(meta->album), jstr(release, "title", ""));
  svc_str_copy(meta->date, sizeof(meta->date), jstr(release, "date", ""));
  svc_str_copy(meta->release_id, sizeof(meta->release_id), jstr(release, "id", ""));
  meta->has_cover = cJSON_IsTrue(
      cJSON_GetObjectItemCaseSensitive(cJSON_GetObjectItemCaseSensitive(release, "cover-art-archive"), "front"));
  credit(cJSON_GetObjectItemCaseSensitive(release, "artist-credit"), meta->artist, sizeof(meta->artist));
  // The medium this disc is: a set's other discs have tracks of their own.
  medium = NULL;
  cJSON_ArrayForEach(m, cJSON_GetObjectItemCaseSensitive(release, "media")) {
    const cJSON *d;
    cJSON_ArrayForEach(d, cJSON_GetObjectItemCaseSensitive(m, "discs")) {
      if (!strcmp(jstr(d, "id", ""), discid)) {
        medium = m;
      }
    }
  }
  if (!medium) {
    medium = cJSON_GetArrayItem(cJSON_GetObjectItemCaseSensitive(release, "media"), 0);
  }
  if (medium && cJSON_GetArraySize(cJSON_GetObjectItemCaseSensitive(release, "media")) > 1) {
    size_t l = strlen(meta->album);
    snprintf(meta->album + l, sizeof(meta->album) - l, " (Disc %d)", jint(medium, "position", 1));
  }
  cJSON_ArrayForEach(m, cJSON_GetObjectItemCaseSensitive(medium, "tracks")) {
    int pos = jint(m, "position", 0);
    if (pos >= 1 && pos <= 99) {
      svc_str_copy(meta->titles[pos - 1], sizeof(meta->titles[0]), jstr(m, "title", ""));
      found++;
    }
  }
  RIP_INFO("MusicBrainz: %s - %s (%s), %d track title(s)", meta->artist, meta->album, meta->date, found);
  cJSON_Delete(j);
  return RIP_OK;
}
