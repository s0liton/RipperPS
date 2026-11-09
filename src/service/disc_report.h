#ifndef RIPPER_DISC_REPORT_H_
#define RIPPER_DISC_REPORT_H_

// Every disc that goes in leaves a short report, so the discs testers try
// (and drives ripper hasn't met) can be looked at afterwards: what the drive
// and ripper made of the disc, what ripper offered or why it couldn't, the
// disc's first sectors and its files. Only reads, a second or two.
//
// Kept in <data>/diag/discs/<time>-<drive>-<name>/ beside the drive tests;
// reports_bundle() zips them all with ripper.log, to send back.

#include <stddef.h>
#include <stdint.h>

#include "cJSON.h"
#include "ripper/disc.h"
#include "ripper/drive.h"
#include "ripper/reader.h"
#include "ripper/transport.h"

struct disc_report {
  const char *drive_id;         // "cd1"
  const char *drive_label;      // "USB drive"
  struct rip_drive *drive;      // not const: Kreon is unlocked to read an Xbox disc's security sector
  struct rip_transport *t;      // the caller holds the drive while this reads
  struct rip_reader *reader;    // NULL when the disc can't be read by sector
  const struct rip_disc *disc;  // NULL when the disc never got ready
  uint16_t profile;
  const char *outcome;     // what the UI said: "ready to rip", or why not
  const cJSON *offer;      // NULL for none
  const char *mount_root;  // where the system mounted the disc, or NULL
};

void disc_report_write(const struct disc_report *r);

// The disc reports, the drive tests and ripper.log as one zip; NULL if it
// couldn't be made. free() it.
char *reports_bundle(size_t *len);

#endif
