/* config.h for ripper's build of libdvdcss (no autotools). The CSS commands
 * are sent by ripper's transport (src/core/css.c), not by ioctl.c, which is
 * left out. */
#define HAVE_ERRNO_H 1
#define HAVE_SYS_PARAM_H 1
#define HAVE_SYS_UIO_H 1
#define HAVE_UNISTD_H 1
#define DVDCSS_RIPPER 1
#ifndef O_BINARY
#define O_BINARY 0
#endif
