#include "utils.h"
#include <errno.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

pthread_mutex_t g_log_mutex;

void mutex_lock_(pthread_mutex_t *m, int line) {
  if (0 != pthread_mutex_lock(m)) {
    fprintf(stderr, "Failed to lock mutex on line %i: %s\n", line,
            strerror(errno));
    exit(1);
  }
}

void mutex_unlock_(pthread_mutex_t *m, int line) {
  if (0 != pthread_mutex_unlock(m)) {
    fprintf(stderr, "Failed to unlock mutex on line %i: %s\n", line,
            strerror(errno));
    exit(1);
  }
}

void mutex_init_(pthread_mutex_t *m, int line) {
  if (0 != pthread_mutex_init(m, NULL)) {
    fprintf(stderr, "Failed to initialized mutex on line %i: %s\n", line,
            strerror(errno));
    exit(1);
  }
}

void release_logf(const char *fmt, ...) {
  va_list args;
  va_start(args, fmt);

  mutex_lock(&g_log_mutex);
  vfprintf(stderr, fmt, args);
  mutex_unlock(&g_log_mutex);
}

void release_log(const char *s) { release_logf("%s", s); }

int write_all(int fd, const char *buf, size_t len) {
  while (len > 0) {
    int n = write(fd, buf, len);
    if (n < 0) {
      if (errno == EINTR)
        continue; // interrupted, try again
      return n;
    }
    len -= n;
    buf += n;
  }
  return 0;
}
