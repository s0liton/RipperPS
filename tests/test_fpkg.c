// PS4 fake packages: a package built from a small made-up game with
// throwaway keys (openssl RSA-2048 and random AES keys, made for this test
// only), then read back with a reader of its own: the header and its
// signature, each entry's digest and encryption, and the PFS images.

#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "ripper/aes.h"
#include "ripper/common.h"
#include "ripper/fpkg.h"
#include "ripper/hash.h"
#include "ripper/rsa.h"
#include "ripper/sfo.h"
#include "ripper/util.h"
#include "test.h"

static const char TEST_KEYS[] =
    "PkgPublicKey0 = 9bb98f7d1051b692de3c1f19311ffe27fabcc3f8e849abb974364d184883e72b"
    "b76b6b7b117a5bb226a51eea7596fe09e25ff2c596d3d867c3a6a6d68408e596"
    "8f354e825ff7788781e8ce9324f598e4e438bd5908ca71f7c31a2dd85c1175c9"
    "75003f31711e911670ca139e05c10ff9eb16e9954267aca6af60dbe9ac17a2cb"
    "4bc338a61681513abe7957ec8d2a357f66bc5e8669ced974d7f4e0e1dc273ca2"
    "c88c8b93b903a909ab217407028f66a8f7c5c082dfbacf314807cd605ed295c0"
    "6eb4868435d4c02f6cde0439c818e335bf1c4157a176a80bfec8d707620e3c84"
    "0109d68a9f71e9bfe214803c013e923e0fa2fbfb432f5a45621b0c4ef802089b\n"
    "PkgPublicKey1 = ba912dc6b48dfe98d42113382cc6b439e75f91366944b9420edf5ea657961d45"
    "1b4fb6e41ba65a316a0e99c371aeec1d8797f203d3f26bb30436884751c68a1d"
    "cacf2565b84e8daca248ff9161002450dbe01c718e3b3b1fb98510d8fb0c688f"
    "01a177ab25583ab752ca0f9be619b8e798b4e9557e60198561b674fc5b61eb8e"
    "58474e2088868c00a4fffc9b1a0eedfb6aebb90097628823e8d933ba046e9c69"
    "a998b7a90402de6453aaa8c972170adab4d05477f9b3b4df5ca001c2664955a4"
    "1dc8996b814b5f3037a0f59d1be605ffa22457608d509851f803d97ce0f021df"
    "db0ea129aea4b7345f114c9ac9a6fc7199b19ccf9f69a718c656120644b6d153\n"
    "PkgPublicKey2 = 9c5cc18f8e320c5ddc810e7abe0b5efe199cca2db96b7e6b2105af24c2a38345"
    "f68465c233de6414325e52c73962b4b08e4d4f393e34c19704d7f86259cc3687"
    "7ed5c1e84f5029952ee7e8aa84a8b0ca8e3b04b32441f395b375537a28f8a9ff"
    "399a3489930e8ee70f770353df6b85c5702117110c59965b1874d7787dc1c1ad"
    "c68da69b8a202e0a749e154dc1c5c52e348e5a6c28ab88383139910fc7b5aa68"
    "e13181a8168a14386aafbcd5ee454ebc09d8675ed9c0d5bbe86ff55c2e01e1e9"
    "b167317e528d3f40ef35c9789ac70ed7baffae351499661977d800989bc0bb2e"
    "530c666e0b90c2e5b53f093ea67bf294a045c3f2e5467b10b6f0a5cae185ab53\n"
    "PkgPublicKey3 = b917dc844ee166faf337839c4744954f8baffe75ddda68baa0e7e382e9092dea"
    "297bac09129f458beb805a8193c1c63b3980def1f18cccf9bb8d51bfb5e3fa19"
    "fd07e28a4bf22f7e148e2b86176eaa0f0e891d86c1f43f947c10b2f0dfd63062"
    "e79825967e0778a45a8addac7bd206b7b7944805c0788f2f1f57104823d48764"
    "c570c472f0718700e00163fb251450bcbe59a746f97b53e105015c958c7254ee"
    "ace92550cbd3c07a1cafa4fc289e3cd50f78468351df55937ce23bded52aa28c"
    "bc21fc07b8ebe5564c8b5a4062e5a7085012647649810c8870aa72c5ab783af3"
    "db2f8c8ea70056e02fa07679e7b5d5f6ffa3b5fdfa6f705a3d4a742cd280f867\n"
    "PkgPublicKey4 = cb1daff66dca41bc8d6cc3b9ceef860277df92961733d674db42b5253cfba713"
    "14e78e68ece815c48a9e9a3391ed278d54ce940e1de7c4841f2b6de16c3d1c2a"
    "c65dd58fb34f2e73897e21ef9db940a1114f62dc27cabe37fef16df76f845041"
    "c402e22993a0fc84e87f353eaaafdd482cfb40de72df42bbe4e01f871dc55ab2"
    "d8b7ec6b73fa30654cb7182d19b7b5810617685ab93de8dcaf5d949fd426f72c"
    "ae43f426c04c4dc65139c6752633136e4003b8e92b9442a419483a42e315848a"
    "d429509c1634f3f5f52ce2b4fe280be0af38fbe3c9fd9d0229a7d43793370c5f"
    "8c678cf3b757c51c055645eb1eb4323978299fb71c89f1fc7f0195eaba200f8f\n"
    "PkgPublicKey5 = a0aacdfbe83d8f8a044a8ae09d4309b7d2c14565c6f9b984a7b9794768ff1379"
    "6c6d76a1b0292223090b4f61d914e153f6ac6b2a03ac524a2eb73487055d8eaa"
    "2b18000a724c47e6ed803f543275d294310e0e7cffd3cafa178a1f2249dbc009"
    "eaa4c4b4a282bee38d8e2bca9e6393cb23b46c9172d7246465dc9793093269a3"
    "e8ac1c7a65c2739aabc181423efd3f1bb0577194b10a501868c9cba2031e8aa0"
    "38178aecbc13deae7e7972afee8cc3d9ab9692087e348fc48ba51bb39c9cd34b"
    "193b0dec2faaf2132f5c7129c9d5082a39ad5a1f66db4da221ee3b4d75698d23"
    "17a610c5034da9abe2cdafcf9d4b3adf58a3a4c799f78616dedd9536d5294d6f\n"
    "PkgPublicKey6 = b175e2de2111fbeb8a6ac297200b5d60913c5be99d1e8b5ee5a6dff5f86e9d8a"
    "c1dd06f6b7676ef240c2e72e6c5439f24933c2f51c5b8ce326b3b0b798e3d43a"
    "d5d354bfe6b71a7f1b1d36b0b43a66750dfa7c1e150c1ad62d947607fe2bc727"
    "fc6c98502ee1ddb94a799118b4d13867472343a20d96becc7125b256fc56caf6"
    "2d75c3def0c10e7bb16474506c72ca74d12cd21b4e49fb8cbf3133db687a0a95"
    "7decaad18b5bc71d5538fdac61668e112f0798ca414cc6dc8ff85fa1800554af"
    "9104db064d0e1d10fa2103b4e7017f10750401c1dbe00025b70cd555e8e969b4"
    "d224f40eb8bb2df0629cfc8f9215f08ebf2cca7e77c99cba9d162fb4665e39ab\n"
    "FakeKeyModulus = b3449945d2bdb75b2f2616ca2ffd97a23ff6b0e03af7ccdf9da6575c6a81469c"
    "dac8485df260584f35841cfe6213818cb39a29787968ca3c2d7fe4572d2db371"
    "f855e3a29934280633e185f0107704ea19566713376b1becc8ea18ae5cecef1b"
    "2107791975af9f7886409ea9890f420aba0b0ca611be9a923631cd0d9e815244"
    "961fbe6dcd9220f39f365d4ebd1eebfabf9d200086a39c0e82a16084f5893151"
    "351024b6d7515fb84cd958ea280e79b19da6bc23b16c6f7309a90285217560e0"
    "45968091acccb92bd641eb219d355912947a330be611bca411ab3aef08fddd58"
    "26501764d41c15ee444b876b35342d13a6e80a8644de6e640322d9d99ce12a03\n"
    "DebugRifModulus = 9e6b005ed9e7ac3414900c3c40f74de43719a2727225058698ba40b4cceecc52"
    "c36d7028a971ac450637332990c8c6f0ccd10e214365b2938a0d98cd9e73265f"
    "2da90d5703d83dad381f82354de874ef1a8a158499e788b881ba970e6cc27d94"
    "5a676a038f88e5096a73a469c1d7253df9a9aeef7ba377090e149341bf85544a"
    "4c7872fa7469fac3655ac67127879d3b1ffa12c0711ec6735ee9425c4d5ba601"
    "d20bc6c1c6b0c3f0cfa2d2faf1f9e5b24e731db0283061b7ee4315447f3a0c5c"
    "3e7316d8a9d8f999f0796408ffcb5c0fbbc99c6c2d23d9167735a78aae8846e1"
    "6f649012cd53c3fdf220402baf7219ce31e12217a958a047679020cdc1fb9cef\n"
    "DebugRifPrivateExponent = 4a88b87f6811a6c5482970004766b3b89cae0c6ec142a6e36967343b7cb21be3"
    "12bca2491753fac3ed053aa6a34ebbbf1ef67b29ee4e626325507964c0d897a4"
    "af547b08c62fcef0f95765c95392fbb565acb32c14c7495dd9f7a2cbb0231fb7"
    "9583536f42709ba63ef612d4895582d9b26601fefe126e568dcfe36acb43dd60"
    "6bd210596a283515eaa89f2b72d150334f9529da60be7608773eb4d9ff1fc502"
    "748fa7c5b959ed18a195a34c33d2a34c7fec66ef4a5bf49a26cd76cc41283e27"
    "34d4cc5b4b60563556060483a0937ab6247a3e79999024b205c7ba708cbb7598"
    "0b48a467e7dcbaa6049b196423b64af0a89674dc1185055c749ec8ef3c4d4d4d\n"
    "RifDebugKey = 3a4992009f5481b3e158fd4ab83f752c\n"
    "KeystoneHmacKey = 191cd217ac2883805098d7f8f87017132011747e1e84eaedb7051f582db6b241\n"
    "KeystoneMacData = 3ee25f04f7f4546b941a0e45aa546c3e3988b97ba71154085d65651cf0ede5c5\n";

