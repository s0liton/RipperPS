#include "langs.h"

#include <ctype.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>

#include "cJSON.h"
#include "ripper/common.h"

struct lang {
  const char *key;
  const char *name;
  // Folder names for it, lower case without "_", "-" or spaces.
  const char *aliases[12];
  // The PS5 system languages it covers, as pgo_stub's playgo_stub.dat names them.
  const char *playgo;
};

static const struct lang LANGS[] = {
    {"en",
     "English",
     {"english", "eng", "enus", "engb", "usenglish", "ukenglish", "americanenglish", "britishenglish"},
     "english_us,english_gb"},
    {"fr", "French", {"french", "fre", "fra", "frfr", "frca", "canadianfrench", "francais"}, "french,french_ca"},
    {"de", "German", {"german", "ger", "deu", "dede", "deutsch"}, "german"},
    {"it", "Italian", {"italian", "ita", "itit", "italiano"}, "italian"},
    {"es", "Spanish", {"spanish", "spa", "eses", "castilian", "espanol", "spanishspain"}, "spanish"},
    {"es-419",
     "Latin American Spanish",
     {"mexicanspanish", "latamspanish", "latinamericanspanish", "latinspanish", "spanishlatam", "spanishlatinamerica",
      "spanishmexico", "esmx", "es419", "eslatam"},
     "spanish_la"},
    {"pt", "Portuguese", {"portuguese", "por", "ptpt", "portugueseportugal", "europeanportuguese"}, "portuguese_pt"},
    {"pt-BR",
     "Brazilian Portuguese",
     {"brazilianportuguese", "portuguesebrazil", "portuguesebrazilian", "ptbr", "brazilian"},
     "portuguese_br"},
    {"ja", "Japanese", {"japanese", "jpn", "jajp", "jpjp"}, "japanese"},
    {"ko", "Korean", {"korean", "kor", "kokr"}, "korean"},
    {"zh",
     "Chinese",
     {"chinese", "chs", "cht", "zhcn", "zhtw", "zhhk", "zhhans", "zhhant", "simplifiedchinese", "traditionalchinese",
      "chinesesimplified", "chinesetraditional"},
     "chinese_t,chinese_s"},
    {"ru", "Russian", {"russian", "rus", "ruru"}, "russian"},
    {"pl", "Polish", {"polish", "pol", "plpl"}, "polish"},
    {"nl", "Dutch", {"dutch", "nld", "nlnl"}, "dutch"},
    {"sv", "Swedish", {"swedish", "swe", "svse"}, "swedish"},
    {"da", "Danish", {"danish", "dan", "dadk"}, "danish"},
    {"no", "Norwegian", {"norwegian", "nor", "nbno", "nono"}, "norwegian"},
    {"fi", "Finnish", {"finnish", "fin", "fifi"}, "finnish"},
    {"tr", "Turkish", {"turkish", "tur", "trtr"}, "turkish"},
    {"ar", "Arabic", {"arabic", "ara", "arae", "arsa"}, "arabic"},
    {"cs", "Czech", {"czech", "ces", "cze", "cscz"}, "czech"},
    {"hu", "Hungarian", {"hungarian", "hun", "huhu"}, "hungarian"},
    {"el", "Greek", {"greek", "gre", "ell", "elgr"}, "greek"},
    {"th", "Thai", {"thai", "tha", "thth"}, "thai"},
    {"uk", "Ukrainian", {"ukrainian", "ukr", "ukua"}, NULL},
};

// What folder names start with before the language: "eLID_German", "lang_fr".
static const char *const PREFIXES[] = {"elid", "lid", "language", "lang", "locale", "loc", "lng", "voice", "vo"};

static const struct lang *find_key(const char *key) {
  size_t i;

  for (i = 0; i < RIP_ARRAY_LEN(LANGS); i++) {
    if (!strcmp(LANGS[i].key, key)) {
      return &LANGS[i];
    }
  }
  return NULL;
}

