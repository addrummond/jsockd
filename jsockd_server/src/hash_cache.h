#ifndef HASH_CACHE_H_
#define HASH_CACHE_H_

#ifndef _REENTRANT
#define _REENTRANT
#endif

#include "typeofshim.h"
#include <inttypes.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdlib.h>
#include <xxHash/xxhash.h>

// These platforms should provide lock-free 128-bit atomics (assuming
// non-ancient processors in the x86 case). Robust compile-time detection of
// this specific functionality appears to be a bit of a rabbit hole.
#if defined(__GNUC__) &&                                                       \
    (defined(__x86_64__) || defined(__aarch64__) || defined(__arm64__))
#define HASH_CACHE_USE_128_BIT_UIDS
#endif

#ifdef HASH_CACHE_USE_128_BIT_UIDS
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wpedantic"
typedef __int128 HashCacheUid;
#pragma GCC diagnostic pop
#define HASH_CACHE_UID_FORMAT_SPECIFIER "%016" PRIx64 "%016" PRIx64
#define HASH_CACHE_UID_FORMAT_ARGS(uid) (uint64_t)((uid) >> 64), (uint64_t)(uid)
#else
typedef XXH64_hash_t HashCacheUid;
#define HASH_CACHE_UID_FORMAT_SPECIFIER "%016" PRIx64
#define HASH_CACHE_UID_FORMAT_ARGS(uid) (uint64_t)(uid)
#endif

// This structure should be embedded in a larger structure with the following
// field names for use with the macros defined:
//   struct MyBucket {
//     HashCacheBucket bucket;
//     WhateverType payload;
//   }
typedef struct {
#ifdef HASH_CACHE_USE_128_BIT_UIDS
  // 16-byte align may be required for use of native 128-bit atomic
  // instructions.
  _Alignas(16)
#endif
      _Atomic(HashCacheUid) uid;
  int32_t _Atomic refcount;
  int32_t _Atomic update_count;
} HashCacheBucket;

#ifdef HASH_CACHE_USE_128_BIT_UIDS
// We need to ensure that in arrays of these structures, each uid is at a
// 16-byte alined offset (so that native 128-bit atomic instructions can be
// used).
_Static_assert(sizeof(HashCacheBucket) % 16 == 0,
               "HashCacheBucket must be multiple of 16 bytes in size");
#endif

#define HASH_CACHE_BUCKET_ARRAY_SIZE_FROM_HASH_BITS(hash_bits)                 \
  (1 << (hash_bits))

// Used in tests
#define HASH_CACHE_UID_FROM_INT(i) ((HashCacheUid)(i))

size_t get_cache_bucket(HashCacheUid uid, int n_bits);
HashCacheUid get_hash_cache_uid(const void *data, size_t size);
HashCacheBucket *add_to_hash_cache_(HashCacheBucket *buckets,
                                    size_t bucket_size, int n_bits,
                                    HashCacheUid uid, void *object,
                                    size_t object_offset, size_t object_size,
                                    void (*cleanup)(HashCacheBucket *));
HashCacheBucket *get_hash_cache_entry_(HashCacheBucket *buckets,
                                       size_t bucket_size, int n_bits,
                                       HashCacheUid uid);

#define add_to_hash_cache(buckets, n_bits, uid, data_ptr, cleanup)             \
  ((TYPEOF(buckets[0]) *)add_to_hash_cache_(                                   \
      &((buckets)[0].bucket), sizeof((buckets)[0]), (n_bits), (uid),           \
      ((void *)(data_ptr)), offsetof(TYPEOF(buckets[0]), payload),             \
      sizeof(buckets[0].payload), (cleanup)))

#define get_hash_cache_entry(buckets, n_bits, uid)                             \
  ((TYPEOF(buckets[0]) *)get_hash_cache_entry_(                                \
      &((buckets)[0].bucket), sizeof((buckets)[0]), (n_bits), (uid)))

#endif