static const char PKG3_D[] =
    "50a86c491fd2abbca9e6442d030589075bf8faab445d5c0c0ff85d357bf5ab13"
    "82b648f6bf2db6f1cd53669d56d5375c31a7cdffa2e257a4702d9a352fbf440a"
    "b5300d7174bcfa233833e8158bfa8692ff239341591de5e2ffd98b10da8a0d15"
    "dc9479b4c600dd7ac99600199cef9e4bcaed0c21ad891be44e04f3ac6c28be42"
    "d62003e8a2005f49b2aaa1f591bf224d734ed1718267e381a6637da986f8dcd1"
    "80744f2e6d21d5fa937c533eae43718509409d431f213f0d9cf6efd2e2fc8351"
    "d243a808ad2eea4dc75247f5bac9b91409e4d8955480a520b498f7e3fafe70f5"
    "e7decb18dfc6729f1be83812e744f992e144240e38816c05147f2a234017d311";
static const char FAKE_D[] =
    "1ca119a2a75ba45b0bf6419b7bfb3c3fd831408c2994062f0c26a83b051e0235"
    "edfc20653333cf1bf9f2fb0d112d5ba4d47b614b71304563ee419cd38df93222"
    "edc30c689cf3a714b3d7bcce85ad4c514a9c9c3785c3e04b7a1c10b6fff62d8a"
    "ca9a93eccca3b08c7751a89a70b3741b9c71eea232f69ab43bb2cbb44a1a6315"
    "cb7d8cfd5bf932003c76184a5b5bbc26822554b922b75abcbe794f58cdf06bfd"
    "ad84fd9c89f1a89e1aa7694a1286faf8641e6b4e36369558cf0394f03e72005a"
    "a46437ae549f22b016d59a9d63115848e37d102e16623a06610d7cbfe83b7452"
    "579389eb665f7550099a11a95485c248f8c4ee40c6fd67e04034ee8b9ff0fd29";

