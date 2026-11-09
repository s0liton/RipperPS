#ifndef RIPPER_DELIVER_H_
#define RIPPER_DELIVER_H_

// Getting finished rips to where they belong. Local destinations are written
// in place by the rip itself; the others get the files from a staging folder:
//
//   smb   libsmb2, straight to the share (no mount needed)
//   nfs   libnfs, the same
//   ftp   libcurl, FTP or FTPS
//   romm  RomM's chunked upload, then a scan of the platform
//
// Files go up as "<name>.part" and are renamed when complete, so a library
// scanner never picks up half a movie.

#include <stddef.h>

#include "cJSON.h"
#include "svc.h"

struct deliver_req {
  const cJSON *dest;
  const char *dir;           // where the files are
  const char *const *files;  // names in dir
  int count;
  const char *folder;         // relative folder under the destination's path
  const char *name;           // the rip's name: a RomM upload of several files is a zip of this name
  const char *romm_platform;  // RomM platform slug
  svc_progress_fn progress;   // bytes over all files
  void *ud;
};

// where: what the user is shown afterwards (a path, a share, a RomM game).
int deliver_run(const struct deliver_req *r, char *where, size_t wn, char *err, size_t en);
// Connects and checks the folder can be written. msg says what was found.
int deliver_test(const cJSON *dest, char *msg, size_t n);
int deliver_is_local(const cJSON *dest);

// The backends, each in its own file.
int deliver_ftp(const struct deliver_req *r, char *where, size_t wn, char *err, size_t en);
int deliver_ftp_test(const cJSON *dest, char *msg, size_t n);
int deliver_smb(const struct deliver_req *r, char *where, size_t wn, char *err, size_t en);
int deliver_smb_test(const cJSON *dest, char *msg, size_t n);
int deliver_nfs(const struct deliver_req *r, char *where, size_t wn, char *err, size_t en);
int deliver_nfs_test(const cJSON *dest, char *msg, size_t n);
int deliver_romm(const struct deliver_req *r, char *where, size_t wn, char *err, size_t en);
int deliver_romm_test(const cJSON *dest, char *msg, size_t n);

#endif
