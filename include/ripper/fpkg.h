#ifndef RIPPER_FPKG_H_
#define RIPPER_FPKG_H_

#include <stddef.h>
#include <stdint.h>

#include "ripper/iso9660.h"

// PS4 fake packages (fPKGs): a PS4 game folder (decrypted, fake-signed
// executables plus sce_sys/param.sfo and the rest) as the .pkg that GoldHEN on
// a PS4, and etaHEN or ShadowMountPlus on a PS5, install. The layout is that of
// LibOrbisPkg's PkgTool (OpenOrbis fork, LGPL-3.0), ported to C: a "\x7FCNT"
// container whose entries carry the keys, a debug license and the sce_sys files,
// then an outer PFS image, signed and AES-XTS encrypted, holding one file,
// pfs_image.dat: a PFSC wrapper whose blocks are stored, not compressed, around
// an unsigned PFS image of the game.
//
// The keys come from a file the user supplies; RipperPS ships none. It is plain
// text, one "Name = hex" per line, with "#" starting a comment. Names are
// matched without regard to case, and the hex may have spaces in it. Every name
// below is needed unless marked optional; the lengths are in bytes.
//
//   PkgPublicKey0 ... PkgPublicKey6  256  the package keys' RSA moduli (e = 65537),
//                                         LibOrbisPkg's Keys.PkgPublicKeys[0..6]
//   FakeKeyModulus                   256  RSAKeyset.FakeKeyset.Modulus, which the
//                                         image key is wrapped under
//   DebugRifModulus                  256  RSAKeyset.DebugRifKeyset.Modulus
//   DebugRifPrivateExponent          256  RSAKeyset.DebugRifKeyset.PrivateExponent,
//                                         which signs the debug license
//   RifDebugKey                       16  Keys.rif_debug_key, the license secret's key
//   KeystoneHmacKey                   32  Keys.keystone_hmac_key (optional)
//   KeystoneMacData                   32  Keys.keystone_mac_data (optional)
//
// The two keystone keys are needed only for a folder without sce_sys/keystone.

struct fpkg_keys {
  uint8_t pkg_public[7][256];
  uint8_t fake_modulus[256];
  uint8_t rif_modulus[256];
  uint8_t rif_private[256];
  uint8_t rif_debug_key[16];
  uint8_t keystone_hmac_key[32];
  uint8_t keystone_mac_data[32];
  int has_keystone_keys;
};

// RIP_OK, or RIP_ERR / RIP_ENOENT with the reason in err.
int fpkg_keys_load(const char *path, struct fpkg_keys *out, char *err, size_t en);

// Builds a game (GD) package at out_path from folder. The content ID comes from
// sce_sys/param.sfo; the passcode is all zeros, as every fake package's is.
// RIP_OK; RIP_ECANCEL when progress asked to stop; otherwise an error with the
// reason in err. Anything written is removed unless RIP_OK.
int fpkg_from_folder(const char *folder, const char *out_path, const struct fpkg_keys *keys, iso_progress_fn progress,
                     void *ud, char *err, size_t en);

#endif
