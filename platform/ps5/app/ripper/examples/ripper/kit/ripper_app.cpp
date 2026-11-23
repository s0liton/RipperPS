// RipperPS: what the screen shows, kept up to date from the ripper payload.
// See ripper_app.hpp.
//
// SPDX-License-Identifier: GPL-3.0-or-later
#include "ripper_app.hpp"

#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/file.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iterator>
#include <memory>
#include <thread>

#include "cJSON.h"
#include "stb_image.h"  // implemented by base/VulkanglTFModel.cpp
#include "ui/fonts.hpp"

namespace ripper {

namespace {

constexpr float kStatusEvery = 3.0f;
constexpr float kDrivesEvery = 1.0f;    // a disc going in shows within a second
constexpr float kJobsEvery = 1.0f;      // while one is running
constexpr float kJobsIdleEvery = 5.0f;  // otherwise
constexpr float kPairEvery = 2.0f;      // while a RomM sign-in waits for its code
constexpr std::size_t kCoverTextures = 16;

void *g_renderer = nullptr;

using Json = std::unique_ptr<cJSON, void (*)(cJSON *)>;

Json parse(const Response &r) {
  return Json(r.json(), cJSON_Delete);
}

const char *str(const cJSON *o, const char *key, const char *fallback = "") {
  const cJSON *v = cJSON_GetObjectItemCaseSensitive(o, key);
  return cJSON_IsString(v) ? v->valuestring : fallback;
}

double num(const cJSON *o, const char *key, double fallback = 0) {
  const cJSON *v = cJSON_GetObjectItemCaseSensitive(o, key);
  return cJSON_IsNumber(v) ? v->valuedouble : fallback;
}

bool flag(const cJSON *o, const char *key) {
  return cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(o, key));
}

// Any JSON value as the text the forms work with: "1"/"0" for booleans.
std::string as_text(const cJSON *v) {
  if (cJSON_IsString(v))
    return v->valuestring;
  if (cJSON_IsBool(v))
    return cJSON_IsTrue(v) ? "1" : "0";
  if (cJSON_IsNumber(v)) {
    char text[32];
    std::snprintf(text, sizeof text, "%g", v->valuedouble);
    return text;
  }
  return "";
}

// A cover's colours: its average as the body, a darker tone, and its most
// saturated region's colour as the accent, lifted so it reads on dark glass.
// (RommPS's palette_of.)
Palette palette_of(const unsigned char *rgba, int w, int h) {
  double sum[3] = {0, 0, 0}, sat_sum[3] = {0, 0, 0}, sat_weight = 0;
  const int step = std::max(1, (w * h) / 4096);
  int n = 0;
  for (int i = 0; i < w * h; i += step) {
    const unsigned char *p = rgba + static_cast<std::size_t>(i) * 4;
    const int mx = std::max({p[0], p[1], p[2]}), mn = std::min({p[0], p[1], p[2]});
    const double sat = mx > 0 ? static_cast<double>(mx - mn) / mx : 0.0;
    for (int c = 0; c < 3; ++c) {
      sum[c] += p[c];
      sat_sum[c] += p[c] * sat * sat;
    }
    sat_weight += sat * sat;
    ++n;
  }
  if (n == 0)
    return {};
  auto rgb = [](double r, double g, double b) {
    auto c = [](double v) { return static_cast<std::uint32_t>(std::clamp(v, 0.0, 255.0)); };
    return Color::rgb((c(r) << 16) | (c(g) << 8) | c(b));
  };
  Palette out;
  out.mid = rgb(sum[0] / n, sum[1] / n, sum[2] / n);
  out.dark = rgb(sum[0] / n * 0.35, sum[1] / n * 0.35, sum[2] / n * 0.35);
  if (sat_weight > 0.5) {
    double a[3] = {sat_sum[0] / sat_weight, sat_sum[1] / sat_weight, sat_sum[2] / sat_weight};
    const double peak = std::max({a[0], a[1], a[2], 1.0});
    const double lift = 230.0 / peak;
    out.accent = rgb(a[0] * lift, a[1] * lift, a[2] * lift);
  }
  return out;
}

// The faces the screen draws text with (set_text_fonts). plain() keeps a
// character only when all of them have it.
const hui::gfx::Font *g_text_fonts[4] = {};

// What ps5/ui/setup-kit.sh bakes into them: the kit's "european" set and its
// symbols. Only asked before the fonts are loaded.
bool baked(unsigned cp) {
  if ((cp >= 0xA1 && cp <= 0x17F) || (cp >= 0x410 && cp <= 0x44F))
    return true;
  switch (cp) {
  case 0x401:
  case 0x451:
  case 0x2013:
  case 0x2014:
  case 0x2018:
  case 0x2019:
  case 0x201C:
  case 0x201D:
  case 0x201E:
  case 0x2022:
  case 0x2026:
  case 0x20AC:
  case 0x2122:
  case 0x2190:
  case 0x2191:
  case 0x2192:
  case 0x2193:
  case 0x2713:
    return true;
  }
  return false;
}

bool drawable(unsigned cp) {
  if (!g_text_fonts[0])
    return baked(cp);
  for (const hui::gfx::Font *f : g_text_fonts)
    if (f && !f->has_glyph(cp))
      return false;
  return true;
}

void put_utf8(std::string &out, unsigned cp) {
  if (cp < 0x80)
    out += static_cast<char>(cp);
  else if (cp < 0x800)
    out += static_cast<char>(0xC0 | cp >> 6), out += static_cast<char>(0x80 | (cp & 0x3F));
  else if (cp < 0x10000)
    out += static_cast<char>(0xE0 | cp >> 12), out += static_cast<char>(0x80 | (cp >> 6 & 0x3F)),
        out += static_cast<char>(0x80 | (cp & 0x3F));
  else
    out += static_cast<char>(0xF0 | cp >> 18), out += static_cast<char>(0x80 | (cp >> 12 & 0x3F)),
        out += static_cast<char>(0x80 | (cp >> 6 & 0x3F)), out += static_cast<char>(0x80 | (cp & 0x3F));
}

constexpr unsigned kNothing = 0xFFFFFFFFu;

// The next character of text, advancing p. Bytes that aren't UTF-8 are read
// as Latin-1 (old file names are), a stray continuation byte as nothing; a
// broken sequence never becomes a control character.
unsigned next_char(const unsigned char *&p) {
  const unsigned lead = *p++;
  if (lead < 0x80)
    return lead;
  const int more = lead >= 0xC2 && lead <= 0xDF   ? 1
                   : lead >= 0xE0 && lead <= 0xEF ? 2
                   : lead >= 0xF0 && lead <= 0xF4 ? 3
                                                  : 0;
  if (!more)
    return lead >= 0xC0 ? lead : kNothing;
  unsigned cp = lead & (0x3F >> more);
  for (int i = 0; i < more; ++i) {
    if ((p[i] & 0xC0) != 0x80)
      return lead;  // the bytes after it stay, as characters of their own
    cp = cp << 6 | (p[i] & 0x3F);
  }
  p += more;
  // Too long a form, a surrogate, past Unicode: nothing to show
  static const unsigned kLeast[] = {0, 0x80, 0x800, 0x10000};
  if (cp < kLeast[more] || (cp >= 0xD800 && cp <= 0xDFFF) || cp > 0x10FFFF)
    return kNothing;
  return cp;
}

// Marks that sit on the letter before them (decomposed text: "e" then U+0301).
bool combining(unsigned cp) {
  return (cp >= 0x300 && cp <= 0x36F) || (cp >= 0x1AB0 && cp <= 0x1AFF) || (cp >= 0x1DC0 && cp <= 0x1DFF) ||
         (cp >= 0x20D0 && cp <= 0x20FF) || (cp >= 0xFE20 && cp <= 0xFE2F);
}

// What a name can carry that shows as nothing: control codes, a soft hyphen, zero widths,
// direction marks, variation selectors, the byte order mark.
bool invisible(unsigned cp) {
  return (cp >= 0x80 && cp < 0xA0) || cp == 0xAD || (cp >= 0x200B && cp <= 0x200F) || (cp >= 0x202A && cp <= 0x202E) ||
         (cp >= 0x2060 && cp <= 0x2064) || (cp >= 0xFE00 && cp <= 0xFE0F) || cp == 0xFEFF;
}

// A letter and the mark after it, as one character: 0 when there's none.
unsigned compose(char base, unsigned mark) {
  static const struct {
    unsigned mark;
    const char *bases;
    const char16_t *made;
  } kMarks[] = {
      {0x300, "AEIOUaeiou", u"ÀÈÌÒÙàèìòù"},
      {0x301, "AEIOUYaeiouyCcLlNnRrSsZz",
       u"ÁÉÍÓÚÝáéíóúý"
       u"ĆćĹĺŃńŔŕŚśŹź"},
      {0x302, "AEIOUaeiouCcGgHhJjSsWwYy",
       u"ÂÊÎÔÛâêîôû"
       u"ĈĉĜĝĤĥĴĵŜŝŴŵŶŷ"},
      {0x303, "ANOanoIiUu", u"ÃÑÕãñõĨĩŨũ"},
      {0x304, "AEIOUaeiou", u"ĀĒĪŌŪāēīōū"},
      {0x306, "AEGIOUaegiou", u"ĂĔĞĬŎŬăĕğĭŏŭ"},
      {0x307, "CEGIZcegz", u"ĊĖĠİŻċėġż"},
      {0x308, "AEIOUYaeiouy", u"ÄËÏÖÜŸäëïöüÿ"},
      {0x30A, "AaUu", u"ÅåŮů"},
      {0x30B, "OoUu", u"ŐőŰű"},
      {0x30C, "CDELNRSTZcdelnrstz", u"ČĎĚĽŇŘŠŤŽčďěľňřšťž"},
      {0x327, "CcGgKkLlNnRrSsTt", u"ÇçĢģĶķĻļŅņŖŗŞşŢţ"},
      {0x328, "AEIUaeiu", u"ĄĘĮŲąęįų"},
  };
  for (const auto &m : kMarks)
    if (m.mark == mark)
      for (int i = 0; m.bases[i]; ++i)
        if (m.bases[i] == base)
          return m.made[i];
  return 0;
}

// Appends what a character the fonts lack reads as, in ASCII; false for one
// with no stand-in (Japanese, say).
bool fold(unsigned cp, std::string &out) {
  static const char *const kSymbols[] = {// U+00A0 to U+00BF
                                         " ",  "",  "c", "GBP", "",  "JPY", "|",   "S",   "",    "(C)", "a",
                                         "\"", "-", "",  "(R)", "",  "o",   "+/-", "2",   "3",   "'",   "u",
                                         "",   ".", "",  "1",   "o", "\"",  "1/4", "1/2", "3/4", ""};
  static const char kLatin1[] =  // U+00C0 to U+00FF; Æ, Þ, ß, æ and þ are below
      "AAAAAAACEEEEIIII"
      "DNOOOOOxOUUUUYTs"
      "aaaaaaaceeeeiiii"
      "dnooooo/ouuuuyty";
  static const char kExtendedA[] =  // U+0100 to U+017F; Ĳ, ĳ, Œ and œ are below
      "AaAaAaCcCcCcCcDd"
      "DdEeEeEeEeEeGgGg"
      "GgGgHhHhIiIiIiIi"
      "IiIiJjKkkLlLlLlL"
      "lLlNnNnNnnNnOoOo"
      "OoOoRrRrRrSsSsSs"
      "SsTtTtTtUuUuUuUu"
      "UuUuWwYyYZzZzZzs";
  static const char kPinyin[] = "AaIiOoUuUuUuUuUu";  // U+01CD to U+01DC: Ǎ ǎ Ǐ ǐ ... ǜ
  // Greek and Cyrillic capitals in Latin letters; the small ones are these in lower case
  static const char *const kGreek[] = {// U+0391 to U+03A9 (U+03A2, unused, as final sigma's slot)
                                       "A", "B", "G", "D", "E", "Z", "I", "Th", "I", "K",  "L",  "M", "N",
                                       "X", "O", "P", "R", "s", "S", "T", "Y",  "F", "Ch", "Ps", "O"};
  static const char *const kCyrillic[] = {// U+0410 to U+042F
                                          "A",  "B",  "V",  "G",    "D", "E", "Zh", "Z", "I",  "Y", "K",
                                          "L",  "M",  "N",  "O",    "P", "R", "S",  "T", "U",  "F", "Kh",
                                          "Ts", "Ch", "Sh", "Shch", "",  "Y", "",   "E", "Yu", "Ya"};
  auto lower = [&out](const char *s) {
    for (; *s; ++s)
      out += static_cast<char>(std::tolower(static_cast<unsigned char>(*s)));
    return true;
  };
  auto put = [&out](const char *s) { return out += s, true; };
  auto one = [&out](char c) { return out += c, true; };
  if (cp >= 0xA0 && cp <= 0xBF)
    return put(kSymbols[cp - 0xA0]);
  if (cp >= 0xC0 && cp <= 0xFF)
    return cp == 0xC6   ? put("AE")
           : cp == 0xDE ? put("Th")
           : cp == 0xDF ? put("ss")
           : cp == 0xE6 ? put("ae")
           : cp == 0xFE ? put("th")
                        : one(kLatin1[cp - 0xC0]);
  if (cp >= 0x100 && cp <= 0x17F)
    return cp == 0x132   ? put("IJ")
           : cp == 0x133 ? put("ij")
           : cp == 0x152 ? put("OE")
           : cp == 0x153 ? put("oe")
                         : one(kExtendedA[cp - 0x100]);
  if (cp >= 0x1CD && cp <= 0x1DC)
    return one(kPinyin[cp - 0x1CD]);
  if (cp >= 0x391 && cp <= 0x3A9)
    return put(kGreek[cp - 0x391]);
  if (cp >= 0x3B1 && cp <= 0x3C9)
    return lower(kGreek[cp - 0x3B1]);
  if (cp >= 0x410 && cp <= 0x42F)
    return put(kCyrillic[cp - 0x410]);
  if (cp >= 0x430 && cp <= 0x44F)
    return lower(kCyrillic[cp - 0x430]);
  if (cp >= 0xFF01 && cp <= 0xFF5E)  // full-width ASCII
    return one(static_cast<char>(cp - 0xFEE0));
  if ((cp >= 0x2000 && cp <= 0x200A) || cp == 0x2028 || cp == 0x2029 || cp == 0x202F || cp == 0x205F || cp == 0x3000)
    return one(' ');
  switch (cp) {
  case 0x192:
    return put("f");
  case 0x1A0:
    return put("O");
  case 0x1A1:
    return put("o");
  case 0x1AF:
    return put("U");
  case 0x1B0:
    return put("u");
  case 0x218:
    return put("S");
  case 0x219:
    return put("s");
  case 0x21A:
    return put("T");
  case 0x21B:
    return put("t");
  case 0x2BC:
    return put("'");
  case 0x2C6:
    return put("^");
  case 0x2DC:
    return put("~");
  case 0x386:
    return put("A");
  case 0x388:
    return put("E");
  case 0x389:
  case 0x38A:
    return put("I");
  case 0x38C:
  case 0x38F:
    return put("O");
  case 0x38E:
    return put("Y");
  case 0x390:
  case 0x3AE:
  case 0x3AF:
  case 0x3CA:
    return put("i");
  case 0x3AC:
    return put("a");
  case 0x3AD:
    return put("e");
  case 0x3CB:
  case 0x3CD:
    return put("y");
  case 0x3CC:
  case 0x3CE:
    return put("o");
  case 0x401:
    return put("Yo");
  case 0x451:
    return put("yo");
  case 0x2010:
  case 0x2011:
  case 0x2012:
  case 0x2013:
  case 0x2014:
  case 0x2015:
  case 0x2212:
    return put("-");
  case 0x2018:
  case 0x2019:
  case 0x201A:
  case 0x201B:
  case 0x2032:
    return put("'");
  case 0x201C:
  case 0x201D:
  case 0x201E:
  case 0x201F:
  case 0x2033:
    return put("\"");
  case 0x2039:
    return put("<");
  case 0x203A:
    return put(">");
  case 0x2020:
    return put("+");
  case 0x2022:
  case 0x2605:
  case 0x2606:
    return put("*");
  case 0x2024:
    return put(".");
  case 0x2025:
    return put("..");
  case 0x2026:
    return put("...");
  case 0x2030:
    return put("%");
  case 0x2044:
  case 0x2215:
    return put("/");
  case 0x20AC:
    return put("EUR");
  case 0x2116:
    return put("No");
  case 0x2122:
    return put("TM");
  case 0x2190:
    return put("<-");
  case 0x2192:
    return put("->");
  }
  return false;
}

// Text the screen's fonts can draw: what they have stays ("Pokémon",
// "Luigi’s Mansion"), anything else folds to its nearest ASCII ("Ō" reads "O",
// "ß" "ss", "™" "TM" in a font without them); a decomposed accent joins its
// letter, or goes. Only a character with no stand-in at all is "?".
std::string plain(const char *text) {
  std::string out;
  const auto *p = reinterpret_cast<const unsigned char *>(text);
  std::size_t base_at = 0;  // where the last plain letter is in out, for a mark after it
  char base = 0;
  while (*p) {
    const unsigned cp = next_char(p);
    if (cp == kNothing || invisible(cp))
      continue;
    if (cp < 0x80) {
      base = std::isalpha(static_cast<int>(cp)) ? static_cast<char>(cp) : 0;
      base_at = out.size();
      out += static_cast<char>(cp);
      continue;
    }
    if (combining(cp)) {
      const unsigned made = base ? compose(base, cp) : 0;
      if (made && drawable(made)) {
        out.resize(base_at);
        put_utf8(out, made);
      }
      base = 0;  // a second mark on the same letter goes
      continue;
    }
    base = 0;
    if (cp != 0xA0 && drawable(cp)) {
      put_utf8(out, cp);
      continue;
    }
    if (!fold(cp, out))
      out += '?';
  }
  return out;
}

Drive parse_drive(const cJSON *o) {
  Drive d;
  d.id = str(o, "id");
  d.label = plain(str(o, "label"));
  d.source = str(o, "source");
  d.vendor = plain(str(o, "vendor"));
  d.product = plain(str(o, "product"));
  d.revision = plain(str(o, "revision"));
  d.reads = str(o, "reads");
  d.state = str(o, "state");
  d.message = sentence(plain(str(o, "message")));
  d.disc_seq = static_cast<int>(num(o, "disc_seq"));
  d.job = static_cast<int>(num(o, "job"));
  if (const cJSON *in = cJSON_GetObjectItemCaseSensitive(o, "install"); cJSON_IsObject(in)) {
    d.install.done = num(in, "done");
    d.install.total = num(in, "total");
    d.install.rate = num(in, "rate");
    d.install.eta = num(in, "eta");
    d.install.playable = flag(in, "playable");
  }
  if (const cJSON *disc = cJSON_GetObjectItemCaseSensitive(o, "disc"); cJSON_IsObject(disc)) {
    d.has_disc = true;
    d.disc.kind = plain(str(disc, "kind"));
    d.disc.profile = plain(str(disc, "profile"));
    d.disc.title = plain(str(disc, "title"));
    d.disc.title_id = str(disc, "title_id");
    d.disc.volume_id = plain(str(disc, "volume_id"));
    d.disc.size = num(disc, "size");
    d.disc.css = flag(disc, "css");
    d.disc.tracks = static_cast<int>(num(disc, "tracks"));
    d.disc.audio_tracks = static_cast<int>(num(disc, "audio_tracks"));
    d.disc.display = plain(str(disc, "display"));
    d.disc.subtitle = plain(str(disc, "subtitle"));
    d.disc.artist = plain(str(disc, "artist"));
    d.disc.details = str(disc, "details");
    d.disc.cover = str(disc, "cover");
  }
  if (const cJSON *offer = cJSON_GetObjectItemCaseSensitive(o, "offer"); cJSON_IsObject(offer)) {
    d.has_offer = true;
    d.system = str(offer, "system");
    d.system_name = plain(str(offer, "system_name"));
    d.group = str(offer, "group");
    const cJSON *f;
    cJSON_ArrayForEach(f, cJSON_GetObjectItemCaseSensitive(offer, "formats")) d.formats.push_back(
        {str(f, "id"), plain(str(f, "label")), str(f, "availability"), sentence(plain(str(f, "reason"))), flag(f, "available")});
    const cJSON *s = cJSON_GetObjectItemCaseSensitive(offer, "suggestion");
    d.suggestion.format = str(s, "format");
    d.suggestion.destination = str(s, "destination");
    d.suggestion.folder = str(s, "folder");
    d.suggestion.romm_platform = str(s, "romm_platform");
    d.suggestion.name = str(s, "name");  // sent back as it came: it names the files
    d.suggestion.options = "{}";
    if (const cJSON *opts = cJSON_GetObjectItemCaseSensitive(s, "options"); cJSON_IsObject(opts))
      if (char *text = cJSON_PrintUnformatted(opts)) {
        d.suggestion.options = text;
        cJSON_free(text);
      }
  }
  return d;
}

Job parse_job(const cJSON *o) {
  Job j;
  j.id = static_cast<int>(num(o, "id"));
  j.state = str(o, "state");
  j.phase = plain(str(o, "phase"));
  j.name = plain(str(o, "name"));
  j.system = str(o, "system");
  j.system_name = plain(str(o, "system_name"));
  j.format = str(o, "format");
  j.format_label = plain(str(o, "format_label"));
  j.destination = str(o, "destination");
  j.destination_name = plain(str(o, "destination_name"));
  j.drive = str(o, "drive");
  j.drive_label = plain(str(o, "drive_label"));
  j.where = plain(str(o, "where"));
  j.error = sentence(plain(str(o, "error")));
  j.note = sentence(plain(str(o, "note")));
  j.done = num(o, "done");
  j.total = num(o, "total");
  j.rate = num(o, "rate");
  j.eta = num(o, "eta", -1);
  j.started = num(o, "started");
  j.finished = num(o, "finished");
  j.retryable = flag(o, "retryable");
  j.ripped = flag(o, "ripped");
  j.stage = str(o, "stage");
  const cJSON *rip = cJSON_GetObjectItemCaseSensitive(o, "rip");
  j.rip_done = num(rip, "done");
  j.rip_total = num(rip, "total");
  j.rip_seconds = num(rip, "seconds");
  if (const cJSON *x = cJSON_GetObjectItemCaseSensitive(o, "transfer"); cJSON_IsObject(x)) {
    j.has_transfer = true;
    j.transfer_kind = str(x, "kind");
    j.transfer_done = num(x, "done");
    j.transfer_total = num(x, "total");
  }
  return j;
}

Catalog parse_catalog(const cJSON *o) {
  Catalog c;
  const cJSON *s;
  c.known = true;
  cJSON_ArrayForEach(s, cJSON_GetObjectItemCaseSensitive(o, "systems")) {
    System sys{str(s, "id"), plain(str(s, "name")), str(s, "group"), str(s, "romm_slug"), {}};
    const cJSON *f;
    cJSON_ArrayForEach(f, cJSON_GetObjectItemCaseSensitive(s, "formats")) {
      FormatInfo fi{str(f, "id"), plain(str(f, "label")), plain(str(f, "help")), flag(f, "planned"), {}};
      const cJSON *opt;
      cJSON_ArrayForEach(opt, cJSON_GetObjectItemCaseSensitive(f, "options")) {
        Option op{str(opt, "id"),
                  plain(str(opt, "label")),
                  str(opt, "type"),
                  {},
                  as_text(cJSON_GetObjectItemCaseSensitive(opt, "default"))};
        const cJSON *ch;
        cJSON_ArrayForEach(ch, cJSON_GetObjectItemCaseSensitive(opt, "choices"))
            op.choices.push_back({str(ch, "id"), plain(str(ch, "label")), str(ch, "only")});
        fi.options.push_back(std::move(op));
      }
      sys.formats.push_back(std::move(fi));
    }
    c.systems.push_back(std::move(sys));
  }
  const cJSON *t;
  cJSON_ArrayForEach(t, cJSON_GetObjectItemCaseSensitive(o, "destination_types")) {
    DestType dt{str(t, "type"),
                plain(str(t, "label")),
                plain(str(t, "help")),
                !cJSON_IsFalse(cJSON_GetObjectItemCaseSensitive(t, "available")),
                {}};
    const cJSON *f;
    cJSON_ArrayForEach(f, cJSON_GetObjectItemCaseSensitive(t, "fields"))
        dt.fields.push_back({str(f, "id"), plain(str(f, "label")), str(f, "type"), plain(str(f, "placeholder")),
                             as_text(cJSON_GetObjectItemCaseSensitive(f, "default"))});
    c.dest_types.push_back(std::move(dt));
  }
  return c;
}

Settings parse_settings(const cJSON *j) {
  Settings s;
  s.known = true;
  s.notify = flag(j, "notify");
  s.auto_eject = flag(j, "auto_eject");
  s.auto_start = flag(j, "auto_start");
  s.tmdb_key_set = flag(j, "tmdb_key_set");
  s.read_retries = static_cast<int>(num(j, "read_retries", 3));
  const cJSON *k;
  cJSON_ArrayForEach(k, cJSON_GetObjectItemCaseSensitive(j, "key_files")) {
    KeyFile f{str(k, "id"), str(k, "label"), str(k, "path"), str(k, "default"), flag(k, "found"),
              static_cast<int>(num(k, "count", 0))};
    s.key_files.push_back(f);
  }
  const cJSON *d;
  cJSON_ArrayForEach(d, cJSON_GetObjectItemCaseSensitive(j, "destinations")) {
    Destination dest{str(d, "id"), str(d, "type"), plain(str(d, "name")), "", {}, {}};
    const cJSON *f;
    cJSON_ArrayForEach(f, d) {
      const std::string key = f->string;
      if (key.size() > 4 && key.compare(key.size() - 4, 4, "_set") == 0)
        dest.secret_set[key.substr(0, key.size() - 4)] = cJSON_IsTrue(f);
      else
        dest.values[key] = as_text(f);
    }
    const std::string host = dest.values["host"], url = dest.values["url"], path = dest.values["path"];
    dest.where = plain_text(!url.empty() ? url : !host.empty() ? host + (path.empty() ? "" : "/" + path) : path);
    s.destinations.push_back(std::move(dest));
  }
  const cJSON *p;
  cJSON_ArrayForEach(p, cJSON_GetObjectItemCaseSensitive(j, "profiles")) {
    Profile pr{str(p, "format"), str(p, "destination"), str(p, "folder"), str(p, "romm_platform"), {}};
    const cJSON *o;
    cJSON_ArrayForEach(o, cJSON_GetObjectItemCaseSensitive(p, "options")) pr.options[o->string] = as_text(o);
    s.profiles[p->string] = std::move(pr);
  }
  return s;
}

// A destination as the payload takes it: every field, secrets only when typed.
cJSON *destination_json(const Destination &d) {
  cJSON *o = cJSON_CreateObject();
  cJSON_AddStringToObject(o, "id", d.id.c_str());
  cJSON_AddStringToObject(o, "type", d.type.c_str());
  cJSON_AddStringToObject(o, "name", d.name.c_str());
  const DestType *t = app().catalog().dest_type(d.type);
  for (const auto &[key, value] : d.values) {
    if (key == "id" || key == "type" || key == "name")
      continue;
    const Field *field = nullptr;
    if (t)
      for (const Field &f : t->fields)
        if (f.id == key)
          field = &f;
    if (field && field->type == "toggle")
      cJSON_AddBoolToObject(o, key.c_str(), value == "1" || value == "true");
    else if (field && field->type == "secret" && value.empty())
      continue;  // left as the payload keeps it
    else
      cJSON_AddStringToObject(o, key.c_str(), value.c_str());
  }
  return o;
}

std::string print(cJSON *o) {
  char *text = cJSON_PrintUnformatted(o);
  std::string out = text ? text : "{}";
  cJSON_free(text);
  cJSON_Delete(o);
  return out;
}

}  // namespace

