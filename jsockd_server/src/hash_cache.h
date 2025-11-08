#ifndef HASH_CACHE_H_
#define HASH_CACHE_H_

#ifndef _REENTRANT
#define _REENTRANT
#endif

#include <stdbool.h>
#include <stdlib.h>
#include <xxHash/xxhash.h>

typedef XXH128_hash_t HashCacheUid;

// We don't actually store anything in the buckets. The idea is for the user
// to define another array the same size as the bucket array holding the data
// corresponding to each bucket. That way we don't need to do any void* casting.
typedef struct {
  HashCacheUid uid;
} HashCacheBucket;

#define HASH_CACHE_BUCKET_ARRAY_SIZE_FROM_HASH_BITS(hash_bits)                 \
  (1 << (hash_bits))

size_t get_cache_bucket(HashCacheUid uid, int n_bits);
HashCacheUid get_hash_cache_uid(const void *data, size_t size);
HashCacheBucket *get_hash_cache_bucket(HashCacheBucket buckets[], int n_bits,
                                   HashCacheUid uid);
HashCacheBucket *get_hash_cache_entry(HashCacheBucket buckets[], int n_bits,
                                      HashCacheUid uid);

#endif
