// PS4 fake packages, ported from LibOrbisPkg (OpenOrbis fork,
// https://github.com/OpenOrbis/LibOrbisPkg, LGPL-3.0, by maxton and
// contributors): PKG/PkgBuilder.cs, PkgWriter.cs, Entry.cs, Pkg.cs, Enums.cs,
// Rif/LicenseDat.cs, LicenseInfo.cs, PlayGo/ChunkDat.cs, Manifest.cs,
// SFO/ParamSfo.cs and Util/Crypto.cs, MersenneTwister.cs. Conveyed under the
// GPL-3.0 as LGPL-3.0 section 2 allows. The PFS images are in pfs.c.
//
// The bytes are what PkgTool's pkg_build writes for the same folder, except:
// generated sce_sys files a dump may carry (license.dat, playgo-chunk.dat...)
// are left out rather than added a second time; the PlayGo chunk hash table
// is sized to what it holds, where LibOrbisPkg's estimate falls short on
// multi-gigabyte games; and the dates come from param.sfo, not the clock.

#include "ripper/fpkg.h"

#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "pfs.h"
#include "ripper/aes.h"
#include "ripper/common.h"
#include "ripper/hash.h"
#include "ripper/log.h"
#include "ripper/rsa.h"
#include "ripper/util.h"

#define BODY_OFFSET 0x2000u
#define BODY_ALIGN 0x80000u
#define CHUNK 0x10000u
#define CONTENT_ID_LEN 36
#define DRM_PS4 0xFu
#define CONTENT_GD 0x1Au
#define SC_ENTRIES 6
#define MAX_SFO (1u << 20)

// Fake packages all use this passcode.
static const char PASSCODE[33] = "00000000000000000000000000000000";

enum {
  E_DIGESTS = 0x1,
  E_ENTRY_KEYS = 0x10,
  E_IMAGE_KEY = 0x20,
  E_GENERAL_DIGESTS = 0x80,
  E_METAS = 0x100,
  E_ENTRY_NAMES = 0x200,
  E_LICENSE_DAT = 0x400,
  E_LICENSE_INFO = 0x401,
  E_PSRESERVED_DAT = 0x409,
  E_PARAM_SFO = 0x1000,
  E_CHUNK_DAT = 0x1001,
  E_CHUNK_SHA = 0x1002,
  E_CHUNK_XML = 0x1003
};

static void set_err(char *err, size_t en, const char *fmt, const char *arg) {
  if (en) {
    snprintf(err, en, fmt, arg ? arg : "");
  }
}

static void put_be64(uint8_t *p, uint64_t v) {
  rip_put_be32(p, (uint32_t)(v >> 32));
  rip_put_be32(p + 4, (uint32_t)v);
}

static void sha256(const void *data, size_t len, uint8_t out[32]) {
  struct rip_sha256 h;

  rip_sha256_init(&h);
  rip_sha256_update(&h, data, len);
  rip_sha256_final(&h, out);
}

static uint64_t align_up(uint64_t v, uint64_t a) {
  return (v + a - 1) / a * a;
}

/* Entry names (Enums.cs EntryNames) */

static int digits2(const char *s, int *v) {
  if (s[0] < '0' || s[0] > '9' || s[1] < '0' || s[1] > '9') {
    return 0;
  }
  *v = (s[0] - '0') * 10 + (s[1] - '0');
  return 1;
}

// name is prefix, two digits, then suffix.
static int numbered(const char *name, const char *prefix, const char *suffix, int *v) {
  size_t pl = strlen(prefix);
  return !strncmp(name, prefix, pl) && digits2(name + pl, v) && !strcmp(name + pl + 2, suffix);
}

uint32_t fpkg_entry_id(const char *name) {
  static const struct {
    const char *name;
    uint32_t id;
  } fixed[] = {
      {"nptitle.dat", 0x402},
      {"npbind.dat", 0x403},
      {"selfinfo.dat", 0x404},
      {"imageinfo.dat", 0x406},
      {"target-deltainfo.dat", 0x407},
      {"origin-deltainfo.dat", 0x408},
      {"param.sfo", E_PARAM_SFO},
      {"pronunciation.xml", 0x1004},
      {"pronunciation.sig", 0x1005},
      {"pic1.png", 0x1006},
      {"pubtoolinfo.dat", 0x1007},
      {"app/playgo-chunk.dat", 0x1008},
      {"app/playgo-chunk.sha", 0x1009},
      {"app/playgo-manifest.xml", 0x100A},
      {"shareparam.json", 0x100B},
      {"shareoverlayimage.png", 0x100C},
      {"save_data.png", 0x100D},
      {"shareprivacyguardimage.png", 0x100E},
      {"icon0.png", 0x1200},
      {"pic0.png", 0x1220},
      {"snd0.at9", 0x1240},
      {"changeinfo/changeinfo.xml", 0x1260},
      {"icon0.dds", 0x1280},
      {"pic0.dds", 0x12A0},
      {"pic1.dds", 0x12C0},
  };
  // The package makes these itself; a dump's copies would be second entries.
  static const char *const made[] = {
      ".digests",    ".entry_keys",  ".image_key",     ".general_digests", ".metas",           ".entry_names",
      "license.dat", "license.info", "psreserved.dat", "playgo-chunk.dat", "playgo-chunk.sha", "playgo-manifest.xml",
  };
  size_t i;
  int a;
  int b;

  for (i = 0; i < RIP_ARRAY_LEN(made); i++) {
    if (!strcmp(name, made[i])) {
      return FPKG_ENTRY_DROP;
    }
  }
  for (i = 0; i < RIP_ARRAY_LEN(fixed); i++) {
    if (!strcmp(name, fixed[i].name)) {
      return fixed[i].id;
    }
  }
  if (numbered(name, "icon0_", ".png", &a) && a <= 30) {
    return 0x1201u + (uint32_t)a;
  }
  if (numbered(name, "icon0_", ".dds", &a) && a <= 30) {
    return 0x1281u + (uint32_t)a;
  }
  if (numbered(name, "pic1_", ".png", &a) && a <= 30) {
    return 0x1241u + (uint32_t)a;
  }
  if (numbered(name, "pic1_", ".dds", &a) && a <= 30) {
    return 0x12C1u + (uint32_t)a;
  }
  if (numbered(name, "changeinfo/changeinfo_", ".xml", &a) && a <= 30) {
    return 0x1261u + (uint32_t)a;
  }
  if (numbered(name, "trophy/trophy", ".trp", &a)) {
    return 0x1400u + (uint32_t)a;
  }
  if (numbered(name, "keymap_rp/0", ".png", &a) && a >= 1 && a <= 10) {
    return 0x1600u + (uint32_t)(a - 1);
  }
  if (!strncmp(name, "keymap_rp/", 10) && digits2(name + 10, &a) && a <= 30 && numbered(name + 12, "/0", ".png", &b) &&
      b >= 1 && b <= 10) {
    return 0x1610u + 16u * (uint32_t)a + (uint32_t)(b - 1);
  }
  return 0;
}

/* Key file */

struct key_name {
  const char *name;
  size_t off;
  size_t len;
  int optional;
};

#define KEY(n, field, opt) {n, offsetof(struct fpkg_keys, field), sizeof(((struct fpkg_keys *)0)->field), opt}