std::string plain_text(const char *text) {
  return plain(text ? text : "");
}

void set_text_fonts(const hui::ui::Fonts &fonts) {
  const hui::ui::FontRef *faces[] = {&fonts.regular, &fonts.semibold, &fonts.display, &fonts.mono};
  for (int i = 0; i < 4; ++i)
    g_text_fonts[i] = faces[i]->font;
}

std::string json_string(const std::string &s) {
  std::unique_ptr<cJSON, void (*)(cJSON *)> v(cJSON_CreateString(s.c_str()), cJSON_Delete);
  char *text = cJSON_PrintUnformatted(v.get());
  std::string out = text ? text : "\"\"";
  cJSON_free(text);
  return out;
}

std::string sentence(std::string text) {
  if (!text.empty())
    text[0] = static_cast<char>(std::toupper(static_cast<unsigned char>(text[0])));
  return text;
}

std::string human_size(double bytes) {
  static const char *const kUnits[] = {"B", "KB", "MB", "GB", "TB"};
  int unit = 0;
  while (bytes >= 1000.0 && unit < 4) {
    bytes /= 1000.0;
    ++unit;
  }
  char text[32];
  std::snprintf(text, sizeof text, unit == 0 || bytes >= 100 ? "%.0f %s" : "%.1f %s", bytes, kUnits[unit]);
  return text;
}

