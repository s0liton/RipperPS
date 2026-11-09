#ifndef RIPPER_LINUX_SGIO_TRANSPORT_H_
#define RIPPER_LINUX_SGIO_TRANSPORT_H_

#include "ripper/transport.h"

// Sends MMC commands to a Linux optical drive (/dev/sr0) with the SG_IO ioctl.
int sgio_transport_open(struct rip_transport *t, const char *device);

#endif
