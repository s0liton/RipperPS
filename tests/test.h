#ifndef RIPPER_TEST_H_
#define RIPPER_TEST_H_

#include <stdio.h>
#include <string.h>

extern int test_failures;

#define CHECK(cond)                                                     \
  do {                                                                  \
    if (!(cond)) {                                                      \
      fprintf(stderr, "  FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
      test_failures++;                                                  \
    }                                                                   \
  } while (0)

#define CHECK_MEM(a, b, n) CHECK(memcmp((a), (b), (n)) == 0)
#define CHECK_STR(a, b) CHECK(strcmp((a), (b)) == 0)

// Writes a scratch directory path ("./test-tmp-XXXXXX") into dir.
void test_tmpdir(char *dir, size_t len);
// Reads a whole file; the caller frees the result.
unsigned char *test_read_file(const char *path, size_t *len);
// Builds a PARAM.SFO holding one string key; returns its length.
size_t test_make_sfo(unsigned char *out, const char *key, const char *value);

// Suites in other files.
void test_rip_all(void);
void test_formats(void);
void test_firmware(void);

#endif
