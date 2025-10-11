#include "log.h"
#include "config.h"
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

int init_log_mutex(void) {
  int r = pthread_mutex_init(&g_log_mutex, NULL);
  if (r != 0) {
    fprintf(stderr, "Failed to initalize log mutex\n");
    return r;
  }
  return 0;
}

void destroy_log_mutex(void) {
  if (0 != pthread_mutex_destroy(&g_log_mutex)) {
    fprintf(stderr, "Unable to destroy log mutex\n");
    exit(1);
  }
}

void print_log_prefix(LogLevel log_level, FILE *f, bool last_line) {
  struct timespec ts;
  char timebuf[ISO8601_MAX_LEN + 1];

  if (0 == clock_gettime(CLOCK_REALTIME, &ts)) {
    timespec_to_iso8601(&ts, timebuf, sizeof(timebuf) / sizeof(timebuf[0]));
  } else {
    strcpy(timebuf, "0000-00-00T00:00:00.000000Z");
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
  fprintf(stderr, "%s jsockd %s [%s] ", last_line ? "$" : "*", timebuf, ll);
}

static int remove_trailing_ws(const char *buf, int len) {
  while (--len >= 0) {
    if (buf[len] != ' ' && buf[len] != '\t' && buf[len] != '\n' &&
        buf[len] != '\r')
      break;
  }
  return len + 1;
}

void jsockd_logf(LogLevel log_level, const char *fmt, ...) {
  if (log_level == LOG_DEBUG && !CMAKE_BUILD_TYPE_IS_DEBUG)
    return;

  va_list args, args2;

  // Cannot use args twice in the two subsequent vsnprintf calls, so copy it.
  // https://stackoverflow.com/a/55274498/376854
  va_start(args, fmt);
  va_copy(args2, args);

  static char log_buf_[8192];
  char *log_buf = log_buf_;

  int n = vsnprintf(NULL, 0, fmt, args);
  va_end(args);

  if (n > ABSOLUTE_MAX_LOG_BUF_SIZE - 1)
    n = ABSOLUTE_MAX_LOG_BUF_SIZE - 1;

  if ((size_t)n > sizeof(log_buf_) / sizeof(log_buf_[0]))
    log_buf = (char *)calloc((size_t)n, sizeof(char));

  mutex_lock(&g_log_mutex);

  int m = vsnprintf(log_buf, n + 1, fmt, args2);
  va_end(args2);
  m = MIN(m, n);
  m = remove_trailing_ws(log_buf, m);

  print_log_prefix(log_level, stderr, NULL == memchr(log_buf, '\n', (size_t)m));
  log_with_prefix_for_subsequent_lines(log_level, stderr, log_buf, m);
  fputc('\n', stderr);
  fflush(stderr);

  if (log_buf != log_buf_)
    free(log_buf);

  mutex_unlock(&g_log_mutex);
}

void jsockd_log(LogLevel log_level, const char *s) {
  jsockd_logf(log_level, "%s", s);
}

void log_with_prefix_for_subsequent_lines(LogLevel log_level, FILE *fo,
                                          const char *buf, size_t len) {
  size_t start = 0;
  size_t i;
  for (i = 0; i < len; ++i) {
    if (buf[i] == '\n') {
      fwrite(buf + start, 1, i - start + 1, fo);
      start = i + 1;
      print_log_prefix(log_level, fo,
                       NULL == memchr(buf + start, '\n', len - start));
    }
  }
  fwrite(buf + start, 1, i - start, fo);
}
