#include "log.h"
#include "config.h"
#include "globals.h"
#include "utils.h"
#include <assert.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

static pthread_mutex_t log_mutex = PTHREAD_MUTEX_INITIALIZER;

void lock_log_mutex(void) { mutex_lock(&log_mutex); }

void unlock_log_mutex(void) { mutex_unlock(&log_mutex); }

void print_log_prefix(LogLevel log_level, FILE *f, bool last_line) {
  if (g_interactive_logging_mode)
    return;
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
  default:
    // High bytes should have been cleared before this function is called, so
    // previous values are all expected values.
    assert(0);
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
  if (!CMAKE_BUILD_TYPE_IS_DEBUG) {
    // nested if instead of && avoids weird clang warning about constant operand
    // of &&
    if ((log_level & 0xFF) == LOG_DEBUG)
      return;
  }
  if (g_interactive_logging_mode && 0 == (log_level & LOG_INTERACTIVE))
    return;

  log_level &= ~LOG_INTERACTIVE;

  char *log_buf = NULL;
  size_t log_buf_len = 0;
  FILE *memf = open_memstream(&log_buf, &log_buf_len);
  if (!memf)
    return;

  va_list args;
  va_start(args, fmt);
  vfprintf(memf, fmt, args);
  va_end(args);
  fclose(memf);

  int m = (int)MIN(log_buf_len, (size_t)(ABSOLUTE_MAX_LOG_BUF_SIZE - 1));
  m = remove_trailing_ws(log_buf, m);

  mutex_lock(&log_mutex);

  print_log_prefix(log_level, stderr, NULL == memchr(log_buf, '\n', (size_t)m));
  if (g_log_prefix)
    fprintf(stderr, "%s", g_log_prefix);
  log_with_prefix_for_subsequent_lines(log_level, stderr, log_buf, m);
  fputc('\n', stderr);
  fflush(stderr);

  mutex_unlock(&log_mutex);

  free(log_buf);
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