std::string duration(double seconds) {
  const long s = std::lround(std::max(0.0, seconds));
  char text[32];
  if (s >= 3600)
    std::snprintf(text, sizeof text, "%ld h %02ld min", s / 3600, (s / 60) % 60);
  else if (s >= 60)
    std::snprintf(text, sizeof text, "%ld min %02ld s", s / 60, s % 60);
  else
    std::snprintf(text, sizeof text, "%ld s", s);
  return text;
}

Palette system_palette(const std::string &group, const std::string &system) {
  // PlayStation blues, Sega's, Nintendo's violet, Xbox green; movies amber,
  // music magenta; anything else the app's own.
  struct Tint {
    const char *system;
    std::uint32_t mid, accent;
  };
  static const Tint kSystems[] = {
      {"ps1", 0x1d2f6b, 0x7aa7ff},       {"ps2", 0x14246b, 0x6f8dff},      {"ps3", 0x1b1f3a, 0x9fb4ff},
      {"ps4", 0x0f2a6b, 0x5ea2ff},       {"ps5", 0x1a2a5c, 0xb8c8ff},      {"saturn", 0x2a2a3a, 0xc0c4d6},
      {"segacd", 0x2b1d4d, 0x9d7bff},    {"xbox", 0x123d1a, 0x6be07b},     {"xbox360", 0x123d1a, 0x6be07b},
      {"xboxone", 0x123d1a, 0x6be07b},   {"gamecube", 0x2a1f5a, 0xa58cff}, {"wiiu", 0x1f3a4a, 0x7fd4ff},
      {"dreamcast", 0x3a1f1f, 0xff8c5a}, {"wii", 0x2b3440, 0xd6e4f0}};
  for (const Tint &t : kSystems)
    if (system == t.system)
      return {hui::gfx::mix(Color::rgb(t.mid), Color::rgb(0x000000), 0.65f), Color::rgb(t.mid), Color::rgb(t.accent)};
  if (group == "movies")
    return {Color::rgb(0x1f1408), Color::rgb(0x5a3a12), Color::rgb(0xffbf5e)};
  if (group == "music")
    return {Color::rgb(0x1f0b1d), Color::rgb(0x5a1f55), Color::rgb(0xff7ce0)};
  return Palette{};
}

