#ifndef HASH_CACHE_H_
#define HASH_CACHE_H_

#ifndef _REENTRANT
#define _REENTRANT
#endif

#include "typeofshim.h"
#include <stdatomic.h>
#include <stdbool.h>
#include <stdlib.h>
#include <xxHash/xxhash.h>

typedef XXH64_hash_t HashCacheUid;

// This structure should be embedded in a larger structure with the following
// field names for use with the macros defined:
//   struct MyBucket {
//     HashCacheBucket bucket;
//     WhateverType payload;
//   }
typedef struct {
  atomic_uint_fast64_t uid;
  atomic_int refcount;
} HashCacheBucket;

#define HASH_CACHE_BUCKET_ARRAY_SIZE_FROM_HASH_BITS(hash_bits)                 \
  (1 << (hash_bits))

size_t get_cache_bucket(HashCacheUid uid, int n_bits);
HashCacheUid get_hash_cache_uid(const void *data, size_t size);
HashCacheBucket *add_to_hash_cache_(HashCacheBucket *buckets,
                                    size_t bucket_size, int n_bits,
                                    HashCacheUid uid, void *object,
                                    size_t object_offset, size_t object_size);
HashCacheBucket *get_hash_cache_entry_(HashCacheBucket *buckets,
                                       size_t bucket_size, int n_bits,
                                       HashCacheUid uid);

#define add_to_hash_cache(buckets, n_bits, uid, data_ptr)                      \
  ((TYPEOF(buckets[0]) *)add_to_hash_cache_(                                   \
      &((buckets)[0].bucket), sizeof((buckets)[0]), (n_bits), (uid),           \
      ((void *)(data_ptr)), offsetof(TYPEOF(buckets[0]), payload),             \
      sizeof(buckets[0].payload)))

#define get_hash_cache_entry(buckets, n_bits, uid)                             \
  ((TYPEOF(buckets[0]) *)get_hash_cache_entry_(                                \
      &((buckets)[0].bucket), sizeof((buckets)[0]), (n_bits), (uid)))

#endif
