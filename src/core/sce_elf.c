#include "ripper/sce_elf.h"

#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "ripper/common.h"
#include "ripper/hash.h"
#include "ripper/log.h"
#include "ripper/util.h"

#define EHDR_SIZE 0x40
#define PHDR_SIZE 0x38
#define PT_LOAD 1
#define PT_SCE_DYNLIBDATA 0x61000000u
#define PT_SCE_PROCPARAM 0x61000001u
#define PT_SCE_MODULE_PARAM 0x61000002u
#define PT_SCE_RELRO 0x61000010u
#define PT_SCE_COMMENT 0x6FFFFF00u
#define PT_SCE_VERSION 0x6FFFFF01u
#define PROCESS_PARAM_MAGIC 0x4942524Fu  // "ORBI"
#define MODULE_PARAM_MAGIC 0x3C13F4BFu
#define BLOCK 0x4000u  // what each digest covers
#define ROUND16(x) (((x) + 15) & ~(uint64_t)15)

static uint64_t le64(const uint8_t *p) {
  return (uint64_t)rip_le32(p) | ((uint64_t)rip_le32(p + 4) << 32);
}

static void put_le64(uint8_t *p, uint64_t v) {
  rip_put_le32(p, (uint32_t)v);
  rip_put_le32(p + 4, (uint32_t)(v >> 32));
}

static int read_magic(const char *path, uint8_t m[4]) {
  int fd = open(path, O_RDONLY);
  int ok = fd >= 0 && rip_read_full(fd, m, 4) == 4;

  if (fd >= 0) {
    close(fd);
  }
  return ok;
}

int sce_is_elf(const char *path) {
  uint8_t m[4];

  return read_magic(path, m) && !memcmp(m,
                                        "\x7F"
                                        "ELF",
                                        4);
}

int sce_is_self(const char *path) {
  uint8_t m[4];

  return read_magic(path, m) && (rip_le32(m) == SCE_SELF_PS4_MAGIC || rip_le32(m) == SCE_SELF_PS5_MAGIC);
}

int sce_retarget_fself(const char *path, enum sce_target target) {
  uint8_t head[12];
  uint8_t magic[4];
  int fd = open(path, O_RDWR);
  int rc = 0;

  if (fd < 0) {
    return -1;
  }
  if (rip_seek_read(fd, 0, head, sizeof(head)) != 0 ||
      (rip_le32(head) != SCE_SELF_PS4_MAGIC && rip_le32(head) != SCE_SELF_PS5_MAGIC) || rip_le32(head + 8) != 0x101) {
    rc = -1;  // not one of ours: left alone
  } else if (rip_le32(head) != (target == SCE_TARGET_PS5 ? SCE_SELF_PS5_MAGIC : SCE_SELF_PS4_MAGIC)) {
    rip_put_le32(magic, target == SCE_TARGET_PS5 ? SCE_SELF_PS5_MAGIC : SCE_SELF_PS4_MAGIC);
    rc = rip_seek_write(fd, 0, magic, 4) == 0 ? 1 : -1;
  }
  if (close(fd) != 0) {
    rc = -1;
  }
  return rc;
}

/* FSELF */

struct seg {
  uint32_t type;
  uint64_t offset;
  uint64_t filesz;
};

static int signed_segment(uint32_t type) {
  return type == PT_LOAD || type == PT_SCE_RELRO || type == PT_SCE_DYNLIBDATA || type == PT_SCE_COMMENT;
}

// One SELF entry: properties as make_fself packs them, then offset and sizes.
static void put_entry(uint8_t *e, int digest, unsigned segment_index, uint64_t offset, uint64_t size) {
  uint64_t props = 1u << 2;  // signed
  if (digest) {
    props |= 1u << 16;  // a digest table
  } else {
    props |= (1u << 11) | (2u << 12);  // blocked, 16 KiB blocks
  }
  props |= (uint64_t)(segment_index & 0xFFFF) << 20;
  put_le64(e, props);
  put_le64(e + 8, offset);
  put_le64(e + 16, size);
  put_le64(e + 24, size);
}

