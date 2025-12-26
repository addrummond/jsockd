#include "utils.h"
#include "globals.h"
#include "log.h"
#include "quickjs-libc.h"
#include <errno.h>
#ifdef LINUX
#define _GNU_SOURCE // make ppoll available
#endif
#include <poll.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

void mutex_lock_(pthread_mutex_t *m, const char *file, int line) {
  int r;
  if (0 != (r = pthread_mutex_lock(m))) {
    fprintf(stderr, "Failed to lock mutex at %s:%i: %s\n", file, line,
            strerror(r));
    exit(1);
  }
}

void mutex_unlock_(pthread_mutex_t *m, const char *file, int line) {
  int r;
  if (0 != (r = pthread_mutex_unlock(m))) {
    fprintf(stderr, "Failed to unlock mutex at %s:%i: %s\n", file, line,
            strerror(r));
    exit(1);
  }
}

void mutex_init_(pthread_mutex_t *m, const char *file, int line) {
  int r;
  if (0 != (r = pthread_mutex_init(m, NULL))) {
    fprintf(stderr, "Failed to initialized mutex at %s:%i: %s\n", file, line,
            strerror(r));
    exit(1);
  }
}

int write_all(int fd, const char *buf, size_t len) {
  while (len > 0) {
    int n = write(fd, buf, len);
    if (n <= 0) {
      if (errno == EINTR)
        continue; // interrupted, try again
      return n;
    }
    len -= n;
    buf += n;
  }
  return 0;
}

int writev_all(int fildes, struct iovec *iov, int iovcnt) {
  if (iovcnt <= 0)
    return 0;

  ssize_t total_len = 0;
  for (int i = 0; i < iovcnt; i++) {
    total_len += (ssize_t)iov[i].iov_len;
  }
  if (total_len == 0)
    return 0;

  while (iovcnt > 0) {
    ssize_t n = writev(fildes, iov, iovcnt);
    if (n <= 0) {
      if (errno == EINTR)
        continue;
      return -1;
    }

    total_len -= n;
    ssize_t remaining = n;

    while (iovcnt > 0 && remaining >= (ssize_t)iov->iov_len) {
      remaining -= (ssize_t)iov->iov_len;
      iov++;
      iovcnt--;
    }

    if (iovcnt > 0) {
      iov->iov_base = (char *)iov->iov_base + remaining;
      iov->iov_len -= (size_t)remaining;
    }
  }
  return 0;
}

void munmap_or_warn(const void *addr, size_t length) {
  if (munmap((void *)addr, length) < 0) {
    jsockd_logf(LOG_DEBUG, "Error unmapping memory at %p of size %zu: %s\n",
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

static long ns_to_us(long ns) { return (ns / 1000) + (ns % 1000 >= 500); }

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
  if (len + 9 < buflen)
    snprintf(buf + len, buflen - len, ".%06ldZ", ns_to_us(ts->tv_nsec));
}

void write_to_wbuf(WBuf *buf, const char *inp, size_t size) {
  size_t to_write =
      buf->length >= buf->index ? MIN(buf->length - buf->index, size) : 0;
  memcpy(buf->buf + buf->index, inp, to_write);
  buf->index += to_write;
}

static void write_to_wbuf_wrapper(void *opaque, const char *inp, size_t size) {
  write_to_wbuf((WBuf *)opaque, inp, size);
}

void dump_error_to_wbuf(JSContext *ctx, JSValueConst exception_val,
                        WBuf *wbuf) {
  JS_PrintValue(ctx, write_to_wbuf_wrapper, (void *)wbuf, exception_val, NULL);
}

void log_error_with_prefix(const char *prefix, JSContext *ctx,
                           JSValueConst exception_val) {
  char errbuf[1024 * 8];
  WBuf wbuf = {.buf = errbuf, .length = sizeof(errbuf) / sizeof(char)};
  dump_error_to_wbuf(ctx, exception_val, &wbuf);
  jsockd_logf(LOG_ERROR, "%s%.*s\n", prefix, (int)wbuf.index, wbuf.buf);
}

PollFdResult poll_fd(int fd, int timeout_ms) {
  struct pollfd pfd = {.fd = fd, .events = POLLIN | POLLPRI};
  if (0 == poll(&pfd, 1, timeout_ms)) {
    if (atomic_load_explicit(&g_interrupted_or_error, memory_order_acquire))
      return SIG_INTERRUPT_OR_ERROR;
    return GO_AROUND;
  }
  // We get here on error too, in which case we may as well just try reading
  // from the fd and handle the error there.
  return READY;
}

PollFdResult ppoll_fd(int fd, const struct timespec *timeout) {
#ifdef MACOS
  int ms =
      MAX(1, (int)timeout->tv_sec * 1000 + (int)timeout->tv_nsec / 1000000);
  return poll_fd(fd, ms);
#else
  struct pollfd pfd = {.fd = fd, .events = POLLIN | POLLPRI};
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wimplicit-function-declaration"
  if (0 == ppoll(&pfd, 1, timeout, NULL)) {
#pragma GCC diagnostic pop
    if (atomic_load_explicit(&g_interrupted_or_error, memory_order_acquire))
      return SIG_INTERRUPT_OR_ERROR;
    return GO_AROUND;
  }
  // We get here on error too, in which case we may as well just try reading
  // from the fd and handle the error there.
  return READY;
#endif
}

void print_value_to_stdout(void *opaque, const char *buf, size_t size) {
  (void)opaque;
  fwrite(buf, 1, size, stdout);
}

char *read_all_stdin(size_t *out_size) {
  const size_t CHUNK = 8192;
  char *buf = NULL;
  size_t size = 0;
  size_t cap = 0;

  for (;;) {
    if (size + CHUNK > cap) {
      size_t new_cap = cap ? cap * 2 : CHUNK;
      if (new_cap < size + CHUNK)
        new_cap = size + CHUNK;
      char *new_buf = realloc(buf, new_cap);
      if (!new_buf) {
        free(buf);
        return NULL;
      }
      buf = new_buf;
      cap = new_cap;
    }

    size_t n = fread(buf + size, 1, CHUNK, stdin);
    size += n;

    if (n < CHUNK) {
      if (feof(stdin))
        break;
      if (ferror(stdin)) {
        free(buf);
        return NULL;
      }
    }
  }

  // Shrink to fit and NUL-terminate for text use
  char *final = realloc(buf, size + 1);
  if (!final)
    return NULL;
  final[size] = '\0';
  *out_size = size;
  return final;
}
