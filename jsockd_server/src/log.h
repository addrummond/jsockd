#ifndef LOG_H_
#define LOG_H_

#include <pthread.h>
#include <stdbool.h>
#include <stdio.h>

#define ISO8601_MAX_LEN 29

#if defined(__GNUC__) || defined(__clang__)
#define PRINTF_FMT(fmt_idx, first_arg_idx)                                     \
  __attribute__((format(printf, fmt_idx, first_arg_idx)))
#else
#define PRINTF_FMT(fmt_idx, first_arg_idx)
#endif

typedef enum {
  LOG_DEBUG,
  LOG_INFO,
  LOG_WARN,
  LOG_ERROR,
  LOG_INTERACTIVE = 0xF00000,
  LOG_DROPPABLE = 0xF000000
} LogLevel;

void lock_log_mutex(void);
void unlock_log_mutex(void);
void jsockd_logf(LogLevel level, const char *fmt, ...) PRINTF_FMT(2, 3);
void jsockd_log(LogLevel level, const char *s);
void print_log_prefix(LogLevel log_level, FILE *f, bool last_line);
void log_with_prefix_for_subsequent_lines(LogLevel log_level, FILE *fo,
                                          const char *buf, size_t len);

#endif