static int copy_range(int in, uint64_t from, int out, uint64_t to, uint64_t len) {
  uint8_t buf[64 * 1024];

  while (len) {
    size_t n = len < sizeof(buf) ? (size_t)len : sizeof(buf);
    if (rip_seek_read(in, from, buf, n) != 0 || rip_seek_write(out, to, buf, n) != 0) {
      return RIP_ERR;
    }
    from += n;
    to += n;
    len -= n;
  }
  return RIP_OK;
}

// SHA-256 of each 16 KiB block of a segment, one after another.
static int write_digests(int in, const struct seg *s, int out, uint64_t at) {
  uint8_t *buf = malloc(BLOCK);
  uint64_t done = 0;
  int rc = RIP_OK;

  if (!buf) {
    return RIP_ENOMEM;
  }
  while (done < s->filesz && rc == RIP_OK) {
    size_t n = s->filesz - done < BLOCK ? (size_t)(s->filesz - done) : BLOCK;
    struct rip_sha256 h;
    uint8_t d[32];
    if (rip_seek_read(in, s->offset + done, buf, n) != 0) {
      rc = RIP_ERR;
      break;
    }
    rip_sha256_init(&h);
    rip_sha256_update(&h, buf, n);
    rip_sha256_final(&h, d);
    rc = rip_seek_write(out, at, d, sizeof(d)) == 0 ? RIP_OK : RIP_ERR;
    at += sizeof(d);
    done += n;
  }
  free(buf);
  return rc;
}

static int file_sha256(int fd, uint64_t size, uint8_t out[32]) {
  uint8_t buf[64 * 1024];
  struct rip_sha256 h;
  uint64_t done = 0;

  rip_sha256_init(&h);
  while (done < size) {
    size_t n = size - done < sizeof(buf) ? (size_t)(size - done) : sizeof(buf);
    if (rip_seek_read(fd, done, buf, n) != 0) {
      return RIP_ERR;
    }
    rip_sha256_update(&h, buf, n);
    done += n;
  }
  rip_sha256_final(&h, out);
  return RIP_OK;
}

