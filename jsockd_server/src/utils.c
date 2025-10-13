#include "utils.h"
#include "log.h"
#include <errno.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

void mutex_lock_(pthread_mutex_t *m, const char *file, int line) {
  if (0 != pthread_mutex_lock(m)) {
    fprintf(stderr, "Failed to lock mutex at %s:%i: %s\n", file, line,
            strerror(errno));
    exit(1);
  }
}

void mutex_unlock_(pthread_mutex_t *m, const char *file, int line) {
  if (0 != pthread_mutex_unlock(m)) {
    fprintf(stderr, "Failed to unlock mutex at %s:%i: %s\n", file, line,
            strerror(errno));
    exit(1);
  }
}

void mutex_init_(pthread_mutex_t *m, const char *file, int line) {
  if (0 != pthread_mutex_init(m, NULL)) {
    fprintf(stderr, "Failed to initialized mutex at %s:%i: %s\n", file, line,
            strerror(errno));
    exit(1);
  }
}

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

void munmap_or_warn(const void *addr, size_t length) {
  if (munmap((void *)addr, length) < 0) {
    jsockd_logf(LOG_DEBUG, "Error unmapping memory at %p of size %zu: %s\n",
                addr, length, strerror(errno));
  }
}

int64_t ns_time_diff(const struct timespec *t1, const struct timespec *t2) {
  int64_t tv_sec1 = (int64_t)t1->tv_sec;
  int64_t tv_sec2 = (int64_t)t2->tv_sec;

  if (tv_sec1 < tv_sec2) {
    tv_sec2 -= tv_sec1;
    tv_sec1 = 0;
  } else {
    tv_sec1 -= tv_sec2;
    tv_sec2 = 0;
  }

  int64_t ns1 = (int64_t)(tv_sec1 * 1000000ULL * 1000ULL + t1->tv_nsec);
  int64_t ns2 = (int64_t)(tv_sec2 * 1000000ULL * 1000ULL + t2->tv_nsec);
  return ns1 - ns2;
}

// swap two small blocks of memory of a given size
void memswap_small(void *m1, void *m2, size_t size) {
  char tmp[size / sizeof(char)];
  memcpy(tmp, m1, size);
  memcpy(m1, m2, size);
  memcpy(m2, tmp, size);
}

int make_temp_dir(char out[], size_t out_size, const char *template) {
  const char *TMPDIR = getenv("TMPDIR");
  if (!TMPDIR)
    TMPDIR = "/tmp";
  if (strlen(TMPDIR) >=
      out_size - strlen(template) - 1 - 1) // -1 for '/', -1 for '\0'
    return -1;
  snprintf(out, out_size, "%s/%s", TMPDIR, template);
  if (!mkdtemp(out))
    return -1;
  return 0;
}

static long ns_to_us(long ns) { return (ns / 1000) + (ns % 1000 >= 500); }

void timespec_to_iso8601(const struct timespec *ts, char *buf, size_t buflen) {
  struct tm tm_utc;
  // Convert seconds to UTC time structure
  if (gmtime_r(&ts->tv_sec, &tm_utc) == NULL) {
    buf[0] = '\0';
    return;
  }
  // Format date and time (without fractional seconds)
  size_t len = strftime(buf, buflen, "%Y-%m-%dT%H:%M:%S", &tm_utc);
  if (len == 0) {
    buf[0] = '\0';
    return;
  }
  // Append fractional seconds and 'Z'
  // Ensure enough space for .nnnnnnnnnZ and null terminator
  if (len + 8 < buflen)
    snprintf(buf + len, buflen - len, ".%06ldZ", ns_to_us(ts->tv_nsec));
}