#define CID "UP0000-CUSA99999_00-RIPPERTEST000000"
#define BS 0x10000u
#define MANY 2800  // empty files: enough entries to spill into a second block

static void unhex_or_die(const char *h, uint8_t *out, size_t len) {
  if (rip_unhex(h, out, len) != 0) {
    fprintf(stderr, "bad test hex\n");
    exit(1);
  }
}

static void sha(const void *d, size_t n, uint8_t out[32]) {
  struct rip_sha256 h;
  rip_sha256_init(&h);
  rip_sha256_update(&h, d, n);
  rip_sha256_final(&h, out);
}

static void derive(uint32_t i, uint8_t out[32]) {
  uint8_t data[96];
  uint8_t be[4];
  uint8_t id[48];

  rip_put_be32(be, i);
  sha(be, 4, data);
  memset(id, 0, sizeof(id));
  memcpy(id, CID, 36);
  sha(id, 48, data + 32);
  memset(data + 64, '0', 32);
  sha(data, 96, out);
}

static uint64_t be64(const uint8_t *p) {
  return (uint64_t)rip_be32(p) << 32 | rip_be32(p + 4);
}

static uint64_t le64(const uint8_t *p) {
  return (uint64_t)rip_le32(p + 4) << 32 | rip_le32(p);
}

/* The made-up game */

struct sfo_kv {
  const char *key;
  int is_int;
  const char *s;
  uint32_t v;
  uint32_t max;
};

static size_t make_sfo(uint8_t *out, const struct sfo_kv *kv, int n) {
  size_t kt = 0x14 + 16 * (size_t)n;
  size_t ko = 0;
  size_t dt;
  size_t dof = 0;
  int i;

  for (i = 0; i < n; i++) {
    ko += strlen(kv[i].key) + 1;
  }
  dt = (kt + ko + 3) & ~(size_t)3;
  ko = 0;
  memset(out, 0, 4096);
  memcpy(out, "\0PSF", 4);
  rip_put_le32(out + 4, 0x101);
  rip_put_le32(out + 8, (uint32_t)kt);
  rip_put_le32(out + 12, (uint32_t)dt);
  rip_put_le32(out + 16, (uint32_t)n);
  for (i = 0; i < n; i++) {
    uint8_t *e = out + 0x14 + 16 * i;
    uint32_t max = kv[i].is_int ? 4 : kv[i].max;
    rip_put_le16(e, (uint16_t)ko);
    rip_put_le16(e + 2, kv[i].is_int ? 0x0404 : 0x0204);
    rip_put_le32(e + 4, kv[i].is_int ? 4 : (uint32_t)strlen(kv[i].s) + 1);
    rip_put_le32(e + 8, max);
    rip_put_le32(e + 12, (uint32_t)dof);
    memcpy(out + kt + ko, kv[i].key, strlen(kv[i].key));
    if (kv[i].is_int) {
      rip_put_le32(out + dt + dof, kv[i].v);
    } else {
      memcpy(out + dt + dof, kv[i].s, strlen(kv[i].s));
    }
    ko += strlen(kv[i].key) + 1;
    dof += max;
  }
  return dt + dof;
}

static void put_file(const char *dir, const char *rel, const uint8_t *data, size_t len) {
  char path[512];
  char *slash;
  int fd;

  snprintf(path, sizeof(path), "%s/%s", dir, rel);
  slash = strrchr(path, '/');
  *slash = '\0';
  rip_mkdir_p(path);
  *slash = '/';
  fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
  CHECK(fd >= 0);
  if (fd >= 0) {
    CHECK(len == 0 || rip_write_all(fd, data, len) == 0);
    close(fd);
  }
}

static void pattern_file(const char *dir, const char *rel, size_t len, unsigned seed) {
  uint8_t *d = malloc(len ? len : 1);
  size_t i;

  for (i = 0; i < len; i++) {
    d[i] = (uint8_t)((i * 131u + seed) ^ (i >> 9));
  }
  put_file(dir, rel, d, len);
  free(d);
}