static const struct key_name KEY_NAMES[] = {
    {"PkgPublicKey0", offsetof(struct fpkg_keys, pkg_public[0]), 256, 0},
    {"PkgPublicKey1", offsetof(struct fpkg_keys, pkg_public[1]), 256, 0},
    {"PkgPublicKey2", offsetof(struct fpkg_keys, pkg_public[2]), 256, 0},
    {"PkgPublicKey3", offsetof(struct fpkg_keys, pkg_public[3]), 256, 0},
    {"PkgPublicKey4", offsetof(struct fpkg_keys, pkg_public[4]), 256, 0},
    {"PkgPublicKey5", offsetof(struct fpkg_keys, pkg_public[5]), 256, 0},
    {"PkgPublicKey6", offsetof(struct fpkg_keys, pkg_public[6]), 256, 0},
    KEY("FakeKeyModulus", fake_modulus, 0),
    KEY("DebugRifModulus", rif_modulus, 0),
    KEY("DebugRifPrivateExponent", rif_private, 0),
    KEY("RifDebugKey", rif_debug_key, 0),
    KEY("KeystoneHmacKey", keystone_hmac_key, 1),
    KEY("KeystoneMacData", keystone_mac_data, 1),
};

static int hex_val(char c) {
  if (c >= '0' && c <= '9') {
    return c - '0';
  }
  if (c >= 'a' && c <= 'f') {
    return c - 'a' + 10;
  }
  if (c >= 'A' && c <= 'F') {
    return c - 'A' + 10;
  }
  return -1;
}

static char *trim(char *s) {
  char *e;

  while (*s == ' ' || *s == '\t') {
    s++;
  }
  e = s + strlen(s);
  while (e > s && (e[-1] == ' ' || e[-1] == '\t' || e[-1] == '\r')) {
    *--e = '\0';
  }
  return s;
}

static int is_modulus(const uint8_t *n) {
  return n[0] != 0 && (n[255] & 1);
}

int fpkg_keys_load(const char *path, struct fpkg_keys *out, char *err, size_t en) {
  int seen[RIP_ARRAY_LEN(KEY_NAMES)];
  FILE *f = fopen(path, "r");
  char line[4096];
  int lineno = 0;
  size_t i;
  int rc = RIP_OK;

  memset(out, 0, sizeof(*out));
  memset(seen, 0, sizeof(seen));
  if (!f) {
    set_err(err, en, "can't read the key file", NULL);
    return RIP_ENOENT;
  }
  while (rc == RIP_OK && fgets(line, sizeof(line), f)) {
    char *hash = strchr(line, '#');
    char *nl = strchr(line, '\n');
    char *eq;
    char *name;
    char *val;
    const struct key_name *k = NULL;
    uint8_t *dst;
    size_t got = 0;
    int hi = -1;
    lineno++;
    if (!nl && !feof(f)) {
      if (en) {
        snprintf(err, en, "line %d of the key file is too long", lineno);
      }
      rc = RIP_ERR;
      break;
    }
    if (nl) {
      *nl = '\0';
    }
    if (hash) {
      *hash = '\0';
    }
    name = trim(line);
    if (!*name) {
      continue;
    }
    eq = strchr(name, '=');
    if (!eq) {
      if (en) {
        snprintf(err, en, "line %d of the key file has no \"=\"", lineno);
      }
      rc = RIP_ERR;
      break;
    }
    *eq = '\0';
    name = trim(name);
    val = eq + 1;
    for (i = 0; i < RIP_ARRAY_LEN(KEY_NAMES); i++) {
      if (!strcasecmp(name, KEY_NAMES[i].name)) {
        k = &KEY_NAMES[i];
        break;
      }
    }
    if (!k) {
      set_err(err, en, "the key file has an unknown name, %s", name);
      rc = RIP_ERR;
      break;
    }
    if (seen[i]) {
      set_err(err, en, "the key file has %s twice", k->name);
      rc = RIP_ERR;
      break;
    }
    seen[i] = 1;
    dst = (uint8_t *)out + k->off;
    for (; *val && rc == RIP_OK; val++) {
      int v;
      if (*val == ' ' || *val == '\t' || *val == '\r') {
        continue;
      }
      v = hex_val(*val);
      if (v < 0 || got >= k->len) {
        rc = RIP_ERR;
        break;
      }
      if (hi < 0) {
        hi = v;
      } else {
        dst[got++] = (uint8_t)(hi << 4 | v);
        hi = -1;
      }
    }
    if (rc != RIP_OK || got != k->len || hi >= 0) {
      if (en) {
        snprintf(err, en, "the key file's %s isn't %u bytes of hex", k->name, (unsigned)k->len);
      }
      rc = RIP_ERR;
    }
  }
  fclose(f);
  if (rc != RIP_OK) {
    return rc;
  }
  for (i = 0; i < RIP_ARRAY_LEN(KEY_NAMES); i++) {
    if (!seen[i] && !KEY_NAMES[i].optional) {
      set_err(err, en, "the key file has no %s", KEY_NAMES[i].name);
      return RIP_ENOENT;
    }
  }
  if (seen[RIP_ARRAY_LEN(KEY_NAMES) - 1] != seen[RIP_ARRAY_LEN(KEY_NAMES) - 2]) {
    set_err(err, en, "the key file needs both KeystoneHmacKey and KeystoneMacData, or neither", NULL);
    return RIP_ERR;
  }
  out->has_keystone_keys = seen[RIP_ARRAY_LEN(KEY_NAMES) - 1];
  for (i = 0; i < 7; i++) {
    if (!is_modulus(out->pkg_public[i])) {
      set_err(err, en, "the key file's %s isn't an RSA modulus", KEY_NAMES[i].name);
      return RIP_ERR;
    }
  }
  if (!is_modulus(out->fake_modulus) || !is_modulus(out->rif_modulus)) {
    set_err(err, en, "the key file's %s isn't an RSA modulus",
            is_modulus(out->fake_modulus) ? "DebugRifModulus" : "FakeKeyModulus");
    return RIP_ERR;
  }
  {
    uint8_t h[32];
    uint8_t sig[256];
    sha256("ripper", 6, h);
    if (rip_rsa_sign_sha256(out->rif_modulus, out->rif_private, 256, h, sig) != RIP_OK ||
        rip_rsa_verify_sha256(out->rif_modulus, 256, h, sig) != RIP_OK) {
      set_err(err, en, "the key file's DebugRifPrivateExponent doesn't go with its DebugRifModulus", NULL);
      return RIP_ERR;
    }
  }
  return RIP_OK;
}

/* LibOrbisPkg's key wrap: PKCS#1 v1.5 type 2 padding from a Mersenne
   Twister seeded by the key and message, so the output is deterministic. */

struct mt {
  uint32_t s[624];
  int i;
};

static void mt_seed(struct mt *m, uint32_t seed) {
  int i;

  m->s[0] = seed;
  for (i = 1; i < 624; i++) {
    m->s[i] = 1812433253u * (m->s[i - 1] ^ (m->s[i - 1] >> 30)) + (uint32_t)i;
  }
  m->i = 624;
}

static void mt_seed_array(struct mt *m, const uint32_t *key, int len) {
  int i = 1;
  int j = 0;
  int k;

  mt_seed(m, 19650218u);
  for (k = 624 > len ? 624 : len; k; k--) {
    m->s[i] = (m->s[i] ^ ((m->s[i - 1] ^ (m->s[i - 1] >> 30)) * 1664525u)) + key[j] + (uint32_t)j;
    i++;
    j++;
    if (i >= 624) {
      m->s[0] = m->s[623];
      i = 1;
    }
    if (j >= len) {
      j = 0;
    }
  }
  for (k = 623; k; k--) {
    m->s[i] = (m->s[i] ^ ((m->s[i - 1] ^ (m->s[i - 1] >> 30)) * 1566083941u)) - (uint32_t)i;
    i++;
    if (i >= 624) {
      m->s[0] = m->s[623];
      i = 1;
    }
  }
  m->s[0] = 0x80000000u;
}

