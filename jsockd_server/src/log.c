#include "log.h"
#include "utils.h"
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#ifdef CMAKE_BUILD_TYPE_DEBUG
#define CMAKE_BUILD_TYPE_IS_DEBUG 1
#else
#define CMAKE_BUILD_TYPE_IS_DEBUG 0
#endif

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

void print_log_prefix(LogLevel log_level, FILE *f, int line) {
  if (log_level == LOG_DEBUG && !CMAKE_BUILD_TYPE_IS_DEBUG)
    return;

  struct timespec ts;
  char timebuf[ISO8601_MAX_LEN];

  if (0 == clock_gettime(CLOCK_REALTIME, &ts)) {
    timespec_to_iso8601(&ts, timebuf, sizeof(timebuf) / sizeof(timebuf[0]));
  } else {
    strcpy(timebuf, "<unknown time>");
  }

  const char *ll = "";
  switch (log_level) {
  case LOG_DEBUG:
    ll = "DEBUG";
    break;
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

void jsockd_logf(LogLevel log_level, const char *fmt, ...) {
  va_list args, args2;

  // Cannot use args twice in the two subsequent vsnprintf calls, so copy it.
  // https://stackoverflow.com/a/55274498/376854
  va_start(args, fmt);
  va_copy(args2, args);

  static char log_buf_[8192];
  char *log_buf = log_buf_;

  int n = vsnprintf(NULL, 0, fmt, args);
  va_end(args);

  if ((size_t)n > sizeof(log_buf_) / sizeof(log_buf_[0]))
    log_buf = (char *)calloc((size_t)n, sizeof(char));

  if (g_log_mutex_initialized)
    mutex_lock(&g_log_mutex);

  vsnprintf(log_buf, n, fmt, args2);
  va_end(args2);

  print_log_prefix(log_level, stderr, 1);
  log_with_prefix_for_subsequent_lines(
      log_level, stderr, log_buf,
      (size_t)(n - 1)); // n includes null terminator
  fputc('\n', stderr);

  if (log_buf != log_buf_)
    free(log_buf);

  if (g_log_mutex_initialized)
    mutex_unlock(&g_log_mutex);
}

void jsockd_log(LogLevel log_level, const char *s) {
  jsockd_logf(log_level, "%s", s);
}

static size_t remove_trailing_ws(const char *buf, size_t len) {
  if (len == 0)
    return 0;
  --len;
  do {
    if (buf[len] != ' ' && buf[len] != '\t' && buf[len] != '\n' &&
        buf[len] != '\r')
      break;
  } while (len-- > 0);
  return len + 1;
}

void log_with_prefix_for_subsequent_lines(LogLevel log_level, FILE *fo,
                                          const char *buf, size_t len) {
  len = remove_trailing_ws(buf, len);

  int line = 1;
  size_t start = 0;
  size_t i;
  for (i = 0; i < len; ++i) {
    if (buf[i] == '\n') {
      fwrite(buf + start, 1, i - start + 1, fo);
      ++line;
      start = i + 1;
      print_log_prefix(log_level, fo, line);
    }
  }
  fwrite(buf + start, 1, i - start, fo);
}