// Files that land in the inner image, and the ones that must not.
static const char *const IN_IMAGE[] = {
    "/eboot.bin",
    "/sce_module/libc.prx",
    "/data/big.bin",
    "/data/AZ",
    "/data/B;",
    "/data/part.bin",
    "/data/sub/empty.txt",
    "/sce_sys/about/right.sprx",
    "/sce_sys/keystone",
    "/sce_sys/trophy/notes.txt",
};
static const char *const NOT_IN_IMAGE[] = {"/sce_sys/param.sfo", "/sce_sys/icon0.png", "/sce_sys/trophy/trophy00.trp",
                                           "/sce_sys/playgo-chunk.dat", "/sce_sys/pic1.png"};

static void make_game(const char *dir) {
  static const struct sfo_kv kv[] = {
      {"APP_TYPE", 1, NULL, 1, 0},
      {"APP_VER", 0, "01.00", 0, 8},
      {"ATTRIBUTE", 1, NULL, 0, 0},
      {"CATEGORY", 0, "gd", 0, 4},
      {"CONTENT_ID", 0, CID, 0, 48},
      {"FORMAT", 0, "obs", 0, 4},
      {"PUBTOOLINFO", 0, "c_date=20240102,c_time=030405,sdk_ver=05050001", 0, 512},
      {"SYSTEM_VER", 1, NULL, 0x05050000, 0},
      {"TITLE", 0, "Ripper Test", 0, 128},
      {"TITLE_ID", 0, "CUSA99999", 0, 12},
      {"VERSION", 0, "01.00", 0, 8},
  };
  uint8_t sfo[4096];
  size_t n = make_sfo(sfo, kv, (int)RIP_ARRAY_LEN(kv));
  char name[64];
  int i;

  put_file(dir, "sce_sys/param.sfo", sfo, n);
  pattern_file(dir, "sce_sys/icon0.png", 5000, 1);
  pattern_file(dir, "sce_sys/pic1.png", 70000, 2);
  pattern_file(dir, "sce_sys/trophy/trophy00.trp", 3000, 3);
  pattern_file(dir, "sce_sys/trophy/notes.txt", 10, 4);
  pattern_file(dir, "sce_sys/playgo-chunk.dat", 416, 5);
  pattern_file(dir, "sce_sys/about/right.sprx", 777, 6);
  pattern_file(dir, "eboot.bin", 100000, 7);
  pattern_file(dir, "sce_module/libc.prx", 65536, 8);
  pattern_file(dir, "data/big.bin", 20 * BS + 123, 9);
  pattern_file(dir, "data/part.bin", 3 * BS, 10);
  pattern_file(dir, "data/AZ", 11, 11);  // AZ and B; share a flat path hash
  pattern_file(dir, "data/B;", 12, 12);
  put_file(dir, "data/sub/empty.txt", NULL, 0);
  for (i = 0; i < MANY; i++) {
    snprintf(name, sizeof(name), "many/f%04d", i);
    put_file(dir, name, NULL, 0);
  }
}

/* A reader of its own */

struct pkg {
  uint8_t *b;
  size_t len;
  struct fpkg_keys k;
  uint8_t pkg3_d[256];
  uint8_t fake_d[256];
};

static const uint8_t *find_meta(const struct pkg *p, uint32_t id) {
  uint32_t n = rip_be32(p->b + 0x10);
  const uint8_t *t = p->b + rip_be32(p->b + 0x18);
  uint32_t i;

  for (i = 0; i < n; i++) {
    if (rip_be32(t + 32 * i) == id) {
      return t + 32 * i;
    }
  }
  return NULL;
}

// An entry's bytes, decrypted when it's encrypted. The caller frees them.
static uint8_t *get_entry(const struct pkg *p, uint32_t id, uint32_t *len) {
  const uint8_t *m = find_meta(p, id);
  uint8_t *out;

  *len = 0;
  if (!m) {
    return NULL;
  }
  *len = rip_be32(m + 20);
  out = malloc(*len ? *len : 1);
  memcpy(out, p->b + rip_be32(m + 16), *len);
  if (rip_be32(m + 8) & 0x80000000u) {
    uint8_t buf[64];
    uint8_t ik[32];
    struct rip_aes128 aes;
    memcpy(buf, m, 32);
    derive((rip_be32(m + 12) >> 12) & 0xF, buf + 32);
    sha(buf, 64, ik);
    rip_aes128_init_dec(&aes, ik + 16);
    rip_aes128_cbc_decrypt(&aes, ik, out, *len);
  }
  return out;
}

// m^d mod n, then the PKCS#1 v1.5 type 2 block's 32-byte payload.
static int unwrap(const uint8_t *n, const uint8_t *d, const uint8_t c[256], uint8_t out[32]) {
  uint8_t m[256];
  int i;

  if (rip_rsa_modexp(c, d, 256, n, 256, m) != RIP_OK || m[0] != 0 || m[1] != 2 || m[223] != 0) {
    return 0;
  }
  for (i = 2; i < 223; i++) {
    if (m[i] == 0) {
      return 0;
    }
  }
  memcpy(out, m + 224, 32);
  return 1;
}

struct outer {
  uint8_t *img;
  uint64_t blocks;
  struct rip_hmac_sha256 sign;
  int bad_sigs;
};

static int sig_ok(struct outer *o, const uint8_t *rec, uint64_t block) {
  struct rip_hmac_sha256 h = o->sign;
  uint8_t s[32];

  if (block >= o->blocks) {
    return 0;
  }
  rip_hmac_sha256_update(&h, o->img + block * BS, BS);
  rip_hmac_sha256_final(&h, s);
  return !memcmp(s, rec, 32) && rip_le32(rec + 32) == block;
}

