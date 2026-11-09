#ifndef RIPPER_GAME_H_
#define RIPPER_GAME_H_

#include <stdint.h>

#include "ripper/sce_elf.h"

// After a PS4/PS5 game is dumped (decrypted ELFs, sce_sys): readies the
// folder for where it will run. An emulator takes it as it is; a jailbroken
// console wants its ELFs fake-signed, and, on older firmware, backported.

struct game_finish {
  int fself;                 // wrap every ELF as an FSELF
  enum sce_target target;    // ... for this console
  uint32_t backport_ps4;     // 0: leave the PS4 SDK version alone
  uint32_t backport_ps5;     // 0: leave the PS5 SDK version alone
};

typedef int (*game_progress_fn)(void *ud, unsigned done, unsigned total);  // nonzero stops

// RIP_OK; *elfs (if not NULL) gets how many ELFs were handled.
int game_finish_folder(const char *dir, const struct game_finish *opts, game_progress_fn progress, void *ud,
                       unsigned *elfs);

#endif
