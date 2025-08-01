#include "utils.h"
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

const uint8_t *mmap_file(const char *filename, size_t *out_size) {
  int fd = open(filename, O_RDONLY);
  if (fd < 0) {
    release_logf("Error opening file for memory mapping %s: %s\n", filename,
                 strerror(errno));
    return NULL;
  }
  struct stat st;
  if (0 != fstat(fd, &st) || st.st_size == 0) {
    release_logf(
        "Error opening or file %s for memory mapping, or file empty: %s\n",
        filename, strerror(errno));
    close(fd);
    return NULL;
  }
  *out_size = st.st_size;
  const uint8_t *contents =
      (const uint8_t *)mmap(NULL, st.st_size, PROT_READ, MAP_PRIVATE, fd, 0);
  if (contents == MAP_FAILED) {
    release_logf("Error memory mapping file %s: %s\n", filename,
                 strerror(errno));
    close(fd);
    return NULL;
  }
  if (0 != close(fd)) {
    munmap((void *)contents, st.st_size);
    return NULL;
  }
  return contents;
}
