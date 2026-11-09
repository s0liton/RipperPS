#include "ripper/util.h"

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

long rip_read_full(int fd, void *buf, size_t len) {
  size_t got = 0;

  while (got < len) {
    ssize_t n = read(fd, (uint8_t *)buf + got, len - got);
    if (n < 0) {
      if (errno == EINTR) {
        continue;
      }
      return -1;
    }
    if (n == 0) {
      break;
    }
    got += (size_t)n;
  }
  return (long)got;
}

int rip_write_all(int fd, const void *buf, size_t len) {
  size_t put = 0;

  while (put < len) {
    ssize_t n = write(fd, (const uint8_t *)buf + put, len - put);
    if (n < 0) {
      if (errno == EINTR) {
        continue;
      }
      return -1;
    }
    put += (size_t)n;
  }
  return 0;
}

int rip_seek_read(int fd, uint64_t off, void *buf, size_t len) {
  if (lseek(fd, (off_t)off, SEEK_SET) < 0) {
    return -1;
  }
  return rip_read_full(fd, buf, len) == (long)len ? 0 : -1;
}

int rip_seek_write(int fd, uint64_t off, const void *buf, size_t len) {
  if (lseek(fd, (off_t)off, SEEK_SET) < 0) {
    return -1;
  }
  return rip_write_all(fd, buf, len);
}

int rip_write_file(const char *path, const void *data, size_t len) {
  char tmp[1100];
  int fd;
  int ok;

  snprintf(tmp, sizeof(tmp), "%s.tmp", path);
  fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC, 0644);
  if (fd < 0) {
    return -1;
  }
  ok = rip_write_all(fd, data, len) == 0;
  ok = close(fd) == 0 && ok;
  if (!ok || rename(tmp, path) != 0) {
    unlink(tmp);
    return -1;
  }
  return 0;
}

int rip_mkdir_p(const char *path) {
  char tmp[1024];
  size_t n = strlen(path);
  size_t i;

  if (n == 0 || n >= sizeof(tmp)) {
    return -1;
  }
  memcpy(tmp, path, n + 1);
  for (i = 1; i <= n; i++) {
    if (tmp[i] == '/' || tmp[i] == '\0') {
      char saved = tmp[i];
      tmp[i] = '\0';
      if (mkdir(tmp, 0777) != 0 && errno != EEXIST) {
        return -1;
      }
      tmp[i] = saved;
    }
  }
  return 0;
}

int rip_file_exists(const char *path) {
  struct stat st;

  return stat(path, &st) == 0;
}

void rip_sanitize_name(char *name, size_t cap) {
  char out[512];
  size_t o = 0;
  size_t n;
  const char *p;

  for (p = name; *p && o + 4 < sizeof(out); p++) {
    unsigned char c = (unsigned char)*p;
    if (c == ':' && p[1] == ' ') {
      // "Fallout: New Vegas" -> "Fallout - New Vegas", as Redump names them.
      out[o++] = ' ';
      out[o++] = '-';
    } else if (c < 0x20 || strchr("<>:\"/\\|?*", c)) {
      out[o++] = c == ':' ? '-' : '_';
    } else {
      out[o++] = (char)c;
    }
  }
  out[o] = '\0';
  n = strlen(out);
  while (n > 0 && (out[n - 1] == '.' || out[n - 1] == ' ')) {
    out[--n] = '\0';
  }
  if (cap == 0) {
    return;
  }
  snprintf(name, cap, "%s", n ? out : "disc");
}

void rip_hex(const uint8_t *data, size_t len, char *out) {
  static const char digits[] = "0123456789abcdef";
  size_t i;

  for (i = 0; i < len; i++) {
    out[2 * i] = digits[data[i] >> 4];
    out[2 * i + 1] = digits[data[i] & 0x0F];
  }
  out[2 * len] = '\0';
}

static int hex_digit(char c) {
  if (c >= '0' && c <= '9') {
    return c - '0';
  }
  c = (char)tolower((unsigned char)c);
  if (c >= 'a' && c <= 'f') {
    return c - 'a' + 10;
  }
  return -1;
}

int rip_unhex(const char *hex, uint8_t *out, size_t len) {
  size_t i;

  for (i = 0; i < len; i++) {
    int hi = hex_digit(hex[2 * i]);
    int lo = hi < 0 ? -1 : hex_digit(hex[2 * i + 1]);
    if (lo < 0) {
      return -1;
    }
    out[i] = (uint8_t)((hi << 4) | lo);
  }
  return 0;
}

uint64_t rip_now_ms(void) {
  struct timespec ts;

  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (uint64_t)ts.tv_sec * 1000u + (uint64_t)ts.tv_nsec / 1000000u;
}