static uint32_t mt_next(struct mt *m) {
  uint32_t y;

  if (m->i >= 624) {
    int k;
    for (k = 0; k < 624; k++) {
      y = (m->s[k] & 0x80000000u) | (m->s[(k + 1) % 624] & 0x7FFFFFFFu);
      m->s[k] = m->s[(k + 397) % 624] ^ (y >> 1) ^ ((y & 1) ? 0x9908B0DFu : 0);
    }
    m->i = 0;
  }
  y = m->s[m->i++];
  y ^= y >> 11;
  y ^= (y << 7) & 0x9D2C5680u;
  y ^= (y << 15) & 0xEFC60000u;
  y ^= y >> 18;
  return y;
}

static int wrap_key(const uint8_t modulus[256], const uint8_t msg[32], uint8_t out[256]) {
  static const uint8_t e[3] = {0x01, 0x00, 0x01};
  uint8_t buf[256 + 32];
  uint8_t h[32];
  uint8_t pad[256];
  uint32_t seed[8];
  struct mt m;
  int i;
  int k = 2;

  memcpy(buf, modulus, 256);
  memcpy(buf + 256, msg, 32);
  sha256(buf, sizeof(buf), h);
  sha256(h, 32, h);
  for (i = 0; i < 8; i++) {
    seed[i] = rip_be32(h + 4 * i);
  }
  mt_seed_array(&m, seed, 8);
  memset(pad, 0, sizeof(pad));
  pad[1] = 2;
  memcpy(pad + 224, msg, 32);
  while (k < 223) {
    uint8_t src[48];
    for (i = 0; i < 12; i++) {
      rip_put_be32(src + 4 * i, mt_next(&m));
    }
    sha256(src, sizeof(src), h);
    for (i = 0; i < 32 && k < 223; i++) {
      if (h[i]) {
        pad[k++] = h[i];
      }
    }
  }
  return rip_rsa_modexp(pad, e, sizeof(e), modulus, 256, out);
}

// Crypto.ComputeKeys: SHA-256(SHA-256(BE32 index) || SHA-256(content ID
// padded to 48) || passcode). Index 1 is EKPFS.
static void derive_key(const char *cid, uint32_t index, uint8_t out[32]) {
  uint8_t data[96];
  uint8_t be[4];
  uint8_t id[48];

  rip_put_be32(be, index);
  sha256(be, 4, data);
  memset(id, 0, sizeof(id));
  memcpy(id, cid, CONTENT_ID_LEN);
  sha256(id, sizeof(id), data + 32);
  memcpy(data + 64, PASSCODE, 32);
  sha256(data, sizeof(data), out);
}

/* param.sfo, as ParamSfo.cs reads and writes it */

#define SFO_UTF8_SPECIAL 0x0004
#define SFO_UTF8 0x0204
#define SFO_INT 0x0404

struct sfo_value {
  char key[128];
  uint16_t fmt;
  uint32_t len;  // bytes used: a UTF-8 string's NUL included
  uint32_t max;
  uint8_t *data;  // max bytes
};

struct sfo {
  struct sfo_value *v;
  size_t n;
};

static void sfo_free(struct sfo *s) {
  size_t i;

  for (i = 0; i < s->n; i++) {
    free(s->v[i].data);
  }
  free(s->v);
  s->v = NULL;
  s->n = 0;
}

static int sfo_parse(const uint8_t *b, size_t len, struct sfo *s) {
  uint32_t kt;
  uint32_t dt;
  uint32_t n;
  uint32_t i;

  memset(s, 0, sizeof(*s));
  if (len < 0x14 || memcmp(b, "\0PSF", 4) != 0) {
    return RIP_ERR;
  }
  kt = rip_le32(b + 8);
  dt = rip_le32(b + 12);
  n = rip_le32(b + 16);
  if (n > 1024 || 0x14 + (size_t)n * 16 > len || kt > len || dt > len) {
    return RIP_ERR;
  }
  s->v = calloc(n ? n : 1, sizeof(*s->v));
  if (!s->v) {
    return RIP_ENOMEM;
  }
  for (i = 0; i < n; i++) {
    const uint8_t *e = b + 0x14 + 16 * i;
    struct sfo_value *v = &s->v[s->n];
    size_t koff = (size_t)kt + rip_le16(e);
    uint32_t vlen = rip_le32(e + 4);
    size_t doff = (size_t)dt + rip_le32(e + 12);
    size_t kl;
    size_t take;
    if (koff >= len) {
      sfo_free(s);
      return RIP_ERR;
    }
    kl = strnlen((const char *)b + koff, len - koff);
    if (kl == 0 || kl >= sizeof(v->key) || koff + kl >= len) {
      sfo_free(s);
      return RIP_ERR;
    }
    memcpy(v->key, b + koff, kl);
    v->fmt = rip_le16(e + 2);
    v->max = rip_le32(e + 8);
    if (v->fmt == SFO_INT) {
      v->max = 4;
      v->len = 4;
      take = 4;
    } else if (v->fmt == SFO_UTF8) {
      take = vlen > 0 ? vlen - 1 : 0;
      v->len = (uint32_t)take + 1;
    } else if (v->fmt == SFO_UTF8_SPECIAL) {
      take = vlen;
      v->len = vlen;
    } else {
      sfo_free(s);
      return RIP_ERR;
    }
    if (v->len > v->max || v->max > MAX_SFO || doff + take > len) {
      sfo_free(s);
      return RIP_ERR;
    }
    v->data = calloc(1, v->max ? v->max : 1);
    if (!v->data) {
      sfo_free(s);
      return RIP_ENOMEM;
    }
    memcpy(v->data, b + doff, take);
    s->n++;
  }
  return RIP_OK;
}

static struct sfo_value *sfo_find(struct sfo *s, const char *key) {
  size_t i;

  for (i = 0; i < s->n; i++) {
    if (!strcmp(s->v[i].key, key)) {
      return &s->v[i];
    }
  }
  return NULL;
}

// A value as LibOrbisPkg prints it: integers as 0x%08x, strings as is.
static size_t sfo_text(struct sfo *s, const char *key, char *out, size_t cap) {
  struct sfo_value *v = sfo_find(s, key);
  size_t n;

  out[0] = '\0';
  if (!v) {
    return 0;
  }
  if (v->fmt == SFO_INT) {
    return (size_t)snprintf(out, cap, "0x%08x", rip_le32(v->data));
  }
  n = v->fmt == SFO_UTF8 ? v->len - 1 : v->len;
  if (n >= cap) {
    n = cap - 1;
  }
  memcpy(out, v->data, n);
  out[n] = '\0';
  return n;
}

static int sfo_set(struct sfo *s, const char *key, uint16_t fmt, const void *data, uint32_t len, uint32_t max) {
  struct sfo_value *v = sfo_find(s, key);

  if (!v) {
    struct sfo_value *nv = realloc(s->v, (s->n + 1) * sizeof(*nv));
    if (!nv) {
      return RIP_ENOMEM;
    }
    s->v = nv;
    v = &s->v[s->n++];
    memset(v, 0, sizeof(*v));
    snprintf(v->key, sizeof(v->key), "%s", key);
  }
  free(v->data);
  v->data = calloc(1, max);
  if (!v->data) {
    return RIP_ENOMEM;
  }
  v->fmt = fmt;
  v->len = len;
  v->max = max;
  memcpy(v->data, data, fmt == SFO_UTF8 ? len - 1 : len);
  return RIP_OK;
}

static int sfo_key_cmp(const void *a, const void *b) {
  const struct sfo_value *x = a;
  const struct sfo_value *y = b;
  return pfs_name_cmp(x->key, strlen(x->key), y->key, strlen(y->key));
}