// A signed inode's data, every block checked against its signature.
static uint8_t *outer_file(struct outer *o, const uint8_t *ino, uint64_t *size) {
  uint32_t blocks = rip_le32(ino + 0x60);
  uint8_t *out;
  uint32_t k;

  *size = le64(ino + 8);
  out = calloc(blocks ? blocks : 1, BS);
  for (k = 0; k < blocks; k++) {
    const uint8_t *rec;
    if (k < 12) {
      rec = ino + 0x64 + 36 * k;
    } else if (k < 12 + 1820) {
      const uint8_t *ib = ino + 0x64 + 36 * 12;
      if (k == 12 && !sig_ok(o, ib, rip_le32(ib + 32))) {
        o->bad_sigs++;
      }
      rec = o->img + (uint64_t)rip_le32(ib + 32) * BS + 36 * (k - 12);
    } else {
      const uint8_t *ib1 = ino + 0x64 + 36 * 13;
      uint32_t q = (k - 1832) / 1820;
      const uint8_t *ind = o->img + (uint64_t)rip_le32(ib1 + 32) * BS + 36 * q;
      rec = o->img + (uint64_t)rip_le32(ind + 32) * BS + 36 * ((k - 1832) % 1820);
    }
    if (!sig_ok(o, rec, rip_le32(rec + 32))) {
      o->bad_sigs++;
    }
    memcpy(out + (size_t)k * BS, o->img + (uint64_t)rip_le32(rec + 32) * BS, BS);
  }
  return out;
}

static uint32_t find_dirent(const uint8_t *dir, size_t len, const char *name) {
  size_t off = 0;

  while (off + 16 <= len) {
    uint32_t es = rip_le32(dir + off + 12);
    if (es == 0) {
      off = (off / BS + 1) * BS;
      continue;
    }
    if (rip_le32(dir + off + 8) == strlen(name) && !memcmp(dir + off + 16, name, strlen(name))) {
      return rip_le32(dir + off);
    }
    off += es;
  }
  return UINT32_MAX;
}

static uint32_t path_hash(const char *s) {
  uint32_t h = 0;
  for (; *s; s++) {
    unsigned char c = (unsigned char)*s;
    h = (uint32_t)(c >= 'a' && c <= 'z' ? c - 32 : c) + 31 * h;
  }
  return h;
}

struct inner {
  const uint8_t *img;
  uint64_t size;
  const char *folder;
  int files;
  int dirs;
  int many;
  int found[RIP_ARRAY_LEN(IN_IMAGE)];
  int unexpected;
  int mismatched;
  int bad_fpt;
};

static const uint8_t *inner_inode(const struct inner *in, uint32_t ino) {
  // The superblock's inode-block descriptor holds the first block.
  return in->img + (le64(in->img + 0xD8) + ino / 390) * BS + (ino % 390) * 0xA8;
}

// The flat path table holds a path's inode, or a collision list that does.
static int fpt_has(const struct inner *in, const char *path, uint32_t ino, int dir) {
  const uint8_t *fi = inner_inode(in, 1);
  const uint8_t *t = in->img + (uint64_t)rip_le32(fi + 0x64) * BS;
  uint64_t n = le64(fi + 8) / 8;
  uint32_t h = path_hash(path);
  uint64_t i;

  for (i = 0; i < n; i++) {
    uint32_t v = rip_le32(t + 8 * i + 4);
    if (rip_le32(t + 8 * i) != h) {
      continue;
    }
    if (!(v & 0x80000000u)) {
      return v == (ino | (dir ? 0x20000000u : 0));
    } else {
      const uint8_t *ci = inner_inode(in, 2);
      const uint8_t *cr = in->img + (uint64_t)rip_le32(ci + 0x64) * BS + (v & 0x7FFFFFFF);
      while (rip_le32(cr + 12)) {
        if (rip_le32(cr) == ino && rip_le32(cr + 8) == strlen(path) && !memcmp(cr + 16, path, strlen(path))) {
          return 1;
        }
        cr += rip_le32(cr + 12);
      }
      return 0;
    }
  }
  return 0;
}

static void walk_inner(struct inner *in, uint32_t ino, const char *path) {
  const uint8_t *di = inner_inode(in, ino);
  const uint8_t *d = in->img + (uint64_t)rip_le32(di + 0x64) * BS;
  size_t len = (size_t)rip_le32(di + 0x60) * BS;
  size_t off = 0;

  while (off + 16 <= len) {
    uint32_t child = rip_le32(d + off);
    uint32_t type = rip_le32(d + off + 4);
    uint32_t nl = rip_le32(d + off + 8);
    uint32_t es = rip_le32(d + off + 12);
    char sub[600];
    if (es == 0) {
      off = (off / BS + 1) * BS;
      continue;
    }
    snprintf(sub, sizeof(sub), "%s/%.*s", path, (int)nl, (const char *)d + off + 16);
    off += es;
    if (type == 3) {
      in->dirs++;
      if (!fpt_has(in, sub, child, 1)) {
        in->bad_fpt++;
      }
      walk_inner(in, child, sub);
    } else if (type == 2) {
      const uint8_t *fi = inner_inode(in, child);
      uint64_t size = le64(fi + 8);
      const uint8_t *data = in->img + (uint64_t)rip_le32(fi + 0x64) * BS;
      size_t i;
      int known = 0;
      in->files++;
      if (!fpt_has(in, sub, child, 0)) {
        in->bad_fpt++;
      }
      if (!strncmp(sub, "/many/", 6)) {
        in->many++;
        known = size == 0;
      }
      for (i = 0; i < RIP_ARRAY_LEN(IN_IMAGE); i++) {
        if (!strcmp(sub, IN_IMAGE[i])) {
          in->found[i]++;
          known = 1;
        }
      }
      if (!known) {
        in->unexpected++;
      }
      if (strcmp(sub, "/sce_sys/keystone") != 0) {
        char src[700];
        size_t sl = 0;
        unsigned char *want;
        snprintf(src, sizeof(src), "%s%s", in->folder, sub);
        want = test_read_file(src, &sl);
        if (!want || sl != size || (size && memcmp(want, data, (size_t)size) != 0)) {
          in->mismatched++;
        }
        free(want);
      } else if (size != 96 || memcmp(data, "keystone\x02\x00\x01\x00", 12) != 0) {
        in->mismatched++;
      }
    }
  }
}

