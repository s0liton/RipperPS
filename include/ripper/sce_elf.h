#ifndef RIPPER_SCE_ELF_H_
#define RIPPER_SCE_ELF_H_

#include <stdint.h>

// What a decrypted PS4/PS5 game needs to run somewhere other than an
// emulator: its ELFs wrapped as fake-signed SELFs (FSELF) that a jailbroken
// console loads, and optionally "backported", their SDK version lowered so
// a console on older firmware accepts them.
//
// The FSELF layout follows OpenOrbis' make_fself and EchoStretch's
// ps5-app-dumper: two entries per loadable segment (a SHA-256 digest table
// per 16 KiB block, then the segment itself, unencrypted), the ELF and
// program headers, extended info with the ELF's digest, an NPDRM block and
// an empty signature.

#define SCE_SELF_PS4_MAGIC 0x1D3D154Fu  // "4F 15 3D 1D" on disk
#define SCE_SELF_PS5_MAGIC 0xEEF51454u  // "54 14 F5 EE"

enum sce_target {
  SCE_TARGET_PS4,  // a PS4 console's loader (GoldHEN)
  SCE_TARGET_PS5   // a PS5 console's loaders (etaHEN, ShadowMountPlus, itemzflow)
};

int sce_is_elf(const char *path);
int sce_is_self(const char *path);
// Writes elf_path as an FSELF at out_path. RIP_OK, RIP_ENOTSUP for a file
// that isn't an ELF, RIP_ERR otherwise.
int sce_write_fself(const char *elf_path, const char *out_path, enum sce_target target);
// An FSELF written for one console, made the other's: the two differ only in
// their magic. Only fake-signed SELFs (key type 0x101) are touched. 1 when it
// changed, 0 when it was already right, -1 when it isn't one or can't be written.
int sce_retarget_fself(const char *path, enum sce_target target);

// Backports. Each returns 1 when it changed something, 0 when the file was
// already at or below the target, negative on an error. A version of 0
// leaves that field alone.
int sce_backport_elf(const char *path, uint32_t ps4_sdk, uint32_t ps5_sdk);
int sce_backport_sfo(const char *path, uint32_t ps4_sdk);   // PUBTOOLINFO sdk_ver, SYSTEM_VER
int sce_backport_json(const char *path, uint32_t ps5_sdk);  // requiredSystemSoftwareVersion, sdkVersion

// The firmware a backport can aim at, by id ("ps4-9.00", "ps5-4"), and the
// SDK versions that means. RIP_ENOENT for an unknown id.
int sce_backport_target(const char *id, uint32_t *ps4_sdk, uint32_t *ps5_sdk);

#endif
