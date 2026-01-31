#include "mmap_file.h"
#include "typeofshim.h"
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

const uint8_t *mmap_file(const char *filename, size_t *out_size,
                         int *out_errno) {
  *out_errno = 0;
  int fd = open(filename, O_RDONLY);
  if (fd < 0) {
    *out_errno = errno;
    return NULL;
  }
  struct stat st;
  if (0 != fstat(fd, &st)) {
    *out_errno = errno;
    close(fd);
    return NULL;
  }
  if (!S_ISREG(st.st_mode) || st.st_size <= 0 ||
      st.st_size > ((TYPEOF(st.st_size))(SIZE_MAX))) {
    *out_errno = EINVAL;
    close(fd);
    return NULL;
  }
  *out_size = (size_t)st.st_size;
  const uint8_t *contents = (const uint8_t *)mmap(
      NULL, (size_t)st.st_size, PROT_READ, MAP_PRIVATE, fd, 0);
  if (contents == MAP_FAILED) {
    *out_errno = errno;
    munmap((void *)contents, (size_t)st.st_size);
    close(fd);
    return NULL;
  }
  close(fd);
  return contents;
}