int sce_write_fself(const char *elf_path, const char *out_path, enum sce_target target) {
  uint8_t eh[EHDR_SIZE];
  uint8_t *phdrs = NULL;
  struct seg *segs = NULL;
  struct seg version = {0, 0, 0};
  uint8_t *head = NULL;
  struct stat st;
  uint64_t phoff;
  uint16_t phnum;
  unsigned nseg = 0;
  unsigned entries;
  uint64_t header_size;
  uint64_t meta_size;
  uint64_t offset;
  uint64_t file_size;
  int in;
  int out = -1;
  int rc = RIP_ERR;
  unsigned i;
  unsigned j;

  in = open(elf_path, O_RDONLY);
  if (in < 0) {
    return RIP_ERR;
  }
  if (fstat(in, &st) != 0 || rip_seek_read(in, 0, eh, sizeof(eh)) != 0 ||
      memcmp(eh,
             "\x7F"
             "ELF",
             4) != 0) {
    close(in);
    return RIP_ENOTSUP;
  }
  phoff = le64(eh + 0x20);
  phnum = rip_le16(eh + 0x38);
  phdrs = malloc((size_t)phnum * PHDR_SIZE + 1);
  segs = calloc(phnum + 1u, sizeof(*segs));
  if (!phdrs || !segs || phoff != EHDR_SIZE || rip_seek_read(in, phoff, phdrs, (size_t)phnum * PHDR_SIZE) != 0) {
    goto done;
  }
  for (i = 0; i < phnum; i++) {
    const uint8_t *p = phdrs + (size_t)i * PHDR_SIZE;
    struct seg s = {rip_le32(p), le64(p + 0x08), le64(p + 0x20)};
    if (s.offset + s.filesz > (uint64_t)st.st_size) {
      goto done;
    }
    if (s.type == PT_SCE_VERSION) {
      version = s;
    }
    if (signed_segment(s.type)) {
      segs[nseg++] = s;
    }
  }

  entries = nseg * 2;
  header_size = ROUND16(0x20 + (uint64_t)entries * 0x20);
  header_size = ROUND16(header_size + EHDR_SIZE + (uint64_t)phnum * PHDR_SIZE);
  header_size += 0x40 + 0x30;            // extended info, NPDRM block
  meta_size = (uint64_t)entries * 0x50;  // meta blocks
  meta_size += 0x150 + 0x100;            // meta footer, signature
  head = calloc(1, header_size + meta_size);
  if (!head) {
    goto done;
  }

  // The entries: for each signed segment its digests, then its data.
  offset = header_size + meta_size;
  file_size = offset;
  for (i = 0, j = 0; i < phnum; i++) {
    const uint8_t *p = phdrs + (size_t)i * PHDR_SIZE;
    uint64_t filesz = le64(p + 0x20);
    uint64_t digests = (filesz + BLOCK - 1) / BLOCK * 32;
    if (!signed_segment(rip_le32(p))) {
      continue;
    }
    put_entry(head + 0x20 + (size_t)j * 0x20, 1, j + 1, offset, digests);
    offset = ROUND16(offset + digests);
    put_entry(head + 0x20 + (size_t)(j + 1) * 0x20, 0, i, offset, filesz);
    offset = ROUND16(offset + filesz);
    j += 2;
  }
  file_size = offset;

  rip_put_le32(head, target == SCE_TARGET_PS5 ? SCE_SELF_PS5_MAGIC : SCE_SELF_PS4_MAGIC);
  head[4] = 0;                    // version
  head[5] = 1;                    // mode
  head[6] = 1;                    // endian
  head[7] = 0x12;                 // attributes
  rip_put_le32(head + 8, 0x101);  // key type: fake
  rip_put_le16(head + 0x0C, (uint16_t)header_size);
  rip_put_le16(head + 0x0E, (uint16_t)meta_size);
  put_le64(head + 0x10, file_size);
  rip_put_le16(head + 0x18, (uint16_t)entries);
  rip_put_le16(head + 0x1A, 0x22);
  {
    // The ELF and program headers, then the extended info and NPDRM block.
    size_t at = (size_t)ROUND16(0x20 + (uint64_t)entries * 0x20);
    uint8_t *ex;
    memcpy(head + at, eh, EHDR_SIZE);
    memcpy(head + at + EHDR_SIZE, phdrs, (size_t)phnum * PHDR_SIZE);
    at = (size_t)ROUND16(at + EHDR_SIZE + (uint64_t)phnum * PHDR_SIZE);
    ex = head + at;
    put_le64(ex, 0x3100000000000002ull);  // auth id of a regular application
    put_le64(ex + 8, 1);                  // type: fake
    if (file_sha256(in, (uint64_t)st.st_size, ex + 0x20) != RIP_OK) {
      goto done;
    }
    rip_put_le16(ex + 0x40, 3);  // NPDRM block type
    // Meta footer: its one known field.
    rip_put_le32(head + header_size + (uint64_t)entries * 0x50 + 0x30, 0x10000);
  }

  out = open(out_path, O_RDWR | O_CREAT | O_TRUNC, 0755);
  if (out < 0 || rip_seek_write(out, 0, head, header_size + meta_size) != 0) {
    goto done;
  }
  for (i = 0, j = 0; i < nseg; i++, j += 2) {
    uint64_t digest_at = le64(head + 0x20 + (size_t)j * 0x20 + 8);
    uint64_t data_at = le64(head + 0x20 + (size_t)(j + 1) * 0x20 + 8);
    if (write_digests(in, &segs[i], out, digest_at) != RIP_OK ||
        copy_range(in, segs[i].offset, out, data_at, segs[i].filesz) != RIP_OK) {
      goto done;
    }
  }
  // The version segment trails the file, as in a signed SELF.
  if (version.filesz && copy_range(in, version.offset, out, file_size, version.filesz) != RIP_OK) {
    goto done;
  }
  if (!version.filesz && ftruncate(out, (off_t)file_size) != 0) {
    goto done;
  }
  rc = RIP_OK;

done:
  if (out >= 0 && close(out) != 0) {
    rc = RIP_ERR;
  }
  if (rc != RIP_OK && out >= 0) {
    unlink(out_path);
  }
  close(in);
  free(head);
  free(segs);
  free(phdrs);
  return rc;
}

/* Backport */

