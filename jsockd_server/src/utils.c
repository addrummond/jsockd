#include "utils.h"
#include <errno.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

pthread_mutex_t g_log_mutex;
atomic_bool g_log_mutex_initialized = false;

void init_log_mutex(void) {
  int r = pthread_mutex_init(&g_log_mutex, NULL);
  if (r != 0) {
    fprintf(stderr, "Failed to initalize log mutex\n");
    exit(1);
  }
  atomic_store_explicit(&g_log_mutex_initialized, true, memory_order_relaxed);
}

void destroy_log_mutex(void) {
  atomic_store_explicit(&g_log_mutex_initialized, false, memory_order_relaxed);
  if (0 != pthread_mutex_destroy(&g_log_mutex)) {
    fprintf(stderr, "Unable to destroy log mutex\n");
    exit(1);
  }
}

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

void print_log_prefix(LogLevel log_level, FILE *f, int line) {
  struct timespec ts;
  if (0 == clock_gettime(CLOCK_REALTIME, &ts)) {
    char timebuf[ISO8601_MAX_LEN];
    timespec_to_iso8601(&ts, timebuf, sizeof(timebuf) / sizeof(timebuf[0]));
    const char *ll = "";
    switch (log_level) {
    case LOG_INFO:
      ll = "INFO";
      break;
    case LOG_WARN:
      ll = "WARN";
      break;
    case LOG_ERROR:
      ll = "ERROR";
      break;
    }
    fprintf(stderr, "%s jsockd %s [%s] ", line == 1 ? "*" : ".", timebuf, ll);
  }
}

void release_logf(LogLevel log_level, const char *fmt, ...) {
  va_list args;
  va_start(args, fmt);

  if (g_log_mutex_initialized)
    mutex_lock(&g_log_mutex);
  print_log_prefix(log_level, stderr, 1);
  vfprintf(stderr, fmt, args);
  if (g_log_mutex_initialized)
    mutex_unlock(&g_log_mutex);
}

void release_log(LogLevel log_level, const char *s) {
  release_logf(log_level, "%s", s);
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
    debug_logf(LOG_ERROR, "Error unmapping memory at %p of size %zu: %s\n",
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

static long ns_to_ms(long ns) { return (ns / 1000) + (ns % 1000 >= 500); }

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
    snprintf(buf + len, buflen - len, ".%06ldZ", ns_to_ms(ts->tv_nsec));
}