std::string Drive::model() const {
  return product.empty() || source == "virtual" ? label : product;
}

std::string Drive::disc_name() const {
  if (!disc.display.empty())
    return disc.display;
  if (!disc.title.empty())
    return disc.title;
  // A game's ID says more than its volume label ("PS5VOLUME" on every PS5 disc).
  if (!disc.title_id.empty())
    return disc.title_id;
  if (!disc.volume_id.empty())
    return disc.volume_id;
  if (disc.audio_tracks > 0 && disc.details == "done")
    return "Unknown album";
  return disc.kind.empty() ? "Disc" : disc.kind;
}

const FormatInfo *System::format(const std::string &fid) const {
  for (const FormatInfo &f : formats)
    if (f.id == fid)
      return &f;
  return nullptr;
}

const System *Catalog::system(const std::string &sid) const {
  for (const System &s : systems)
    if (s.id == sid)
      return &s;
  return nullptr;
}

const DestType *Catalog::dest_type(const std::string &t) const {
  for (const DestType &d : dest_types)
    if (d.type == t)
      return &d;
  return nullptr;
}

App &app() {
  static App instance;
  return instance;
}

App::App()
    : api_("127.0.0.1", payload_port()),
      poll_api_("127.0.0.1", payload_port()),
      cover_api_("127.0.0.1", payload_port()) {
  refresh_status();
}

