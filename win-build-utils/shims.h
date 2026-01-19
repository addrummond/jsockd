#pragma once

#include <time.h>
#ifndef WINSOCK2_INCLUDED_
#include <winsock2.h>   // defines struct timeval, FD_SET, select(), etc.
#endif
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#define __attribute__(...)
#define __attribute(...)
#define likely(x)      (x)
#define unlikely(x)     (x)
#define force_inline
#define no_inline
#define __maybe_unused
#define __exception

#ifndef CLOCK_REALTIME
#define CLOCK_REALTIME 0
#endif
#ifndef CLOCK_MONOTONIC
#define CLOCK_MONOTONIC 1
#endif

static inline void filetime_to_timespec(const FILETIME* ft, struct timespec* ts) {
  uint64_t t100 = ((uint64_t)ft->dwHighDateTime << 32) | ft->dwLowDateTime;
  /* Difference between Windows epoch (1601) and Unix epoch (1970) in 100ns units */
  const uint64_t EPOCH_DIFF_100NS = 116444736000000000ULL;
  if (t100 < EPOCH_DIFF_100NS) { ts->tv_sec = 0; ts->tv_nsec = 0; return; }
  t100 -= EPOCH_DIFF_100NS;
  ts->tv_sec  = (long)(t100 / 10000000ULL);           /* 10^7 100ns ticks per second */
  ts->tv_nsec = (long)((t100 % 10000000ULL) * 100ULL);/* remainder * 100 -> nanoseconds */
}

/* gettimeofday replacement: tz is ignored (as on many platforms) */
static inline int gettimeofday(struct timeval* tv, void* tz_unused) {
  if (!tv) return -1;
  FILETIME ft;
  /* Prefer precise clock on Win8+ if available */
  static BOOL (WINAPI *pGetPrecise)(LPFILETIME) = NULL;
  static int init = 0;
  if (!init) {
    HMODULE h = GetModuleHandleW(L"kernel32.dll");
    if (h) pGetPrecise = (BOOL (WINAPI *)(LPFILETIME))GetProcAddress(h, "GetSystemTimePreciseAsFileTime");
    init = 1;
  }
  if (pGetPrecise) pGetPrecise(&ft);
  else GetSystemTimeAsFileTime(&ft);

  struct timespec ts;
  filetime_to_timespec(&ft, &ts);
  tv->tv_sec  = ts.tv_sec;
  tv->tv_usec = (long)(ts.tv_nsec / 1000);
  return 0;
}

static inline int clock_gettime(int clk_id, struct timespec* ts) {
  if (!ts) return -1;

  if (clk_id == CLOCK_REALTIME) {
    FILETIME ft;
    static BOOL (WINAPI *pGetPrecise)(LPFILETIME) = NULL;
    static int init = 0;
    if (!init) {
      HMODULE h = GetModuleHandleW(L"kernel32.dll");
      if (h) pGetPrecise = (BOOL (WINAPI *)(LPFILETIME))GetProcAddress(h, "GetSystemTimePreciseAsFileTime");
      init = 1;
    }
    if (pGetPrecise) pGetPrecise(&ft);
    else GetSystemTimeAsFileTime(&ft);
    filetime_to_timespec(&ft, ts);
    return 0;
  }

  if (clk_id == CLOCK_MONOTONIC) {
    LARGE_INTEGER freq, counter;
    if (!QueryPerformanceFrequency(&freq) || !QueryPerformanceCounter(&counter)) {
      ts->tv_sec = 0; ts->tv_nsec = 0; return -1;
    }
    ts->tv_sec  = (long)(counter.QuadPart / freq.QuadPart);
    ts->tv_nsec = (long)((counter.QuadPart % freq.QuadPart) * 1000000000ULL / freq.QuadPart);
    return 0;
  }

  /* Unsupported clock id */
  ts->tv_sec = 0; ts->tv_nsec = 0;
  return -1;
}
