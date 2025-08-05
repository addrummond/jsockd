#ifndef HASH_CACHE_H_
#define HASH_CACHE_H_

#ifndef _REENTRANT
#define _REENTRANT
#endif

#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>

// We don't actually store anything in the buckets. The idea is for the user
// to define another array the same size as the bucket array holding the data
// corresponding to each bucket. That way we don't need to do any void* casting.
typedef struct {
  uint64_t uid;
} HashCacheBucket;

#define HASH_CACHE_BUCKET_ARRAY_SIZE_FROM_HASH_BITS(hash_bits)                 \
  (1 << (hash_bits))

uint64_t get_hash_cache_uid(const void *data, size_t size);
HashCacheBucket *add_to_hash_cache(HashCacheBucket buckets[], int n_bits,
                                   uint64_t uid);
HashCacheBucket *get_hash_cache_entry(HashCacheBucket buckets[], int n_bits,
                                      uint64_t uid);

#endif