void App::say(const std::string &message) {
  // The console's own notification, through ripper (an app can't send
  // one): "Couldn't eject: <why>" shows "Couldn't eject" in bold with the
  // reason under it. Drawn in the app when ripper can't be reached.
  if (status_.reachable) {
    std::string text = message;
    if (const std::size_t colon = text.find(": "); colon != std::string::npos)
      text.replace(colon, 2, "\n");
    api_.post("/api/notify", "{\"message\":" + json_string(text) + "}", [this, message](const Response &r) {
      if (r.ok())
        return;
      toast_ = sentence(plain(message.c_str()));  // it just stopped
      toast_age_ = 0;
    });
    return;
  }
  toast_ = sentence(plain(message.c_str()));
  toast_age_ = 0;
}

void App::tick(float dt, hui::gfx::Renderer *renderer) {
  clock_ += dt;
  api_.poll();
  poll_api_.poll();
  cover_api_.poll();
  toast_age_ += dt;
  if (!toast_.empty() && toast_age_ > 4.0f)
    toast_.clear();

  status_timer_ += dt;
  if (status_timer_ >= kStatusEvery && !status_busy_)
    refresh_status();
  if (status_.reachable) {
    drives_timer_ += dt;
    if (drives_timer_ >= kDrivesEvery && !drives_busy_)
      refresh_drives();
    jobs_timer_ += dt;
    const bool running = std::any_of(jobs_.begin(), jobs_.end(), [](const Job &j) { return j.active(); });
    if (jobs_timer_ >= (running ? kJobsEvery : kJobsIdleEvery) && !jobs_busy_)
      refresh_jobs();
    if (!settings_.known && !settings_busy_)
      refresh_settings();
    if (!catalog_.known && !catalog_busy_)
      refresh_catalog();
    pair_timer_ += dt;
    if (pairing_.status == "pending" && pair_timer_ >= kPairEvery && !pair_busy_)
      refresh_pairing();
    test_timer_ += dt;
    if (test_.active() && test_timer_ >= 1.5f && !test_busy_)
      refresh_drive_test();
  }

  // The payload sent: its API answers within a few seconds.
  if (start_state_ == 2) {
    start_state_ = 0;
    status_timer_ = kStatusEvery;
  } else if (start_state_ == 3) {
    start_state_ = 0;
    say("Couldn't start ripper: " + start_error_);
  }

  // A fetched cover becomes a texture (one a frame).
  if (!to_decode_.empty()) {
    Pending job = std::move(to_decode_.front());
    to_decode_.erase(to_decode_.begin());
    Cover &c = covers_[job.path];
    int w = 0, h = 0, channels = 0;
    unsigned char *rgba = stbi_load_from_memory(reinterpret_cast<const unsigned char *>(job.bytes.data()),
                                                static_cast<int>(job.bytes.size()), &w, &h, &channels, 4);
    if (!rgba)
      c.failed = true;
    else {
      c.palette = palette_of(rgba, w, h);
      c.aspect = h > 0 ? static_cast<float>(w) / static_cast<float>(h) : 1.0f;
      if (renderer) {
        if (c.texture)
          renderer->destroy_texture(c.texture);
        c.texture = renderer->create_texture(w, h, rgba);
      }
      stbi_image_free(rgba);
    }
  }
  // Covers not drawn for a while go, past a few.
  if (renderer && covers_.size() > kCoverTextures)
    for (auto it = covers_.begin(); it != covers_.end();) {
      if (it->second.used < clock_ - 10.0) {
        if (it->second.texture)
          renderer->destroy_texture(it->second.texture);
        it = covers_.erase(it);
      } else
        ++it;
    }
}

