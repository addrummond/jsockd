#ifndef UTILS_H_
#define UTILS_H_

#include <pthread.h>
#include <stdio.h>

extern pthread_mutex_t g_log_mutex;

void mutex_lock_(pthread_mutex_t *m, const char *file, int line);
void mutex_unlock_(pthread_mutex_t *m, const char *file, int line);
void mutex_init_(pthread_mutex_t *m, const char *file, int line);
void release_logf(const char *fmt, ...);
void release_log(const char *s);

#define mutex_lock(m) mutex_lock_((m), __FILE__, __LINE__)
#define mutex_unlock(m) mutex_unlock_((m), __FILE__, __LINE__)
#define mutex_init(m) mutex_init_((m), __FILE__, __LINE__)

int write_all(int fd, const char *buf, size_t len);

#ifdef CMAKE_BUILD_TYPE_DEBUG
#define debug_logf(fmt, ...) release_logf((fmt), __VA_ARGS__)
#define debug_log(s) release_logf("%s", (s));
#else
#define debug_logf(fmt, ...)                                                   \
  do {                                                                         \
  } while (0)
#define debug_log(s)                                                           \
  do {                                                                         \
  } while (0)
#endif

#define MIN(a, b) ((a) < (b) ? (a) : (b))

#define STRINGIFY_(x) #x
#define STRINGIFY(x) STRINGIFY_(x)

#if defined(__GNUC__) || defined(__clang__)
#define UNUSED __attribute__((unused))
#else
#define UNUSED
#endif

#endif