// Sorted by key; the data table 4-byte aligned, each value its full size.
static uint8_t *sfo_write(struct sfo *s, size_t *out_len) {
  size_t kto = 0x14 + 16 * s->n;
  size_t kts = 0;
  size_t dts = 0;
  size_t dto;
  size_t ko = 0;
  size_t dof = 0;
  uint8_t *b;
  size_t i;

  qsort(s->v, s->n, sizeof(*s->v), sfo_key_cmp);
  for (i = 0; i < s->n; i++) {
    kts += strlen(s->v[i].key) + 1;
    dts += s->v[i].max;
  }
  dto = (kto + kts + 3) & ~(size_t)3;
  b = calloc(1, dto + dts);
  if (!b) {
    return NULL;
  }
  memcpy(b, "\0PSF", 4);
  rip_put_le32(b + 4, 0x101);
  rip_put_le32(b + 8, (uint32_t)kto);
  rip_put_le32(b + 12, (uint32_t)dto);
  rip_put_le32(b + 16, (uint32_t)s->n);
  for (i = 0; i < s->n; i++) {
    struct sfo_value *v = &s->v[i];
    uint8_t *e = b + 0x14 + 16 * i;
    size_t kl = strlen(v->key);
    rip_put_le16(e, (uint16_t)ko);
    rip_put_le16(e + 2, v->fmt);
    rip_put_le32(e + 4, v->len);
    rip_put_le32(e + 8, v->max);
    rip_put_le32(e + 12, (uint32_t)dof);
    memcpy(b + kto + ko, v->key, kl);
    memcpy(b + dto + dof, v->data, v->len);
    ko += kl + 1;
    dof += v->max;
  }
  *out_len = dto + dts;
  return b;
}

/* Dates */

static int64_t days_from_civil(int y, unsigned m, unsigned d) {
  int era;
  unsigned yoe;
  unsigned doy;
  unsigned doe;

  y -= m <= 2;
  era = (y >= 0 ? y : y - 399) / 400;
  yoe = (unsigned)(y - era * 400);
  doy = (153 * (m + (m > 2 ? (unsigned)-3 : 9)) + 2) / 5 + d - 1;
  doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
  return (int64_t)era * 146097 + (int64_t)doe - 719468;
}

static void civil_from_days(int64_t z, int *y, unsigned *m, unsigned *d) {
  int64_t era;
  unsigned doe;
  unsigned yoe;
  unsigned doy;
  unsigned mp;

  z += 719468;
  era = (z >= 0 ? z : z - 146096) / 146097;
  doe = (unsigned)(z - era * 146097);
  yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
  doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
  mp = (5 * doy + 2) / 153;
  *d = doy - (153 * mp + 2) / 5 + 1;
  *m = mp < 10 ? mp + 3 : mp - 9;
  *y = (int)(yoe + era * 400 + (*m <= 2));
}

static int parse_digits(const char *s, int n, unsigned *out) {
  int i;

  *out = 0;
  for (i = 0; i < n; i++) {
    if (s[i] < '0' || s[i] > '9') {
      return 0;
    }
    *out = *out * 10 + (unsigned)(s[i] - '0');
  }
  return 1;
}

// The package's date: c_date (and c_time) of the dump's PUBTOOLINFO, which is
// when the disc's own package was made; else param.sfo's modification time.
static int64_t package_time(struct sfo *s, const char *sfo_path, int *with_time) {
  char info[1024];
  const char *d;
  const char *t;
  unsigned y;
  unsigned mo;
  unsigned da;
  struct stat st;

  *with_time = 0;
  sfo_text(s, "PUBTOOLINFO", info, sizeof(info));
  d = strstr(info, "c_date=");
  if (d && parse_digits(d + 7, 4, &y) && parse_digits(d + 11, 2, &mo) && parse_digits(d + 13, 2, &da) && mo >= 1 &&
      mo <= 12 && da >= 1 && da <= 31) {
    int64_t secs = days_from_civil((int)y, mo, da) * 86400;
    unsigned hh;
    unsigned mm;
    unsigned ss;
    t = strstr(info, "c_time=");
    if (t && parse_digits(t + 7, 2, &hh) && parse_digits(t + 9, 2, &mm) && parse_digits(t + 11, 2, &ss) && hh < 24 &&
        mm < 60 && ss < 60) {
      *with_time = 1;
      secs += hh * 3600 + mm * 60 + ss;
    }
    return secs;
  }
  return stat(sfo_path, &st) == 0 ? (int64_t)st.st_mtime : 0;
}

/* The package */

struct entry {
  uint32_t id;
  const char *name;  // NULL for unnamed entries
  uint32_t len;      // bytes stored
  uint32_t room;     // bytes laid out (more than len only for the chunk hashes)
  uint32_t flags1;
  uint32_t flags2;
  uint32_t offset;
  uint32_t name_off;
  uint8_t *data;     // generated entries
  const char *path;  // entries copied from the folder
  uint8_t digest[32];
};

static uint8_t *meta_record(const struct entry *e, uint8_t rec[32]) {
  memset(rec, 0, 32);
  rip_put_be32(rec, e->id);
  rip_put_be32(rec + 4, e->name_off);
  rip_put_be32(rec + 8, e->flags1);
  rip_put_be32(rec + 12, e->flags2);
  rip_put_be32(rec + 16, e->offset);
  rip_put_be32(rec + 20, e->len);
  return rec;
}

// Entry.WriteEncrypted: AES-128-CBC under SHA-256(meta record || key[i]).
static void encrypt_entry(const struct entry *e, const char *cid) {
  uint8_t buf[64];
  uint8_t ik[32];
  uint8_t iv[16];
  struct rip_aes128 aes;

  meta_record(e, buf);
  derive_key(cid, (e->flags2 >> 12) & 0xF, buf + 32);
  sha256(buf, sizeof(buf), ik);
  memcpy(iv, ik, 16);
  rip_aes128_init_enc(&aes, ik + 16);
  rip_aes128_cbc_encrypt(&aes, iv, e->data, e->len);
}

static const char MANIFEST[] =
    "\xEF\xBB\xBF<?xml version=\"1.0\" encoding=\"utf-8\" standalone=\"yes\"?>\r\n"
    "<psproject fmt=\"playgo-manifest\" version=\"0990\">\r\n"
    "  <volume>\r\n"
    "    <chunk_info chunk_count=\"1\" scenario_count=\"1\">\r\n"
    "      <scenarios default_id=\"0\">\r\n"
    "        <scenario id=\"0\" type=\"sp\" initial_chunk_count=\"1\" label=\"Scenario #0\">0</scenario>\r\n"
    "      </scenarios>\r\n"
    "    </chunk_info>\r\n"
    "  </volume>\r\n"
    "</psproject>\r\n";

