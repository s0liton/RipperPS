#ifndef RIPPER_ROMM_H_
#define RIPPER_ROMM_H_

// Pairing the console with RomM (deliver_romm.c): RomM's device flow. The UI
// starts it, shows the code and link, and polls; the token comes back once,
// for the destination form to save.

#include <stddef.h>

#include "cJSON.h"

int romm_pair_start(const char *url, int tls_verify, char *err, size_t en);
// {"status": "idle|pending|ok|error", "message", "user_code", "verification_url", "token" (once, when ok)}
cJSON *romm_pair_status(void);
void romm_pair_cancel(void);
cJSON *romm_rommps(void);
#endif
