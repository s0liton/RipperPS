#ifndef RIPPER_PS5_CAM_TRANSPORT_H_
#define RIPPER_PS5_CAM_TRANSPORT_H_

#include "ripper/transport.h"

// Opens the pass-through device behind the CAM peripheral "<periph><unit>"
// (e.g. "cd0") so MMC commands can be sent with CAMIOCOMMAND.
int cam_transport_open(struct rip_transport *t, const char *periph, unsigned unit);

#endif