// PlayGo/ChunkDat.cs: one chunk, one scenario.
static void chunk_dat(uint8_t *b, const char *cid, uint64_t package_size, uint64_t inner_size) {
  static const uint32_t tables[16] = {256, 32, 288, 2, 304, 9, 320, 16, 352, 32, 384, 2, 400, 12, 336, 16};
  int i;

  memset(b, 0, 416);
  memcpy(b, "plgo", 4);
  rip_put_le16(b + 8, 1);
  rip_put_le16(b + 0xA, 1);
  rip_put_le16(b + 0xC, 1);
  rip_put_le16(b + 0xE, 1);
  rip_put_le32(b + 0x10, 416);
  rip_put_le16(b + 0x16, 1);
  memset(b + 0x20, 0xFF, 32);
  memcpy(b + 0x40, cid, CONTENT_ID_LEN);
  for (i = 0; i < 16; i++) {
    rip_put_le32(b + 0xC0 + 4 * i, tables[i]);
  }
  b[0x100] = 0x80;
  b[0x102] = 3;
  rip_put_le16(b + 0x10E, 1);
  memset(b + 0x110, 0xFF, 8);
  memcpy(b + 0x130, "Chunk #0", 8);
  rip_put_le32(b + 0x148, (uint32_t)package_size);
  rip_put_le32(b + 0x14C, (uint32_t)(package_size >> 32));
  rip_put_le32(b + 0x158, (uint32_t)inner_size);
  rip_put_le32(b + 0x15C, (uint32_t)(inner_size >> 32));
  b[0x160] = 1;
  rip_put_le16(b + 0x174, 1);
  rip_put_le16(b + 0x176, 1);
  memcpy(b + 0x190, "Scenario #0", 11);
}

// Rif/LicenseDat.cs: a debug RIF, its secret under rif_debug_key, signed.
static int license_dat(uint8_t *b, const char *cid, const struct fpkg_keys *k) {
  uint8_t id[48];
  uint8_t h[32];
  uint8_t iv[16];
  struct rip_aes128 aes;

  memset(b, 0, 0x400);
  memcpy(b, "RIF\0", 4);
  rip_put_be16(b + 4, 1);
  rip_put_be16(b + 6, 0xFFFF);
  put_be64(b + 0x10, 1364222275u);
  put_be64(b + 0x18, INT64_MAX);
  memcpy(b + 0x20, cid, CONTENT_ID_LEN);
  rip_put_be16(b + 0x50, 0x200);  // Debug_0
  rip_put_be16(b + 0x52, DRM_PS4);
  rip_put_be16(b + 0x54, CONTENT_GD);
  rip_put_be16(b + 0x56, 3);  // the SKU flag games need
  rip_put_be32(b + 0x64, 1);
  memset(id, 0, sizeof(id));
  memcpy(id, cid, CONTENT_ID_LEN);
  sha256(id, sizeof(id), h);
  memcpy(b + 0x260, h, 16);
  memcpy(b + 0x270, h + 16, 16);
  memcpy(iv, h, 16);
  rip_aes128_init_enc(&aes, k->rif_debug_key);
  rip_aes128_cbc_encrypt(&aes, iv, b + 0x270, 144);
  sha256(b, 0x300, h);
  return rip_rsa_sign_sha256(k->rif_modulus, k->rif_private, 256, h, b + 0x300);
}

static int entry_name_cmp(const struct entry *a, const struct entry *b) {
  if (!a->name || !b->name) {
    return (a->name != NULL) - (b->name != NULL);
  }
  return pfs_name_cmp(a->name, strlen(a->name), b->name, strlen(b->name));
}

// Writes a folder file as an entry, hashing it on the way.
static int copy_entry(int fd, struct entry *e, uint8_t *buf, size_t cap, char *err, size_t en) {
  struct rip_sha256 h;
  uint32_t done = 0;
  int in = open(e->path, O_RDONLY);
  int rc = RIP_OK;

  if (in < 0) {
    set_err(err, en, "can't read %s", e->name);
    return RIP_ERR;
  }
  rip_sha256_init(&h);
  while (done < e->len) {
    size_t want = e->len - done < cap ? e->len - done : cap;
    if (rip_read_full(in, buf, want) != (long)want) {
      set_err(err, en, "a file in the game folder changed while it was read", NULL);
      rc = RIP_ERR;
      break;
    }
    if (rip_seek_write(fd, (uint64_t)e->offset + done, buf, want) != 0) {
      set_err(err, en, "can't write the package", NULL);
      rc = RIP_ERR;
      break;
    }
    rip_sha256_update(&h, buf, want);
    done += (uint32_t)want;
  }
  close(in);
  rip_sha256_final(&h, e->digest);
  return rc;
}

#define MAX_ENTRIES 512

struct build {
  char cid[CONTENT_ID_LEN + 1];
  struct entry e[MAX_ENTRIES];
  size_t n;
  uint8_t *sfo_bytes;
  size_t sfo_len;
  struct sfo sfo;
  struct pfs_image *pfs;
  uint8_t *chunk_sha;
  uint8_t *names;
  uint32_t names_len;
};

static struct entry *add_entry(struct build *b, uint32_t id, const char *name, uint32_t len) {
  struct entry *e;

  if (b->n >= MAX_ENTRIES) {
    return NULL;
  }
  e = &b->e[b->n++];
  memset(e, 0, sizeof(*e));
  e->id = id;
  e->name = name;
  e->len = e->room = len;
  switch (id) {
  case E_DIGESTS:
  case E_ENTRY_NAMES:
    e->flags1 = 0x40000000u;
    break;
  case E_ENTRY_KEYS:
  case E_GENERAL_DIGESTS:
  case E_METAS:
    e->flags1 = 0x60000000u;
    break;
  case E_IMAGE_KEY:
    e->flags1 = 0xE0000000u;
    e->flags2 = 3u << 12;
    break;
  case E_LICENSE_DAT:
    e->flags1 = 0x80000000u;
    e->flags2 = 3u << 12;
    break;
  case E_LICENSE_INFO:
    e->flags1 = 0x80000000u;
    e->flags2 = 2u << 12;
    break;
  }
  return e;
}

static struct entry *find_entry(struct build *b, uint32_t id) {
  size_t i;

  for (i = 0; i < b->n; i++) {
    if (b->e[i].id == id) {
      return &b->e[i];
    }
  }
  return NULL;
}

// Offsets in body order, each 16-byte aligned; returns the body's end.
static uint64_t lay_out(struct build *b) {
  uint64_t off = BODY_OFFSET;
  size_t i;

  for (i = 0; i < b->n; i++) {
    b->e[i].offset = (uint32_t)off;
    off = align_up(off + b->e[i].room, 16);
  }
  return off;
}

static int read_small(const char *path, uint8_t **out, size_t *len) {
  int fd = open(path, O_RDONLY);
  struct stat st;
  int rc = RIP_ERR;

  *out = NULL;
  if (fd < 0) {
    return RIP_ERR;
  }
  if (fstat(fd, &st) == 0 && st.st_size > 0 && st.st_size <= (off_t)MAX_SFO) {
    *len = (size_t)st.st_size;
    *out = malloc(*len);
    if (*out && rip_read_full(fd, *out, *len) == (long)*len) {
      rc = RIP_OK;
    } else {
      free(*out);
      *out = NULL;
    }
  }
  close(fd);
  return rc;
}

static int valid_cid(const char *cid) {
  size_t i;

  if (strlen(cid) != CONTENT_ID_LEN) {
    return 0;
  }
  for (i = 0; i < CONTENT_ID_LEN; i++) {
    if (cid[i] < 0x21 || cid[i] > 0x7E) {
      return 0;
    }
  }
  return 1;
}