static void check_package(struct pkg *p, const char *folder) {
  uint8_t h[32];
  uint8_t dk1[32];
  uint8_t dk3[32];
  uint8_t got[32];
  uint32_t len;
  uint32_t n = rip_be32(p->b + 0x10);
  uint64_t body_size = be64(p->b + 0x28);
  uint64_t pfs_off = be64(p->b + 0x410);
  uint64_t pfs_size = be64(p->b + 0x418);
  const uint8_t *t;
  uint8_t *e;
  uint32_t i;

  // Header.
  CHECK(!memcmp(p->b,
                "\x7F"
                "CNT",
                4));
  CHECK(!memcmp(p->b + 0x40, CID, 36));
  CHECK(rip_be32(p->b + 0x18) == 0x2A80);
  CHECK(be64(p->b + 0x20) == 0x2000);
  CHECK(pfs_off == 0x2000 + body_size && pfs_off % 0x80000 == 0);
  CHECK(be64(p->b + 0x430) == p->len && pfs_off + pfs_size == p->len);
  CHECK(rip_be32(p->b + 0x74) == 0x1A && rip_be32(p->b + 0x78) == 0x0A000000);
  sha(p->b, 0xFE0, h);
  CHECK_MEM(h, p->b + 0xFE0, 32);
  sha(p->b, 0x1000, h);
  CHECK(unwrap(p->k.pkg_public[3], p->pkg3_d, p->b + 0x1000, got));
  CHECK_MEM(h, got, 32);
  sha(p->b + 0x2000, (size_t)body_size, h);
  CHECK_MEM(h, p->b + 0x160, 32);
  sha(p->b + pfs_off, 0x10000, h);
  CHECK_MEM(h, p->b + 0x460, 32);
  sha(p->b + pfs_off, (size_t)pfs_size, h);
  CHECK_MEM(h, p->b + 0x440, 32);

  // Meta table: sorted, every entry's digest right.
  t = p->b + 0x2A80;
  {
    const uint8_t *dg = find_meta(p, 0x1);
    const uint8_t *digests = p->b + rip_be32(dg + 16);
    for (i = 0; i < n; i++) {
      if (i) {
        CHECK(rip_be32(t + 32 * i) > rip_be32(t + 32 * (i - 1)));
        sha(p->b + rip_be32(t + 32 * i + 16), rip_be32(t + 32 * i + 20), h);
        CHECK_MEM(h, digests + 32 * i, 32);
      }
      CHECK(rip_be32(t + 32 * i + 16) % 16 == 0);
    }
    sha(digests, n * 32, h);
    CHECK_MEM(h, p->b + 0x140, 32);
  }

  // Keys: EKPFS is key 1, wrapped under package key 3 and the fake key.
  derive(1, dk1);
  derive(3, dk3);
  e = get_entry(p, 0x10, &len);
  CHECK(e && len == 0x800);
  if (e) {
    CHECK(unwrap(p->k.pkg_public[3], p->pkg3_d, e + 256 + 3 * 256, got));
    CHECK_MEM(got, dk3, 32);
    sha(dk1, 32, h);
    for (i = 0; i < 32; i++) {
      h[i] ^= dk1[i];
    }
    CHECK_MEM(h, e + 32 + 32, 32);
  }
  free(e);
  e = get_entry(p, 0x20, &len);
  CHECK(e && len == 0x100 && unwrap(p->k.fake_modulus, p->fake_d, e, got) && !memcmp(got, dk1, 32));
  free(e);

  // License: decrypts to a RIF signed with the debug key.
  e = get_entry(p, 0x400, &len);
  CHECK(e && len == 0x400);
  if (e) {
    CHECK(!memcmp(e, "RIF\0", 4) && !memcmp(e + 0x20, CID, 36));
    sha(e, 0x300, h);
    CHECK(rip_rsa_verify_sha256(p->k.rif_modulus, 256, h, e + 0x300) == RIP_OK);
  }
  free(e);
  e = get_entry(p, 0x401, &len);
  CHECK(e && len == 0x200 && !memcmp(e, CID, 36) && rip_be32(e + 0x44) == 0x1A);
  free(e);

  // param.sfo with LibOrbisPkg's PUBTOOLINFO, keeping the dump's date.
  e = get_entry(p, 0x1000, &len);
  CHECK(e && len > 0);
  if (e) {
    char v[600];
    CHECK(sfo_get_string(e, len, "PUBTOOLINFO", v, sizeof(v)) == RIP_OK);
    CHECK(!strncmp(v, "c_date=20240102,c_time=030405,img0_l0_size=", 43));
    CHECK(sfo_get_string(e, len, "TITLE", v, sizeof(v)) == RIP_OK && !strcmp(v, "Ripper Test"));
  }
  free(e);

  // sce_sys files with entry names; the dump's playgo-chunk.dat is replaced.
  {
    static const struct {
      uint32_t id;
      const char *rel;
    } files[] = {{0x1200, "sce_sys/icon0.png"}, {0x1006, "sce_sys/pic1.png"}, {0x1400, "sce_sys/trophy/trophy00.trp"}};
    for (i = 0; i < RIP_ARRAY_LEN(files); i++) {
      char src[600];
      size_t sl = 0;
      unsigned char *want;
      snprintf(src, sizeof(src), "%s/%s", folder, files[i].rel);
      want = test_read_file(src, &sl);
      e = get_entry(p, files[i].id, &len);
      CHECK(e && want && len == sl && !memcmp(e, want, sl));
      free(e);
      free(want);
    }
    e = get_entry(p, 0x1001, &len);
    CHECK(e && len == 416 && !memcmp(e, "plgo", 4) && le64(e + 0x148) == p->len);
    free(e);
  }

  // PlayGo chunk hashes over the outer image.
  e = get_entry(p, 0x1002, &len);
  CHECK(e && len == p->len / BS * 4);
  if (e) {
    uint64_t c;
    for (c = pfs_off / BS; c < p->len / BS; c++) {
      sha(p->b + c * BS, BS, h);
      if (memcmp(h, e + 4 * c, 4) != 0) {
        CHECK(!"chunk hash");
        break;
      }
    }
  }
  free(e);

  // The outer image: decrypt, check signatures, find pfs_image.dat.
  {
    struct outer o;
    uint8_t seed[20];
    uint8_t key[32];
    struct rip_aes128 dec;
    struct rip_aes128 tw;
    uint64_t s;
    uint8_t hdr0[0x5A0];
    uint8_t *sr;
    uint8_t *ur;
    uint8_t *img;
    uint64_t sz;
    uint32_t uroot;
    uint32_t pino;

    memset(&o, 0, sizeof(o));
    o.blocks = pfs_size / BS;
    o.img = malloc((size_t)pfs_size);
    memcpy(o.img, p->b + pfs_off, (size_t)pfs_size);
    memset(seed, 0, sizeof(seed));
    rip_put_le32(seed, 1);
    rip_hmac_sha256(dk1, 32, seed, 20, key);
    rip_aes128_init_enc(&tw, key);
    rip_aes128_init_dec(&dec, key + 16);
    rip_put_le32(seed, 2);
    rip_hmac_sha256(dk1, 32, seed, 20, key);
    rip_hmac_sha256_init(&o.sign, key, 32);
    for (s = 16; s < pfs_size / 0x1000; s++) {
      if (s / 16 != 4) {
        rip_aes128_xts_decrypt(&dec, &tw, s, o.img + s * 0x1000, 0x1000);
      }
    }
    CHECK(le64(o.img + 8) == 20130315 && rip_le16(o.img + 0x1C) == 0xD && le64(o.img + 0x38) == o.blocks);
    memcpy(hdr0, o.img, sizeof(hdr0));
    memset(hdr0 + 0x380, 0, 36);
    {
      struct rip_hmac_sha256 hh = o.sign;
      rip_hmac_sha256_update(&hh, hdr0, sizeof(hdr0));
      rip_hmac_sha256_final(&hh, h);
      CHECK_MEM(h, o.img + 0x380, 32);
    }
    CHECK(sig_ok(&o, o.img + 0xB8, 1));
    sr = outer_file(&o, o.img + BS, &sz);  // superroot, inode 0
    uroot = find_dirent(sr, BS, "uroot");
    CHECK(uroot == 2);
    ur = outer_file(&o, o.img + BS + 0x2C8 * 2, &sz);
    pino = find_dirent(ur, BS, "pfs_image.dat");
    CHECK(pino == 3);
    img = outer_file(&o, o.img + BS + 0x2C8 * 3, &sz);
    CHECK(o.bad_sigs == 0);
    CHECK(rip_le32(o.img + BS + 0x2C8 * 3 + 4) == 0xD);  // compressed, signed

    // PFSC around the inner image, its blocks stored.
    CHECK(!memcmp(img, "PFSC", 4));
    {
      uint64_t start = le64(img + 0x20);
      uint64_t inner_len = le64(img + 0x28);
      struct inner in;
      memset(&in, 0, sizeof(in));
      CHECK(start + inner_len == sz && le64(img + 0x400) == start && le64(img + 0x408) == start + BS);
      in.img = img + start;
      in.size = inner_len;
      in.folder = folder;
      CHECK(le64(in.img + 8) == 20130315 && rip_le16(in.img + 0x1C) == 8 && le64(in.img + 0xD8) == 1);
      {
        const uint8_t *sri = inner_inode(&in, 0);
        const uint8_t *srd = in.img + (uint64_t)rip_le32(sri + 0x64) * BS;
        uint32_t ur_ino = find_dirent(srd, BS, "uroot");
        CHECK(ur_ino == 3);  // a collision resolver is inode 2
        CHECK(find_dirent(srd, BS, "collision_resolver") == 2);
        walk_inner(&in, ur_ino, "");
      }
      for (i = 0; i < RIP_ARRAY_LEN(IN_IMAGE); i++) {
        CHECK(in.found[i] == 1);
      }
      CHECK(in.many == MANY);
      CHECK(in.unexpected == 0);
      CHECK(in.mismatched == 0);
      CHECK(in.bad_fpt == 0);
      CHECK(in.files == (int)RIP_ARRAY_LEN(IN_IMAGE) + MANY);
      (void)NOT_IN_IMAGE;
    }
    free(sr);
    free(ur);
    free(img);
    free(o.img);
  }
}