int sce_backport_elf(const char *path, uint32_t ps4_sdk, uint32_t ps5_sdk) {
  uint8_t eh[EHDR_SIZE];
  int changed = 0;
  int fd = open(path, O_RDWR);
  uint64_t phoff;
  uint16_t phnum;
  uint16_t i;

  if (fd < 0) {
    return RIP_ERR;
  }
  if (rip_seek_read(fd, 0, eh, sizeof(eh)) != 0 || memcmp(eh,
                                                          "\x7F"
                                                          "ELF",
                                                          4) != 0) {
    close(fd);
    return RIP_ENOTSUP;
  }
  phoff = le64(eh + 0x20);
  phnum = rip_le16(eh + 0x38);
  for (i = 0; i < phnum; i++) {
    uint8_t ph[PHDR_SIZE];
    uint8_t param[0x20];
    uint32_t type;
    uint64_t at;
    if (rip_seek_read(fd, phoff + (uint64_t)i * PHDR_SIZE, ph, sizeof(ph)) != 0) {
      break;
    }
    type = rip_le32(ph);
    if (type != PT_SCE_PROCPARAM && type != PT_SCE_MODULE_PARAM) {
      continue;
    }
    at = le64(ph + 0x08);
    if (rip_seek_read(fd, at, param, sizeof(param)) != 0) {
      continue;
    }
    // The parameter block: an 8-byte size, then its magic.
    if (rip_le32(param + 8) != PROCESS_PARAM_MAGIC && rip_le32(param + 8) != MODULE_PARAM_MAGIC) {
      continue;
    }
    // The PS4 SDK 8 bytes after the magic, the PS5's 12 after it.
    if (ps4_sdk && rip_le32(param + 0x10) > ps4_sdk) {
      uint8_t v[4];
      rip_put_le32(v, ps4_sdk);
      changed |= rip_seek_write(fd, at + 0x10, v, 4) == 0;
    }
    if (ps5_sdk && rip_le32(param + 0x14) > ps5_sdk) {
      uint8_t v[4];
      rip_put_le32(v, ps5_sdk);
      changed |= rip_seek_write(fd, at + 0x14, v, 4) == 0;
    }
  }
  close(fd);
  return changed;
}

int sce_backport_sfo(const char *path, uint32_t ps4_sdk) {
  size_t len = 0;
  int changed = 0;
  uint8_t *sfo;
  FILE *f = fopen(path, "rb");
  uint32_t keys;
  uint32_t data;
  uint32_t count;
  uint32_t i;

  if (!f) {
    return RIP_ERR;
  }
  fseek(f, 0, SEEK_END);
  len = (size_t)ftell(f);
  fseek(f, 0, SEEK_SET);
  sfo = malloc(len ? len : 1);
  if (!sfo || fread(sfo, 1, len, f) != len || len < 0x14 || memcmp(sfo, "\0PSF", 4) != 0) {
    fclose(f);
    free(sfo);
    return RIP_ENOTSUP;
  }
  fclose(f);
  keys = rip_le32(sfo + 0x08);
  data = rip_le32(sfo + 0x0C);
  count = rip_le32(sfo + 0x10);
  for (i = 0; i < count && 0x14 + (size_t)i * 16 + 16 <= len; i++) {
    const uint8_t *e = sfo + 0x14 + (size_t)i * 16;
    const char *key = (const char *)sfo + keys + rip_le16(e);
    uint16_t fmt = rip_le16(e + 2);
    uint32_t dlen = rip_le32(e + 4);
    uint8_t *val = sfo + data + rip_le32(e + 12);
    if (val + dlen > sfo + len || (const uint8_t *)key >= sfo + len) {
      continue;
    }
    if (!strcmp(key, "SYSTEM_VER") && fmt == 0x0404 && dlen == 4 && rip_le32(val) > ps4_sdk) {
      rip_put_le32(val, ps4_sdk);
      changed = 1;
    } else if (!strcmp(key, "PUBTOOLINFO") && fmt == 0x0204) {
      char *p = NULL;
      uint32_t k;
      for (k = 0; k + 8 <= dlen && !p; k++) {
        if (!memcmp(val + k, "sdk_ver=", 8)) {
          p = (char *)val + k;
        }
      }
      if (p && p + 16 <= (char *)val + dlen && strtoul(p + 8, NULL, 16) > ps4_sdk) {
        char hex[9];
        snprintf(hex, sizeof(hex), "%08X", ps4_sdk);
        memcpy(p + 8, hex, 8);
        changed = 1;
      }
    }
  }
  if (changed) {
    changed = rip_write_file(path, sfo, len) == 0 ? 1 : RIP_ERR;
  }
  free(sfo);
  return changed;
}