static int build_pkg(struct build *b, const char *folder, int fd, const struct fpkg_keys *keys,
                     iso_progress_fn progress, void *ud, char *err, size_t en) {
  const struct pfs_entry *sfo_entry = NULL;
  char category[64];
  char title_id[64];
  char version[64];
  char info[600];
  uint8_t *raw = NULL;
  size_t raw_len = 0;
  uint8_t ekpfs[32];
  uint8_t dk[7][32];
  uint8_t image_digest[32];
  uint8_t signed_digest[32];
  uint8_t hdr[0x1000];
  uint8_t h[32];
  uint8_t *buf = NULL;
  struct entry *e;
  uint64_t outer;
  uint64_t body_end;
  uint64_t body_size;
  uint64_t pkg_size;
  uint64_t est;
  uint32_t chunk_len;
  int64_t when;
  int with_time;
  size_t i;
  size_t j;
  int rc;

  rc = pfs_scan(folder, &b->pfs, err, en);
  if (rc != RIP_OK) {
    return rc;
  }
  for (i = 0; i < pfs_entry_count(b->pfs); i++) {
    if (pfs_entry_at(b->pfs, i)->id == E_PARAM_SFO) {
      sfo_entry = pfs_entry_at(b->pfs, i);
    }
  }
  if (!sfo_entry) {
    set_err(err, en, "the game folder has no sce_sys/param.sfo", NULL);
    return RIP_ENOENT;
  }
  if (read_small(sfo_entry->path, &raw, &raw_len) != RIP_OK) {
    set_err(err, en, "can't read sce_sys/param.sfo", NULL);
    return RIP_ERR;
  }
  rc = sfo_parse(raw, raw_len, &b->sfo);
  free(raw);
  if (rc != RIP_OK) {
    set_err(err, en, "sce_sys/param.sfo isn't a valid param.sfo", NULL);
    return RIP_ERR;
  }
  sfo_text(&b->sfo, "CONTENT_ID", b->cid, sizeof(b->cid));
  sfo_text(&b->sfo, "CATEGORY", category, sizeof(category));
  sfo_text(&b->sfo, "TITLE_ID", title_id, sizeof(title_id));
  if (!sfo_text(&b->sfo, "APP_VER", version, sizeof(version))) {
    sfo_text(&b->sfo, "VERSION", version, sizeof(version));
  }
  if (!valid_cid(b->cid)) {
    set_err(err, en, "param.sfo has no 36-character CONTENT_ID", NULL);
    return RIP_ERR;
  }
  if (strncmp(category, "gd", 2) != 0) {
    set_err(err, en, "param.sfo's CATEGORY is %s, not a game (gd)", category);
    return RIP_ERR;
  }
  if (strncmp(b->cid + 7, title_id, 9) != 0) {
    RIP_WARN("fpkg: content ID %s doesn't hold title ID %s", b->cid, title_id);
  }
  RIP_INFO("fpkg: %s (%s, version %s)", b->cid, title_id, version);

  // The keystone ties saves to the game; keep the dump's.
  if (!pfs_has_keystone(b->pfs)) {
    uint8_t ks[96];
    uint8_t fp[32];
    if (!keys->has_keystone_keys) {
      set_err(err, en, "the game folder has no sce_sys/keystone, and the key file has no KeystoneHmacKey to make one",
              NULL);
      return RIP_ERR;
    }
    memset(ks, 0, 32);
    memcpy(ks, "keystone\x02\x00\x01\x00", 12);
    rip_hmac_sha256(keys->keystone_hmac_key, 32, PASSCODE, 32, fp);
    memcpy(ks + 32, fp, 32);
    rip_hmac_sha256(keys->keystone_mac_data, 32, ks, 64, ks + 64);
    if (pfs_add_keystone(b->pfs, ks) != RIP_OK) {
      set_err(err, en, "out of memory", NULL);
      return RIP_ENOMEM;
    }
  }

  when = package_time(&b->sfo, sfo_entry->path, &with_time);
  rc = pfs_plan(b->pfs, when, err, en);
  if (rc != RIP_OK) {
    return rc;
  }
  outer = pfs_outer_size(b->pfs);

  // PUBTOOLINFO and PUBTOOLVER as PkgBuilder sets them. img0_l0_size is from
  // a body of 512 KiB, which PkgBuilder assumes at that point.
  {
    int y;
    unsigned mo;
    unsigned da;
    int64_t days = when >= 0 ? when / 86400 : (when - 86399) / 86400;
    int64_t sec = when - days * 86400;
    uint8_t ver[4];
    int n;
    civil_from_days(days, &y, &mo, &da);
    n = snprintf(info, sizeof(info), "c_date=%04d%02u%02u", y, mo, da);
    if (with_time) {
      n += snprintf(info + n, sizeof(info) - (size_t)n, ",c_time=%02u%02u%02u", (unsigned)(sec / 3600),
                    (unsigned)(sec / 60 % 60), (unsigned)(sec % 60));
    }
    snprintf(info + n, sizeof(info) - (size_t)n,
             ",img0_l0_size=%llu,img0_l1_size=0,img0_sc_ksize=512,img0_pc_ksize=832",
             (unsigned long long)((BODY_ALIGN + outer + 0xFFFFF) / (1024 * 1024)));
    rip_put_le32(ver, 0x02890000);
    if (sfo_set(&b->sfo, "PUBTOOLINFO", SFO_UTF8, info, (uint32_t)strlen(info) + 1, 0x200) != RIP_OK ||
        sfo_set(&b->sfo, "PUBTOOLVER", SFO_INT, ver, 4, 4) != RIP_OK ||
        !(b->sfo_bytes = sfo_write(&b->sfo, &b->sfo_len))) {
      set_err(err, en, "out of memory", NULL);
      return RIP_ENOMEM;
    }
  }

  // Entries in PkgBuilder's body order.
  add_entry(b, E_ENTRY_KEYS, NULL, 0x800);
  add_entry(b, E_IMAGE_KEY, NULL, 0x100);
  add_entry(b, E_GENERAL_DIGESTS, NULL, 0x180);
  add_entry(b, E_METAS, NULL, 0);
  add_entry(b, E_DIGESTS, NULL, 0);
  add_entry(b, E_ENTRY_NAMES, NULL, 0);
  add_entry(b, E_CHUNK_DAT, "playgo-chunk.dat", 416);
  add_entry(b, E_CHUNK_SHA, "playgo-chunk.sha", 0);
  add_entry(b, E_CHUNK_XML, "playgo-manifest.xml", sizeof(MANIFEST) - 1);
  add_entry(b, E_LICENSE_DAT, NULL, 0x400);
  add_entry(b, E_LICENSE_INFO, NULL, 0x200);
  add_entry(b, E_PARAM_SFO, "param.sfo", (uint32_t)b->sfo_len);
  add_entry(b, E_PSRESERVED_DAT, NULL, 0x2000);
  for (i = 0; i < pfs_entry_count(b->pfs); i++) {
    const struct pfs_entry *pe = pfs_entry_at(b->pfs, i);
    if (pe->id == E_PARAM_SFO) {
      continue;
    }
    if (pe->size > 0x40000000u) {
      set_err(err, en, "sce_sys/%s is too big for a package entry", pe->name);
      return RIP_ERR;
    }
    e = add_entry(b, pe->id, pe->name, (uint32_t)pe->size);
    if (!e) {
      set_err(err, en, "sce_sys has too many files", NULL);
      return RIP_ERR;
    }
    e->path = pe->path;
  }
  find_entry(b, E_METAS)->len = find_entry(b, E_METAS)->room = (uint32_t)b->n * 32;
  find_entry(b, E_DIGESTS)->len = find_entry(b, E_DIGESTS)->room = (uint32_t)b->n * 32;

  // Names: an empty one, then each named entry's in name order.
  {
    struct entry *sorted[MAX_ENTRIES];
    size_t cap = 1;
    for (i = 0; i < b->n; i++) {
      struct entry *x = &b->e[i];
      for (j = i; j > 0 && entry_name_cmp(sorted[j - 1], x) > 0; j--) {
        sorted[j] = sorted[j - 1];
      }
      sorted[j] = x;
      cap += x->name ? strlen(x->name) + 1 : 0;
    }
    b->names = calloc(1, cap);
    if (!b->names) {
      set_err(err, en, "out of memory", NULL);
      return RIP_ENOMEM;
    }
    b->names_len = 1;
    for (i = 0; i < b->n; i++) {
      if (sorted[i]->name) {
        size_t l = strlen(sorted[i]->name);
        sorted[i]->name_off = b->names_len;
        memcpy(b->names + b->names_len, sorted[i]->name, l);
        b->names_len += (uint32_t)l + 1;
      }
    }
    e = find_entry(b, E_ENTRY_NAMES);
    e->len = e->room = b->names_len;
  }

  // The PlayGo chunk hashes: 4 bytes per 64 KiB of the whole package, which
  // depends on the body they're in. PkgBuilder's estimate, from the entries
  // so far with the meta table counted up to this entry; grown when short.
  e = find_entry(b, E_CHUNK_SHA);
  est = BODY_OFFSET;
  for (i = 0; i < b->n; i++) {
    uint32_t len = b->e[i].id == E_METAS ? 8 * 32 : b->e[i].len;
    est += align_up(len, 16);
  }
  est = align_up(est, BODY_ALIGN) + outer;
  est += (est + 16) / CHUNK * 4;
  e->room = (uint32_t)(est / CHUNK * 4);
  for (;;) {
    body_end = lay_out(b);
    body_size = align_up(body_end, BODY_ALIGN) - BODY_OFFSET;
    pkg_size = BODY_OFFSET + body_size + outer;
    if (pkg_size / CHUNK * 4 > UINT32_MAX / 2) {
      set_err(err, en, "the game is too big for a package", NULL);
      return RIP_ERR;
    }
    chunk_len = (uint32_t)(pkg_size / CHUNK * 4);
    if (chunk_len <= e->room) {
      break;
    }
    e->room = chunk_len;
  }
  e->len = chunk_len;
  b->chunk_sha = calloc(1, chunk_len);
  buf = malloc(1u << 20);
  if (!b->chunk_sha || !buf) {
    free(buf);
    set_err(err, en, "out of memory", NULL);
    return RIP_ENOMEM;
  }

  if (ftruncate(fd, (off_t)pkg_size) != 0) {
    free(buf);
    set_err(err, en, "can't write the package", NULL);
    return RIP_ERR;
  }

  // The outer PFS first: the header's digests need it.
  for (i = 0; i < 7; i++) {
    derive_key(b->cid, (uint32_t)i, dk[i]);
  }
  memcpy(ekpfs, dk[1], 32);
  rc = pfs_write(b->pfs, fd, BODY_OFFSET + body_size, ekpfs, b->chunk_sha + 4 * ((BODY_OFFSET + body_size) / CHUNK),
                 image_digest, signed_digest, progress, ud, err, en);
  if (rc != RIP_OK) {
    free(buf);
    return rc;
  }

  // The header, but for the hashes of the body.
  memset(hdr, 0, sizeof(hdr));
  memcpy(hdr,
         "\x7F"
         "CNT",
         4);
  rip_put_be32(hdr + 0x04, 1);
  rip_put_be32(hdr + 0x0C, 0xF);
  rip_put_be32(hdr + 0x10, (uint32_t)b->n);
  rip_put_be16(hdr + 0x14, SC_ENTRIES);
  rip_put_be16(hdr + 0x16, (uint16_t)b->n);
  rip_put_be32(hdr + 0x18, find_entry(b, E_METAS)->offset);
  rip_put_be32(hdr + 0x1C, 0x800 + 0x100 + 0x180 + 2 * (uint32_t)b->n * 32);
  put_be64(hdr + 0x20, BODY_OFFSET);
  put_be64(hdr + 0x28, body_size);
  memcpy(hdr + 0x40, b->cid, CONTENT_ID_LEN);
  rip_put_be32(hdr + 0x70, DRM_PS4);
  rip_put_be32(hdr + 0x74, CONTENT_GD);
  rip_put_be32(hdr + 0x78, 0x0A000000u);
  rip_put_be32(hdr + 0x7C, (uint32_t)(BODY_OFFSET + body_size));
  rip_put_be32(hdr + 0x80, 0x20161020u);
  rip_put_be32(hdr + 0x84, 0x01738551u);
  rip_put_be32(hdr + 0x9C, 1);
  rip_put_be32(hdr + 0x400, 1);
  rip_put_be32(hdr + 0x404, 1);
  put_be64(hdr + 0x408, 0x80000000000003CCull);
  put_be64(hdr + 0x410, BODY_OFFSET + body_size);
  put_be64(hdr + 0x418, outer);
  put_be64(hdr + 0x428, pkg_size);
  put_be64(hdr + 0x430, pkg_size);
  rip_put_be32(hdr + 0x438, 0x10000);
  rip_put_be32(hdr + 0x43C, 0xD0000);
  memcpy(hdr + 0x440, image_digest, 32);
  memcpy(hdr + 0x460, signed_digest, 32);

  // Generated entries.
  for (i = 0; i < b->n; i++) {
    e = &b->e[i];
    if (!e->path && !(e->data = calloc(1, e->len ? e->len : 1))) {
      free(buf);
      set_err(err, en, "out of memory", NULL);
      return RIP_ENOMEM;
    }
  }
  {
    uint8_t *k = find_entry(b, E_ENTRY_KEYS)->data;
    uint8_t id[48];
    memset(id, 0, sizeof(id));
    memcpy(id, b->cid, CONTENT_ID_LEN);
    sha256(id, sizeof(id), k);
    for (i = 0; i < 7 && rc == RIP_OK; i++) {
      sha256(dk[i], 32, h);
      for (j = 0; j < 32; j++) {
        k[32 + 32 * i + j] = h[j] ^ dk[i][j];
      }
      // Key 0 wraps the passcode itself.
      rc = wrap_key(keys->pkg_public[i], i == 0 ? (const uint8_t *)PASSCODE : dk[i], k + 256 + 256 * i);
    }
  }
  if (rc == RIP_OK) {
    rc = wrap_key(keys->fake_modulus, ekpfs, find_entry(b, E_IMAGE_KEY)->data);
  }
  if (rc == RIP_OK) {
    rc = license_dat(find_entry(b, E_LICENSE_DAT)->data, b->cid, keys);
  }
  if (rc != RIP_OK) {
    free(buf);
    set_err(err, en, "a key in the key file doesn't work", NULL);
    return RIP_ERR;
  }
  {
    uint8_t *li = find_entry(b, E_LICENSE_INFO)->data;
    memcpy(li, b->cid, CONTENT_ID_LEN);
    rip_put_be32(li + 0x44, CONTENT_GD);
    rip_put_be32(li + 0x4C, 1);
  }
  memcpy(find_entry(b, E_ENTRY_NAMES)->data, b->names, b->names_len);
  chunk_dat(find_entry(b, E_CHUNK_DAT)->data, b->cid, pkg_size, pfs_inner_size(b->pfs));
  memcpy(find_entry(b, E_CHUNK_SHA)->data, b->chunk_sha, chunk_len);
  memcpy(find_entry(b, E_CHUNK_XML)->data, MANIFEST, sizeof(MANIFEST) - 1);
  memcpy(find_entry(b, E_PARAM_SFO)->data, b->sfo_bytes, b->sfo_len);

  // General digests (Pkg.CalcGeneralDigests).
  {
    uint8_t *g = find_entry(b, E_GENERAL_DIGESTS)->data;
    char major[1024];
    char v[256];
    size_t n = 0;
    struct rip_sha256 sh;
    uint8_t content[36 + 12 + 8 + 64];
    n += (size_t)snprintf(major + n, sizeof(major) - n, "ATTRIBUTE");
    sfo_text(&b->sfo, "ATTRIBUTE", v, sizeof(v));
    n += (size_t)snprintf(major + n, sizeof(major) - n, "%s", v);
    if (sfo_find(&b->sfo, "ATTRIBUTE2")) {
      sfo_text(&b->sfo, "ATTRIBUTE2", v, sizeof(v));
      n += (size_t)snprintf(major + n, sizeof(major) - n, "ATTRIBUTE2%s", v);
    }
    sfo_text(&b->sfo, "CATEGORY", v, sizeof(v));
    n += (size_t)snprintf(major + n, sizeof(major) - n, "CATEGORY%s", v);
    sfo_text(&b->sfo, "FORMAT", v, sizeof(v));
    n += (size_t)snprintf(major + n, sizeof(major) - n, "FORMAT%s", v);
    sfo_text(&b->sfo, "PUBTOOLVER", v, sizeof(v));
    n += (size_t)snprintf(major + n, sizeof(major) - n, "PUBTOOLVER%s", v);
    if (n >= sizeof(major)) {
      n = sizeof(major) - 1;
    }

    rip_put_be16(g, 0xD256);
    rip_put_be16(g + 2, 0x0100);
    rip_put_be32(g + 0x1C, 0x6E);        // content, game, header, major param, param
    memcpy(g + 0x40, image_digest, 32);  // game
    rip_sha256_init(&sh);                // header
    rip_sha256_update(&sh, hdr, 0x40);
    rip_sha256_update(&sh, hdr + 0x400, 0x80);
    rip_sha256_final(&sh, g + 0x60);
    sha256(major, n, g + 0xA0);                  // major param
    sha256(b->sfo_bytes, b->sfo_len, g + 0xC0);  // param
    memset(content, 0, sizeof(content));
    memcpy(content, b->cid, CONTENT_ID_LEN);
    rip_put_be32(content + 48, DRM_PS4);
    rip_put_be32(content + 52, CONTENT_GD);
    memcpy(content + 56, image_digest, 32);
    memcpy(content + 88, g + 0xA0, 32);
    sha256(content, sizeof(content), g + 0x20);  // content
  }

  // The meta table, sorted by id.
  {
    struct entry *byid[MAX_ENTRIES];
    uint8_t *m = find_entry(b, E_METAS)->data;
    for (i = 0; i < b->n; i++) {
      struct entry *x = &b->e[i];
      for (j = i; j > 0 && byid[j - 1]->id > x->id; j--) {
        byid[j] = byid[j - 1];
      }
      byid[j] = x;
    }
    for (i = 0; i < b->n; i++) {
      meta_record(byid[i], m + 32 * i);
    }

    // Encrypt, write and hash every entry; the digest table last.
    for (i = 0; i < b->n && rc == RIP_OK; i++) {
      e = &b->e[i];
      if (e->path) {
        rc = copy_entry(fd, e, buf, 1u << 20, err, en);
        continue;
      }
      if (e->flags1 & 0x80000000u) {
        encrypt_entry(e, b->cid);
      }
      if (e->id != E_DIGESTS) {
        sha256(e->data, e->len, e->digest);
        if (rip_seek_write(fd, e->offset, e->data, e->len) != 0) {
          set_err(err, en, "can't write the package", NULL);
          rc = RIP_ERR;
        }
      }
    }
    if (rc != RIP_OK) {
      free(buf);
      return rc;
    }
    e = find_entry(b, E_DIGESTS);
    for (i = 1; i < b->n; i++) {
      memcpy(e->data + 32 * i, byid[i]->digest, 32);
    }
    if (rip_seek_write(fd, e->offset, e->data, e->len) != 0) {
      free(buf);
      set_err(err, en, "can't write the package", NULL);
      return RIP_ERR;
    }
    sha256(e->data, e->len, hdr + 0x140);
  }

  // Body digest, read back as written, padding included.
  {
    struct rip_sha256 sh;
    uint64_t done = 0;
    rip_sha256_init(&sh);
    while (done < body_size) {
      size_t len = body_size - done < (1u << 20) ? (size_t)(body_size - done) : (1u << 20);
      if (rip_seek_read(fd, BODY_OFFSET + done, buf, len) != 0) {
        free(buf);
        set_err(err, en, "can't read the package back", NULL);
        return RIP_ERR;
      }
      rip_sha256_update(&sh, buf, len);
      done += len;
    }
    rip_sha256_final(&sh, hdr + 0x160);
  }
  free(buf);

  // The main entries' hashes, the second over the first six meta records only.
  {
    static const uint32_t main_ids[5] = {E_ENTRY_KEYS, E_IMAGE_KEY, E_GENERAL_DIGESTS, E_METAS, E_DIGESTS};
    struct rip_sha256 s1;
    struct rip_sha256 s2;
    rip_sha256_init(&s1);
    rip_sha256_init(&s2);
    for (i = 0; i < 5; i++) {
      e = find_entry(b, main_ids[i]);
      rip_sha256_update(&s1, e->data, e->len);
      if (i < 4) {
        rip_sha256_update(&s2, e->data, e->id == E_METAS ? SC_ENTRIES * 32 : e->len);
      }
    }
    rip_sha256_final(&s1, hdr + 0x100);
    rip_sha256_final(&s2, hdr + 0x120);
  }

  // Header digest, then the "signature": the header's hash wrapped under
  // package key 3.
  sha256(hdr, 0xFE0, hdr + 0xFE0);
  sha256(hdr, 0x1000, h);
  {
    uint8_t sig[256];
    if (wrap_key(keys->pkg_public[3], h, sig) != RIP_OK || rip_seek_write(fd, 0, hdr, sizeof(hdr)) != 0 ||
        rip_seek_write(fd, 0x1000, sig, sizeof(sig)) != 0) {
      set_err(err, en, "can't write the package", NULL);
      return RIP_ERR;
    }
  }
  if (progress && progress(ud, 2 * outer, 2 * outer)) {
    set_err(err, en, "cancelled", NULL);
    return RIP_ECANCEL;
  }
  return RIP_OK;
}