const Cover *App::cover(const Drive &d) {
  if (d.disc.cover.empty())
    return nullptr;
  Cover &c = covers_[d.disc.cover];
  c.used = clock_;
  if (!c.requested) {
    c.requested = true;
    const std::string path = d.disc.cover;
    cover_api_.get(path, [this, path](const Response &r) {
      if (r.ok() && !r.body.empty())
        to_decode_.push_back({path, r.body});
      else
        covers_[path].failed = true;
    });
  }
  return c.texture ? &c : nullptr;
}

void App::refresh_now() {
  refresh_status();
  if (status_.reachable) {
    if (!drives_busy_)
      refresh_drives();
    if (!jobs_busy_)
      refresh_jobs();
  }
}

const std::string &App::bundled_payload() {
  if (!bundled_read_) {
    bundled_read_ = true;
    std::ifstream f("/app0/assets/ripper/ripper.elf", std::ios::binary);
    bundled_.assign(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
  }
  return bundled_;
}

// A build stamp, "YYYYMMDD-HHMMSS" (UTC): two of them compare as text.
static bool is_build_stamp(const std::string &b) {
  if (b.size() != 15 || b[8] != '-')
    return false;
  for (std::size_t i = 0; i < b.size(); i++)
    if (i != 8 && (b[i] < '0' || b[i] > '9'))
      return false;
  return true;
}

std::string App::bundled_build() {
  static const char kMark[] = "ripper-build:";
  const std::string &elf = bundled_payload();
  const std::size_t at = elf.find(kMark);
  if (at == std::string::npos)
    return "";
  const std::size_t start = at + sizeof kMark - 1;
  const std::size_t end = elf.find('\0', start);
  return end == std::string::npos ? "" : elf.substr(start, end - start);
}

// Both HENs load ELF payloads sent to port 9021; RipperPS reaches it on the
// console itself, the way it reaches the payload's API.
void App::start_payload() {
  if (start_state_ == 1)
    return;
  const std::string &elf = bundled_payload();
  if (elf.size() < 4 || elf.compare(0, 4,
                                    "\x7f"
                                    "ELF") != 0) {
    start_error_ = "this RipperPS doesn't carry the ripper payload";
    start_state_ = 3;
    return;
  }
  start_error_.clear();
  start_state_ = 1;
  std::thread([this, elf] {
    std::string err;
    const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    sockaddr_in sa{};
    sa.sin_family = AF_INET;
    sa.sin_port = htons(9021);
    ::inet_pton(AF_INET, "127.0.0.1", &sa.sin_addr);
    if (fd < 0 || ::connect(fd, reinterpret_cast<sockaddr *>(&sa), sizeof sa) != 0)
      err = std::string("the HEN's payload loader (port 9021) didn't answer: ") + std::strerror(errno);
    for (std::size_t sent = 0; err.empty() && sent < elf.size();) {
      const ssize_t n = ::send(fd, elf.data() + sent, elf.size() - sent, 0);
      if (n <= 0)
        err = std::string("sending the payload failed: ") + std::strerror(errno);
      else
        sent += static_cast<std::size_t>(n);
    }
    if (fd >= 0)
      ::close(fd);
    start_error_ = err;
    start_state_ = err.empty() ? 2 : 3;
  }).detach();
}

// ripper holds a lock on this file for as long as it runs; a copy can be
// running and not answering yet, after a start or a wake.
int App::payload_lock() {
  const int fd = ::open("/data/ripper/ripper.lock", O_RDONLY);
  if (fd < 0)
    return errno == ENOENT ? 0 : -1;
  int held = 0;
  if (::flock(fd, LOCK_SH | LOCK_NB) != 0)
    held = errno == EWOULDBLOCK ? 1 : -1;
  ::close(fd);
  return held;
}

void App::refresh_status() {
  status_timer_ = 0;
  status_busy_ = true;
  poll_api_.get("/api/status", [this](const Response &r) {
    status_busy_ = false;
    status_.known = true;
    const bool was = status_.reachable;
    status_.reachable = r.status != 0;
    // Not answering: RipperPS starts the payload it carries, once by
    // itself (Cross on the screen tries again), but only when none is
    // running. One that's running is left to come up; with no way to
    // tell, it waits a few tries first.
    unanswered_ = status_.reachable ? 0 : unanswered_ + 1;
    payload_waiting_ = false;
    if (!status_.reachable && !tried_start_) {
      const int lock = payload_lock();
      if (lock == 1)
        payload_waiting_ = true;
      else if (lock == 0 || unanswered_ >= 3) {
        tried_start_ = true;
        start_payload();
      }
    }
    if (!r.ok()) {
      status_.error = sentence(plain(r.message().c_str()));
      return;
    }
    Json j = parse(r);
    if (!j)
      return;
    status_.error.clear();
    status_.version = str(j.get(), "version");
    status_.build = str(j.get(), "build");
    status_.platform = str(j.get(), "platform");
    status_.ip = str(j.get(), "ip");
    status_.port = static_cast<int>(num(j.get(), "port", 8790));
    status_.uptime = num(j.get(), "uptime");
    status_.jobs_active = static_cast<int>(num(j.get(), "jobs_active"));
    // An older build of ripper than the one RipperPS carries (or one
    // without a stamp): replaced by it, once nothing rips (the new copy
    // stops the old one itself). A newer one sent by hand stays.
    if (!replaced_ && status_.jobs_active == 0)
      if (const std::string mine = bundled_build();
          !mine.empty() && (!is_build_stamp(status_.build) || status_.build < mine)) {
        replaced_ = true;
        start_payload();
      }
    if (!was)  // back (or here for the first time): everything at once
    {
      settings_.known = false;
      catalog_.known = false;
      drives_timer_ = kDrivesEvery;
      jobs_timer_ = kJobsIdleEvery;
      refresh_stats();
    }
  });
}

void App::refresh_drives() {
  drives_timer_ = 0;
  drives_busy_ = true;
  poll_api_.get("/api/drives", [this](const Response &r) {
    drives_busy_ = false;
    Json j = parse(r);
    if (!r.ok() || !cJSON_IsArray(j.get()))
      return;
    std::vector<Drive> drives;
    const cJSON *o;
    cJSON_ArrayForEach(o, j.get()) drives.push_back(parse_drive(o));
    // The console's own drive first, then the others as the payload has them.
    std::stable_sort(drives.begin(), drives.end(),
                     [](const Drive &a, const Drive &b) { return a.internal() && !b.internal(); });
    drives_ = std::move(drives);
  });
}

void App::refresh_jobs() {
  jobs_timer_ = 0;
  jobs_busy_ = true;
  poll_api_.get("/api/jobs", [this](const Response &r) {
    jobs_busy_ = false;
    Json j = parse(r);
    if (!r.ok() || !cJSON_IsArray(j.get()))
      return;
    std::vector<Job> jobs;
    const cJSON *o;
    cJSON_ArrayForEach(o, j.get()) jobs.push_back(parse_job(o));
    // One that just ended: the stats moved on.
    for (const Job &now : jobs)
      if (now.ended())
        if (const Job *before = job(now.id); before && before->active())
          refresh_stats();
    jobs_ = std::move(jobs);
  });
}

void App::refresh_settings() {
  settings_busy_ = true;
  api_.get("/api/settings", [this](const Response &r) {
    settings_busy_ = false;
    Json j = parse(r);
    if (r.ok() && j)
      settings_ = parse_settings(j.get());
  });
}

void App::refresh_catalog() {
  catalog_busy_ = true;
  api_.get("/api/catalog", [this](const Response &r) {
    catalog_busy_ = false;
    Json j = parse(r);
    if (r.ok() && j)
      catalog_ = parse_catalog(j.get());
  });
}

void App::refresh_stats() {
  api_.get("/api/stats", [this](const Response &r) {
    Json j = parse(r);
    if (!r.ok() || !j)
      return;
    Stats s;
    s.known = true;
    s.discs = num(j.get(), "discs");
    s.failed = num(j.get(), "failed");
    s.bytes = num(j.get(), "bytes");
    s.seconds = num(j.get(), "seconds");
    s.sent = num(j.get(), "sent");
    s.since = num(j.get(), "since");
    const cJSON *o;
    cJSON_ArrayForEach(o, cJSON_GetObjectItemCaseSensitive(j.get(), "systems")) if (cJSON_IsNumber(o))
        s.systems.push_back({o->string, o->valuedouble});
    std::sort(s.systems.begin(), s.systems.end(), [](const auto &a, const auto &b) { return a.second > b.second; });
    stats_ = std::move(s);
  });
}

void App::refresh_pairing() {
  pair_timer_ = 0;
  pair_busy_ = true;
  api_.get("/api/romm/pair", [this](const Response &r) {
    pair_busy_ = false;
    Json j = parse(r);
    if (!r.ok() || !j)
      return;
    pairing_.status = str(j.get(), "status");
    pairing_.message = sentence(plain(str(j.get(), "message")));
    if (pairing_.status == "pending") {
      pairing_.user_code = str(j.get(), "user_code");
      pairing_.verification_url = str(j.get(), "verification_url");
    }
    if (const char *token = str(j.get(), "token"); token[0]) {
      pairing_.token = token;
      pairing_.status = "ok";
    }
  });
}

void App::refresh_rommps() {
  api_.get("/api/romm/rommps", [this](const Response &r) {
    Json j = parse(r);
    if (!r.ok() || !j)
      return;
    RommPS p;
    p.found = flag(j.get(), "found");
    p.url = str(j.get(), "url");
    p.username = str(j.get(), "username");
    p.tls_verify = flag(j.get(), "tls_verify");
    rommps_ = std::move(p);
  });
}

const Drive *App::drive(const std::string &id) const {
  for (const Drive &d : drives_)
    if (d.id == id)
      return &d;
  return nullptr;
}

const Job *App::job(int id) const {
  for (const Job &j : jobs_)
    if (j.id == id)
      return &j;
  return nullptr;
}

void App::action(const std::string &path, const std::string &body, const std::string &failed) {
  api_.post(path, body, [this, failed](const Response &r) {
    if (!r.ok())
      say(failed + ": " + r.message());
    refresh_now();
  });
}

void App::eject(const std::string &drive) {
  action("/api/drives/" + drive + "/eject", "{}", "Couldn't eject");
}

void App::identify(const std::string &drive) {
  action("/api/drives/" + drive + "/identify", "{}", "Couldn't read the disc again");
}

void App::lookup(const std::string &drive, const std::string &name) {
  action("/api/drives/" + drive + "/lookup", "{\"name\":" + json_string(name) + "}", "Couldn't look it up");
}

void App::rip(const Drive &d, const std::string &format, const std::string &destination, const std::string &name,
              const std::string &folder, const std::string &options, std::function<void(bool)> done) {
  std::string body = "{\"drive\":" + json_string(d.id) + ",\"disc_seq\":" + std::to_string(d.disc_seq) +
                     ",\"format\":" + json_string(format) + ",\"destination\":" + json_string(destination) +
                     ",\"name\":" + json_string(name) + ",\"folder\":" + json_string(folder) +
                     ",\"romm_platform\":" + json_string(d.suggestion.romm_platform) +
                     ",\"options\":" + (options.empty() ? "{}" : options) + "}";
  api_.post("/api/rip", body, [this, done](const Response &r) {
    if (!r.ok())
      say("Couldn't start the rip: " + r.message());
    refresh_now();
    if (done)
      done(r.ok());
  });
}

void App::cancel_job(int id) {
  action("/api/jobs/" + std::to_string(id) + "/cancel", "{}", "Couldn't cancel");
}

void App::retry_job(int id) {
  action("/api/jobs/" + std::to_string(id) + "/retry", "{}", "Couldn't try again");
}

void App::clear_jobs() {
  jobs_.erase(std::remove_if(jobs_.begin(), jobs_.end(), [](const Job &j) { return j.ended(); }), jobs_.end());
  action("/api/jobs/clear", "{}", "Couldn't clear the list");
}

void App::set_settings(const std::string &fields, const std::function<void(Settings &)> &apply) {
  apply(settings_);
  api_.post("/api/settings", "{" + fields + "}", [this](const Response &r) {
    if (!r.ok()) {
      say("Couldn't change the setting: " + r.message());
      settings_.known = false;  // the payload's, again
    }
  });
}

void App::set_key_file(const std::string &id, const std::string &path) {
  api_.post("/api/settings", "{\"keys\":{" + json_string(id) + ":" + json_string(path) + "}}", [this](const Response &r) {
    if (!r.ok())
      say("Couldn't change the key file: " + r.message());
    settings_.known = false;  // the payload's, with the new file's status
  });
}

void App::save_destinations(const std::vector<Destination> &list, std::function<void(bool)> done) {
  cJSON *arr = cJSON_CreateArray();
  for (const Destination &d : list)
    cJSON_AddItemToArray(arr, destination_json(d));
  cJSON *body = cJSON_CreateObject();
  cJSON_AddItemToObject(body, "destinations", arr);
  api_.post("/api/settings", print(body), [this, done](const Response &r) {
    if (!r.ok())
      say("Couldn't save the destinations: " + r.message());
    settings_.known = false;  // the payload's, with secrets as it keeps them
    if (done)
      done(r.ok());
  });
}

void App::test_destination(const Destination &d, std::function<void(bool, const std::string &)> done) {
  api_.post("/api/destinations/test", print(destination_json(d)), [done](const Response &r) {
    Json j = parse(r);
    const bool ok = r.ok() && j && flag(j.get(), "ok");
    std::string what = j ? plain(str(j.get(), "message")) : "";
    if (what.empty())
      what = r.ok() ? (ok ? "Works" : "Doesn't work") : r.message();
    what = sentence(what);
    done(ok, what);
  });
}

void App::save_profile(const std::string &system, const Profile &p) {
  cJSON *prof = cJSON_CreateObject();
  cJSON_AddStringToObject(prof, "format", p.format.c_str());
  cJSON_AddStringToObject(prof, "destination", p.destination.c_str());
  cJSON_AddStringToObject(prof, "folder", p.folder.c_str());
  cJSON_AddStringToObject(prof, "romm_platform", p.romm_platform.c_str());
  cJSON *opts = cJSON_AddObjectToObject(prof, "options");
  for (const auto &[k, v] : p.options)
    cJSON_AddStringToObject(opts, k.c_str(), v.c_str());
  cJSON *profiles = cJSON_CreateObject();
  cJSON_AddItemToObject(profiles, system.c_str(), prof);
  cJSON *body = cJSON_CreateObject();
  cJSON_AddItemToObject(body, "profiles", profiles);
  settings_.profiles[system] = p;
  api_.post("/api/settings", print(body), [this](const Response &r) {
    if (!r.ok()) {
      say("Couldn't save the profile: " + r.message());
      settings_.known = false;
    }
  });
}

void App::pair_start(const std::string &url, bool tls_verify) {
  pairing_ = Pairing{};
  pairing_.status = "starting";
  api_.post("/api/romm/pair",
            "{\"url\":" + json_string(url) + ",\"tls_verify\":" + (tls_verify ? "true" : "false") + "}",
            [this](const Response &r) {
              Json j = parse(r);
              if (!r.ok() || !j) {
                pairing_.status = "error";
                pairing_.message = sentence(plain(r.message().c_str()));
                return;
              }
              pairing_.status = str(j.get(), "status");
              pairing_.message = sentence(plain(str(j.get(), "message")));
              pairing_.user_code = str(j.get(), "user_code");
              pairing_.verification_url = str(j.get(), "verification_url");
              pair_timer_ = 0;
            });
}

void App::pair_cancel() {
  pairing_ = Pairing{};
  api_.post("/api/romm/pair/cancel", "{}", [](const Response &) {});
}

void App::refresh_games() {
  if (games_busy_)
    return;
  games_busy_ = true;
  api_.get("/api/games", [this](const Response &r) {
    games_busy_ = false;
    Json j = parse(r);
    if (!r.ok() || !cJSON_IsArray(j.get())) {
      if (!r.ok())
        say("Couldn't list the games: " + r.message());
      return;
    }
    std::vector<Game> games;
    const cJSON *o;
    cJSON_ArrayForEach(o, j.get())
        games.push_back({str(o, "title_id"), plain(str(o, "title")), str(o, "name"), str(o, "system"), num(o, "size"),
                         flag(o, "complete"), flag(o, "mounted"), flag(o, "game_running"), flag(o, "partial")});
    games_ = std::move(games);
  });
}

void App::dump(const Game &g, const std::string &destination, const std::string &name, const std::string &folder,
               const std::string &options, std::function<void(bool)> done) {
  std::string body = "{\"title_id\":" + json_string(g.title_id) +
                     ",\"format\":\"dump\",\"destination\":" + json_string(destination) +
                     ",\"name\":" + json_string(name) + ",\"folder\":" + json_string(folder) +
                     ",\"options\":" + (options.empty() ? "{}" : options) + "}";
  api_.post("/api/rip", body, [this, done](const Response &r) {
    if (!r.ok())
      say("Couldn't start the dump: " + r.message());
    refresh_now();
    if (done)
      done(r.ok());
  });
}

void App::refresh_drive_test() {
  test_timer_ = 0;
  test_busy_ = true;
  api_.get("/api/drivetest", [this](const Response &r) {
    test_busy_ = false;
    Json j = parse(r);
    if (!r.ok() || !j)
      return;
    DriveTest t;
    t.state = str(j.get(), "state");
    t.drive = str(j.get(), "drive");
    t.drive_label = plain(str(j.get(), "drive_label"));
    t.drive_name = plain(str(j.get(), "drive_name"));
    t.firmware = plain(str(j.get(), "firmware"));
    t.prompt = sentence(plain(str(j.get(), "prompt")));
    t.step = plain(str(j.get(), "step"));
    t.file = str(j.get(), "file");
    t.error = sentence(plain(str(j.get(), "error")));
    t.done = num(j.get(), "done");
    t.total = num(j.get(), "total");
    const cJSON *d = nullptr;
    cJSON_ArrayForEach(d, cJSON_GetObjectItemCaseSensitive(j.get(), "discs")) {
      TestDisc x;
      x.n = static_cast<int>(num(d, "n"));
      x.kind = plain(str(d, "kind"));
      x.title = plain(str(d, "title"));
      x.outcome = plain(str(d, "outcome"));
      x.result = str(d, "result");
      x.said = plain(str(d, "said"));
      x.signs = plain(str(d, "signs"));
      t.discs.push_back(std::move(x));
    }
    test_ = std::move(t);
  });
}

void App::start_drive_test(const std::string &drive) {
  api_.post("/api/drives/" + drive + "/drivetest", "{}", [this](const Response &r) {
    if (!r.ok()) {
      say("Couldn't start the drive test: " + r.message());
      return;
    }
    test_.state = "waiting";
    refresh_drive_test();
  });
}

void App::finish_drive_test() {
  api_.post("/api/drivetest/finish", "{}", [this](const Response &r) {
    if (!r.ok())
      say("Couldn't finish the drive test: " + r.message());
    refresh_drive_test();
  });
}

void App::discard_drive_test() {
  api_.post("/api/drivetest/discard", "{}", [this](const Response &) { refresh_drive_test(); });
}

void App::label_test_disc(int n, const std::string &said) {
  for (TestDisc &d : test_.discs)
    if (d.n == n)
      d.said = said;  // shown at once; the next refresh confirms it
  api_.post("/api/drivetest/disc", "{\"n\":" + std::to_string(n) + ",\"said\":" + json_string(said) + "}",
            [this](const Response &r) {
              if (!r.ok())
                say("Couldn't save that: " + r.message());
            });
}

}  // namespace ripper

extern "C" void ripper_set_renderer(void *renderer) {
  ripper::g_renderer = renderer;
}

extern "C" void *ripper_renderer() {
  return ripper::g_renderer;
}