static int cancel_at_half(void *ud, uint64_t done, uint64_t total) {
  (void)ud;
  return done * 2 >= total;
}

static void test_crypto(void) {
  static const uint8_t zero[32] = {0};
  uint8_t out[32];
  uint8_t want[32];
  uint8_t sector[32];
  struct rip_aes128 k1;
  struct rip_aes128 k2;
  struct rip_aes128 k1d;

  // RFC 4231 test case 2.
  rip_hmac_sha256("Jefe", 4, "what do ya want for nothing?", 28, out);
  unhex_or_die("5bdcc146bf60754e6a042426089575c75a003f089d2739839dec58b964ec3843", want, 32);
  CHECK_MEM(out, want, 32);
  // IEEE P1619 XTS-AES-128 vector 1.
  rip_aes128_init_enc(&k1, zero);
  rip_aes128_init_enc(&k2, zero);
  rip_aes128_init_dec(&k1d, zero);
  memset(sector, 0, sizeof(sector));
  rip_aes128_xts_encrypt(&k1, &k2, 0, sector, 32);
  unhex_or_die("917cf69ebd68b2ec9b9fe9a3eadda692cd43d2f59598ed858c02c2652fbf922e", want, 32);
  CHECK_MEM(sector, want, 32);
  rip_aes128_xts_decrypt(&k1d, &k2, 0, sector, 32);
  CHECK_MEM(sector, zero, 32);
}