int fpkg_from_folder(const char *folder, const char *out_path, const struct fpkg_keys *keys, iso_progress_fn progress,
                     void *ud, char *err, size_t en) {
  struct build *b = calloc(1, sizeof(*b));
  uint64_t start = rip_now_ms();
  size_t i;
  int fd;
  int rc;

  if (en) {
    err[0] = '\0';
  }
  if (!b) {
    set_err(err, en, "out of memory", NULL);
    return RIP_ENOMEM;
  }
  fd = open(out_path, O_RDWR | O_CREAT | O_TRUNC, 0644);
  if (fd < 0) {
    free(b);
    set_err(err, en, "can't create the package file", NULL);
    return RIP_ERR;
  }
  rc = build_pkg(b, folder, fd, keys, progress, ud, err, en);
  if (rc == RIP_OK && fsync(fd) != 0) {
    set_err(err, en, "can't write the package", NULL);
    rc = RIP_ERR;
  }
  if (close(fd) != 0 && rc == RIP_OK) {
    set_err(err, en, "can't write the package", NULL);
    rc = RIP_ERR;
  }
  if (rc != RIP_OK) {
    unlink(out_path);
    RIP_WARN("fpkg: %s: %s", folder, err);
  } else {
    RIP_INFO("fpkg: wrote %s in %llu ms", out_path, (unsigned long long)(rip_now_ms() - start));
  }
  for (i = 0; i < b->n; i++) {
    free(b->e[i].data);
  }
  sfo_free(&b->sfo);
  free(b->sfo_bytes);
  free(b->names);
  free(b->chunk_sha);
  pfs_free(b->pfs);
  free(b);
  return rc;
}