int sce_backport_json(const char *path, uint32_t ps5_sdk) {
  static const char *const keys[] = {"\"requiredSystemSoftwareVersion\"", "\"sdkVersion\""};
  uint8_t major = (uint8_t)(ps5_sdk >> 24);
  int changed = 0;
  size_t len = 0;
  char *txt;
  FILE *f = fopen(path, "rb");
  size_t k;

  if (!f) {
    return RIP_ERR;
  }
  fseek(f, 0, SEEK_END);
  len = (size_t)ftell(f);
  fseek(f, 0, SEEK_SET);
  txt = malloc(len + 1);
  if (!txt || fread(txt, 1, len, f) != len) {
    fclose(f);
    free(txt);
    return RIP_ERR;
  }
  fclose(f);
  txt[len] = '\0';
  for (k = 0; k < RIP_ARRAY_LEN(keys); k++) {
    // "0xMMmmpp...": lowered to "0xTT000000..." of the same length when MM is above the target.
    char *p = strstr(txt, keys[k]);
    char *q;
    if (!p || !(p = strchr(p + strlen(keys[k]), '"')) || strncmp(p + 1, "0x", 2) != 0) {
      continue;
    }
    q = strchr(p + 1, '"');
    if (!q || q - p < 6) {
      continue;
    }
    {
      char cur[3] = {p[3], p[4], 0};
      if ((uint8_t)strtoul(cur, NULL, 16) > major) {
        char *d;
        snprintf(cur, sizeof(cur), "%02X", major);
        p[3] = cur[0];
        p[4] = cur[1];
        for (d = p + 5; d < q; d++) {
          *d = '0';
        }
        changed = 1;
      }
    }
  }
  if (changed) {
    changed = rip_write_file(path, txt, len) == 0 ? 1 : RIP_ERR;
  }
  free(txt);
  return changed;
}

static const struct {
  const char *id;
  uint32_t ps4_sdk;
  uint32_t ps5_sdk;
} TARGETS[] = {
    // PS4 titles, for a PS4 on that firmware or later.
    {"ps4-5.05", 0x05050001u, 0},
    {"ps4-6.00", 0x06008001u, 0},
    {"ps4-7.00", 0x07008001u, 0},
    {"ps4-9.00", 0x09008001u, 0},
    {"ps4-10.00", 0x10008001u, 0},
    {"ps4-11.00", 0x11008001u, 0},
    // PS5 titles, for a PS5 on that firmware generation or later.
    {"ps5-1", 0x07590001u, 0x01000050u},
    {"ps5-2", 0x08050001u, 0x02000009u},
    {"ps5-3", 0x08540001u, 0x03000027u},
    {"ps5-4", 0x09040001u, 0x04000031u},
    {"ps5-5", 0x09590001u, 0x05000033u},
    {"ps5-6", 0x10090001u, 0x06000038u},
    {"ps5-7", 0x10590001u, 0x07000038u},
    {"ps5-8", 0x11090001u, 0x08000041u},
    {"ps5-9", 0x11590001u, 0x09000040u},
    {"ps5-10", 0x12090001u, 0x10000040u},
};

int sce_backport_target(const char *id, uint32_t *ps4_sdk, uint32_t *ps5_sdk) {
  size_t i;

  for (i = 0; i < RIP_ARRAY_LEN(TARGETS); i++) {
    if (!strcmp(TARGETS[i].id, id)) {
      *ps4_sdk = TARGETS[i].ps4_sdk;
      *ps5_sdk = TARGETS[i].ps5_sdk;
      return RIP_OK;
    }
  }
  return RIP_ENOENT;
}
