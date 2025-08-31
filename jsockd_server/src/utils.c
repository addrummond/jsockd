#include "utils.h"
#include <errno.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

atomic_bool g_in_signal_handler;

static pthread_mutex_t g_log_mutex;
static atomic_bool g_log_mutex_initialized;

void init_log_mutex(void) {
  int r = pthread_mutex_init(&g_log_mutex, NULL);
  if (r != 0) {
    if (!atomic_load_explicit(&g_in_signal_handler, memory_order_relaxed))
      fprintf(stderr, "Failed to initalize log mutex\n");
    exit(1);
  }
  atomic_store_explicit(&g_log_mutex_initialized, true, memory_order_relaxed);
}

void destroy_log_mutex(void) {
  atomic_store_explicit(&g_log_mutex_initialized, false, memory_order_relaxed);
  if (0 != pthread_mutex_destroy(&g_log_mutex)) {
    if (!atomic_load_explicit(&g_in_signal_handler, memory_order_relaxed))
      fprintf(stderr, "Unable to destroy log mutex\n");
    exit(1);
  }
}

void mutex_lock_(pthread_mutex_t *m, const char *file, int line) {
  if (0 != pthread_mutex_lock(m)) {
    if (!atomic_load_explicit(&g_in_signal_handler, memory_order_relaxed))
      fprintf(stderr, "Failed to lock mutex at %s:%i: %s\n", file, line,
              strerror(errno));
    exit(1);
  }
}

void mutex_unlock_(pthread_mutex_t *m, const char *file, int line) {
  if (0 != pthread_mutex_unlock(m)) {
    if (!atomic_load_explicit(&g_in_signal_handler, memory_order_relaxed))
      fprintf(stderr, "Failed to unlock mutex at %s:%i: %s\n", file, line,
              strerror(errno));
    exit(1);
  }
}

void mutex_init_(pthread_mutex_t *m, const char *file, int line) {
  if (0 != pthread_mutex_init(m, NULL)) {
    if (!atomic_load_explicit(&g_in_signal_handler, memory_order_relaxed))
      fprintf(stderr, "Failed to initialized mutex at %s:%i: %s\n", file, line,
              strerror(errno));
    exit(1);
  }
}

void release_logf(const char *fmt, ...) {
  if (atomic_load_explicit(&g_in_signal_handler, memory_order_relaxed))
    return;
  va_list args;
  va_start(args, fmt);

  if (g_log_mutex_initialized)
    mutex_lock(&g_log_mutex);
  vfprintf(stderr, fmt, args);
  if (g_log_mutex_initialized)
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

void munmap_or_warn(const void *addr, size_t length) {
  if (munmap((void *)addr, length) < 0) {
    debug_logf("Error unmapping memory at %p of size %zu: %s\n", addr, length,
               strerror(errno));
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
