#ifndef UTILS_H_
#define UTILS_H_

#ifndef _REENTRANT
#define _REENTRANT
#endif

#include "quickjs.h"
#include <pthread.h>
#include <stdint.h>
#include <time.h>

void mutex_lock_(pthread_mutex_t *m, const char *file, int line);
void mutex_unlock_(pthread_mutex_t *m, const char *file, int line);
void mutex_init_(pthread_mutex_t *m, const char *file, int line);
void munmap_or_warn(const void *addr, size_t length);
int64_t ns_time_diff(const struct timespec *t1, const struct timespec *t2);
void memswap_small(void *m1, void *m2, size_t size);
int string_ends_with(const char *str, const char *suffix);
int make_temp_dir(char out[], size_t out_size, const char *template);
void timespec_to_iso8601(const struct timespec *ts, char *buf, size_t buflen);
void dump_error(JSContext *ctx);

#define mutex_lock(m) mutex_lock_((m), __FILE__, __LINE__)
#define mutex_unlock(m) mutex_unlock_((m), __FILE__, __LINE__)
#define mutex_init(m) mutex_init_((m), __FILE__, __LINE__)

int write_all(int fd, const char *buf, size_t len);

typedef enum { READY, SIG_INTERRUPT_OR_ERROR, GO_AROUND } PollFdResult;
PollFdResult poll_fd(int fd, int timeout_ms);
PollFdResult ppoll_fd(int fd, const struct timespec *timeout);

typedef struct {
  char *buf;
  size_t index;
  size_t length;
} WBuf;
void write_to_wbuf(WBuf *buf, const char *inp, size_t size);

#define MIN(a, b) ((a) < (b) ? (a) : (b))
#define MAX(a, b) ((a) < (b) ? (b) : (a))

#define STRINGIFY_(x) #x
#define STRINGIFY(x) STRINGIFY_(x)

#if defined(__GNUC__) || defined(__clang__)
#define UNUSED __attribute__((unused))
#else
#define UNUSED
#endif

#ifdef CMAKE_BUILD_TYPE_DEBUG
#define CMAKE_BUILD_TYPE_IS_DEBUG 1
#else
#define CMAKE_BUILD_TYPE_IS_DEBUG 0
#endif

#ifdef CLOCK_MONOTONIC_RAW
#define MONOTONIC_CLOCK CLOCK_MONOTONIC_RAW
#else
#define MONOTONIC_CLOCK CLOCK_MONOTONIC
#endif

#endif
