#ifndef LOG_H_
#define LOG_H_

#include <stdio.h>

#define ISO8601_MAX_LEN 29

typedef enum { LOG_INFO, LOG_WARN, LOG_ERROR } LogLevel;

void init_log_mutex(void);
void destroy_log_mutex(void);
void release_logf(LogLevel level, const char *fmt, ...);
void release_log(LogLevel level, const char *s);
void print_log_prefix(LogLevel log_level, FILE *f, int line);
void log_with_prefix_for_subsequent_lines(FILE *fo, const char *buf,
                                          size_t len);

#ifdef CMAKE_BUILD_TYPE_DEBUG
#define debug_logf(log_level, fmt, ...)                                        \
  release_logf((log_level), (fmt), __VA_ARGS__)
#define debug_log(log_level, s) release_logf((log_level), "%s", (s));
#else
#define debug_logf(log_level, fmt, ...) 0
#define debug_log(log_level, s) 0
#endif

#endif