static void test_key_file(const char *dir) {
  char path[256];
  char err[160];
  struct fpkg_keys k;
  FILE *f;

  snprintf(path, sizeof(path), "%s/bad.txt", dir);
  f = fopen(path, "w");
  fputs("# no keys\nPkgPublicKey0 = 00\n", f);
  fclose(f);
  CHECK(fpkg_keys_load(path, &k, err, sizeof(err)) == RIP_ERR);
  CHECK_STR(err, "the key file's PkgPublicKey0 isn't 256 bytes of hex");
  f = fopen(path, "w");
  fputs("Nonsense = 00\n", f);
  fclose(f);
  CHECK(fpkg_keys_load(path, &k, err, sizeof(err)) == RIP_ERR);
  CHECK_STR(err, "the key file has an unknown name, Nonsense");
  f = fopen(path, "w");
  fputs("\n", f);
  fclose(f);
  CHECK(fpkg_keys_load(path, &k, err, sizeof(err)) == RIP_ENOENT);
  CHECK_STR(err, "the key file has no PkgPublicKey0");
  unlink(path);
  snprintf(path, sizeof(path), "%s/missing.txt", dir);
  CHECK(fpkg_keys_load(path, &k, err, sizeof(err)) == RIP_ENOENT);
}

void test_fpkg(void) {
  char dir[64];
  char game[128];
  char keys[128];
  char out[128];
  char out2[128];
  char err[160];
  struct pkg p;
  size_t len2 = 0;
  unsigned char *b2;
  FILE *f;

  test_crypto();
  test_tmpdir(dir, sizeof(dir));
  test_key_file(dir);
  snprintf(game, sizeof(game), "%s/game", dir);
  snprintf(keys, sizeof(keys), "%s/keys.txt", dir);
  snprintf(out, sizeof(out), "%s/a.pkg", dir);
  snprintf(out2, sizeof(out2), "%s/b.pkg", dir);
  make_game(game);

  memset(&p, 0, sizeof(p));
  f = fopen(keys, "w");
  fputs("# throwaway test keys\n", f);
  fputs(TEST_KEYS, f);
  fclose(f);
  CHECK(fpkg_keys_load(keys, &p.k, err, sizeof(err)) == RIP_OK);
  CHECK(p.k.has_keystone_keys);
  unhex_or_die(PKG3_D, p.pkg3_d, 256);
  unhex_or_die(FAKE_D, p.fake_d, 256);

  CHECK(fpkg_from_folder(game, out, &p.k, NULL, NULL, err, sizeof(err)) == RIP_OK);
  p.b = test_read_file(out, &p.len);
  CHECK(p.b && p.len > 0x80000);
  if (p.b) {
    check_package(&p, game);
  }

  // The same folder makes the same bytes.
  CHECK(fpkg_from_folder(game, out2, &p.k, NULL, NULL, err, sizeof(err)) == RIP_OK);
  b2 = test_read_file(out2, &len2);
  CHECK(b2 && p.b && len2 == p.len && !memcmp(b2, p.b, p.len));
  free(b2);
  free(p.b);

  // Cancelled: nothing left behind.
  unlink(out2);
  CHECK(fpkg_from_folder(game, out2, &p.k, cancel_at_half, NULL, err, sizeof(err)) == RIP_ECANCEL);
  CHECK(access(out2, F_OK) != 0);

  // A folder that isn't a game.
  CHECK(fpkg_from_folder(dir, out2, &p.k, NULL, NULL, err, sizeof(err)) == RIP_ENOENT);
  CHECK_STR(err, "the game folder has no sce_sys folder");
  CHECK(access(out2, F_OK) != 0);

  unlink(out);
  unlink(keys);
  {
    char cmd[200];
    snprintf(cmd, sizeof(cmd), "rm -rf '%s'", dir);
    CHECK(system(cmd) == 0);
  }
}
