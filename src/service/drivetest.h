#ifndef RIPPER_DRIVETEST_H_
#define RIPPER_DRIVETEST_H_

// The drive test: a guided session for people helping out with drives ripper
// hasn't met (Kreon, OmniDrive, LibreDrive, anything). It tests the drive,
// then waits for discs: each one put in is tested as it comes (the drive test
// of core diag.c, and the disc report the watcher writes anyway), and the
// person is told when to eject it and put in the next. They can say what each
// disc really is, which is half the point: what ripper made of a disc against
// what it is. Finish packs it all into one zip in <data>/diag/, to attach to
// the drive report form on GitHub. One session at a time.
//
// States: idle, testing (a disc, or the drive alone), waiting (for a disc),
// finishing, done, failed. "prompt" says what to do next in words both UIs
// show as they are.

#include <stddef.h>

#include "cJSON.h"

int drivetest_start(const char *drive_id, char *err, size_t en);
// What the person says disc n (from 1) is: one of the catalog's systems or
// free text, and a note.
int drivetest_label(int n, const char *said, const char *note, char *err, size_t en);
// Stops a disc being tested (what it got so far is kept) and packs the zip.
int drivetest_finish(char *err, size_t en);
// Stops and throws the session away.
void drivetest_discard(void);
cJSON *drivetest_status(void);
// A finished zip's bytes by its name (free it), NULL if there's no such file.
char *drivetest_file(const char *name, size_t *len);
// Nonzero while a session follows this drive: nothing starts a rip on it.
int drivetest_holds(const char *drive_id);

#endif
