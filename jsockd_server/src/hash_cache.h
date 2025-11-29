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

// These platforms should provide lock-free 128-bit atomics. Robust compile-time
// detection of this specific functionality appears to be a bit of a rabbit
// hole.
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
#define HASH_CACHE_UID_FORMHASH_CACHE_UID_FORMAT_SPECIFIER "%016" PRIx64
#define HASH_CACHE_UID_FORMAT_ARGS(uid) (uint64_t)(uid)
#endif

// This structure should be embedded in a larger structure with the following
// field names for use with the macros defined:
//   struct MyBucket {
//     HashCacheBucket bucket;
//     WhateverType payload;
//   }
typedef struct HashCacheBucket {
  atomic_uint_fast64_t uid;
  atomic_int refcount;
} HashCacheBucket;

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
