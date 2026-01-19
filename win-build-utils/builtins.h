#pragma once

#include <intrin.h>
/* Optional: make sure the compiler treats these as intrinsics */
#pragma intrinsic(_BitScanForward)
#pragma intrinsic(_BitScanReverse)
#if defined(_M_X64) || defined(_M_AMD64) || defined(_M_ARM64)
  #pragma intrinsic(_BitScanForward64)
  #pragma intrinsic(_BitScanReverse64)
#endif
/* Count leading zeros (32-bit). Returns 32 when x == 0. */
static inline int msvc_clz_u32(unsigned int x) {
  if (x == 0) return 32;
  unsigned long idx; /* 0..31 */
  _BitScanReverse(&idx, x);
  return 31 - (int)idx;
}
/* Count trailing zeros (32-bit). Returns 32 when x == 0. */
static inline int msvc_ctz_u32(unsigned int x) {
  if (x == 0) return 32;
  unsigned long idx; /* 0..31 */
  _BitScanForward(&idx, x);
  return (int)idx;
}
/* Count leading zeros (64-bit). Returns 64 when x == 0. */
static inline int msvc_clz_u64(unsigned long long x) {
  if (x == 0) return 64;
#if defined(_M_X64) || defined(_M_AMD64) || defined(_M_ARM64)
  unsigned long idx; /* 0..63 */
  _BitScanReverse64(&idx, x);
  return 63 - (int)idx;
#else
  unsigned long idx;
  unsigned int hi = (unsigned int)(x >> 32);
  if (hi != 0) {
    _BitScanReverse(&idx, hi);
    return 31 - (int)idx; /* highest bit in top 32 => clz = 31 - idx */
  } else {
    unsigned int lo = (unsigned int)(x & 0xFFFFFFFFu);
    _BitScanReverse(&idx, lo);
    return 63 - (int)idx; /* 32 + (31 - idx) == 63 - idx */
  }
#endif
}
/* Count trailing zeros (64-bit). Returns 64 when x == 0. */
static inline int msvc_ctz_u64(unsigned long long x) {
  if (x == 0) return 64;
#if defined(_M_X64) || defined(_M_AMD64) || defined(_M_ARM64)
  unsigned long idx; /* 0..63 */
  _BitScanForward64(&idx, x);
  return (int)idx;
#else
  unsigned long idx;
  unsigned int lo = (unsigned int)(x & 0xFFFFFFFFu);
  if (lo != 0) {
    _BitScanForward(&idx, lo);
    return (int)idx;
  } else {
    unsigned int hi = (unsigned int)(x >> 32);
    _BitScanForward(&idx, hi);
    return (int)idx + 32;
  }
#endif
}

/* Map GCC-style builtins to the MSVC-compatible implementations. */
#define __builtin_clz msvc_clz_u32
#define __builtin_ctz msvc_ctz_u32
#define __builtin_clzll msvc_clz_u64
#define __builtin_ctzll msvc_ctz_u64
