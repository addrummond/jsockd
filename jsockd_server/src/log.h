#ifndef LOG_H_
#define LOG_H_

#include <pthread.h>
#include <stdbool.h>
#include <stdio.h>

#define ISO8601_MAX_LEN 29

typedef enum { LOG_DEBUG, LOG_INFO, LOG_WARN, LOG_ERROR, LOG_INTERACTIVE = 0xF000000 } LogLevel;

void lock_log_mutex(void);
void unlock_log_mutex(void);
void jsockd_logf(LogLevel level, const char *fmt, ...);
void jsockd_log(LogLevel level, const char *s);
void print_log_prefix(LogLevel log_level, FILE *f, bool last_line);
void log_with_prefix_for_subsequent_lines(LogLevel log_level, FILE *fo,
                                          const char *buf, size_t len);

#endif
