#ifndef RIPPER_UTIL_H_
#define RIPPER_UTIL_H_

#include <stddef.h>
#include <stdint.h>

// The console libc has no pread/pwrite, so positioned I/O is lseek + read/write.
// Every helper loops until the whole buffer is transferred.

// Returns the number of bytes read (short only at end of file), or -1.
long rip_read_full(int fd, void *buf, size_t len);
int rip_write_all(int fd, const void *buf, size_t len);
int rip_seek_read(int fd, uint64_t off, void *buf, size_t len);
int rip_seek_write(int fd, uint64_t off, const void *buf, size_t len);

// Replaces a file's contents (written beside it, then renamed over it).
int rip_write_file(const char *path, const void *data, size_t len);

// Creates every missing directory in path. Existing directories are fine.
int rip_mkdir_p(const char *path);
int rip_file_exists(const char *path);

// Replaces characters that are unsafe in file names on exFAT/FAT32 ("A: B"
// becomes "A - B") and trims trailing dots and spaces. Never leaves an empty
// string. cap is the size of the buffer name is in.
void rip_sanitize_name(char *name, size_t cap);

// Lowercase hex, out must hold 2*len+1 bytes.
void rip_hex(const uint8_t *data, size_t len, char *out);
// Parses exactly 2*len hex characters. Returns 0 or -1.
int rip_unhex(const char *hex, uint8_t *out, size_t len);

uint64_t rip_now_ms(void);

#endif