static const struct lang *find_alias(const char *plain) {
  size_t i;
  size_t k;

  for (i = 0; i < RIP_ARRAY_LEN(LANGS); i++) {
    for (k = 0; k < RIP_ARRAY_LEN(LANGS[i].aliases) && LANGS[i].aliases[k]; k++) {
      if (!strcmp(LANGS[i].aliases[k], plain)) {
        return &LANGS[i];
      }
    }
  }
  return NULL;
}

// "en-US" -> "en", "es-419" and "es-MX" -> "es-419", "pt-BR" -> "pt-BR".
static const char *locale_key(const char *locale) {
  char base[8];
  size_t n = 0;
  const char *region;

  while (locale[n] && locale[n] != '-' && locale[n] != '_' && n + 1 < sizeof(base)) {
    base[n] = (char)tolower((unsigned char)locale[n]);
    n++;
  }
  base[n] = '\0';
  region = locale[n] ? locale + n + 1 : "";
  if (!strcmp(base, "es") && (!strcmp(region, "419") || !strcasecmp(region, "MX") || !strcasecmp(region, "US"))) {
    return "es-419";
  }
  if (!strcmp(base, "pt") && !strcasecmp(region, "BR")) {
    return "pt-BR";
  }
  if (!strcmp(base, "nb") || !strcmp(base, "nn")) {
    return "no";
  }
  {
    const struct lang *l = find_key(base);
    return l ? l->key : NULL;
  }
}

static const char *jstr_or(const cJSON *j, const char *key) {
  const cJSON *v = cJSON_GetObjectItemCaseSensitive(j, key);
  return cJSON_IsString(v) ? v->valuestring : NULL;
}

int langs_from_scenario(const char *json, struct langs *out) {
  cJSON *j = cJSON_Parse(json ? json : "");
  const cJSON *list = cJSON_GetObjectItemCaseSensitive(j, "chunkSupportedLanguages");
  const cJSON *v;

  memset(out, 0, sizeof(*out));
  cJSON_ArrayForEach(v, list) {
    const char *key = cJSON_IsString(v) ? locale_key(v->valuestring) : NULL;
    if (key && !langs_has(out, key) && out->n < LANGS_MAX) {
      out->keys[out->n++] = key;
    }
  }
  {
    const char *def = jstr_or(j, "chunkDefaultLanguage");
    const char *key = def ? locale_key(def) : NULL;
    out->main = key && langs_has(out, key) ? key : out->n ? out->keys[0] : NULL;
  }
  cJSON_Delete(j);
  return out->n ? RIP_OK : RIP_ENOENT;
}

const char *langs_folder(const char *name) {
  char plain[64];
  size_t n = 0;
  size_t i;
  const struct lang *l;

  for (; *name; name++) {
    if (*name == '_' || *name == '-' || *name == ' ') {
      continue;
    }
    if (n + 1 >= sizeof(plain)) {
      return NULL;
    }
    plain[n++] = (char)tolower((unsigned char)*name);
  }
  plain[n] = '\0';
  if ((l = find_alias(plain)) != NULL) {
    return l->key;
  }
  for (i = 0; i < RIP_ARRAY_LEN(PREFIXES); i++) {
    size_t pl = strlen(PREFIXES[i]);
    if (!strncmp(plain, PREFIXES[i], pl) && (l = find_alias(plain + pl)) != NULL) {
      return l->key;
    }
  }
  return NULL;
}

int langs_has(const struct langs *l, const char *key) {
  int i;

  for (i = 0; i < l->n; i++) {
    if (!strcmp(l->keys[i], key)) {
      return 1;
    }
  }
  return 0;
}

int langs_playgo(const struct langs *l, char *out, size_t n) {
  int count = 0;
  int i;

  if (n) {
    out[0] = '\0';
  }
  for (i = 0; i < l->n; i++) {
    const struct lang *lang = find_key(l->keys[i]);
    size_t at = strlen(out);
    if (lang && lang->playgo && at + strlen(lang->playgo) + 2 <= n) {
      snprintf(out + at, n - at, "%s%s", count ? "," : "", lang->playgo);
      count++;
    }
  }
  return count;
}

const char *langs_name(const char *key) {
  const struct lang *l = find_key(key);

  return l ? l->name : key;
}
