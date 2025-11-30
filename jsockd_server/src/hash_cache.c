#include "hash_cache.h"
#include <memory.h>
#include <stdatomic.h>
#include <stdio.h>

size_t get_cache_bucket(HashCacheUid uid, int n_bits) {
  return uid % (HashCacheUid)(1 << n_bits);
}

static size_t get_bucket_look_forward(int n_bits) { return n_bits * 3 / 2; }

HashCacheUid get_hash_cache_uid(const void *data, size_t len) {
  HashCacheUid r;
#ifdef HASH_CACHE_USE_128_BIT_UIDS
  XXH128_hash_t v = XXH3_128bits(data, len);
  r = v.high64;
  r <<= 64;
  r |= v.low64;
#else
  r = XXH3_64bits(data, len);
#endif
  if (r <= 1)
    return 2; // reserve 0 and 1 as special values
  return r;
}

HashCacheBucket *add_to_hash_cache_(HashCacheBucket *buckets,
                                    size_t bucket_size, int n_bits,
                                    HashCacheUid uid, void *object,
                                    size_t object_offset, size_t object_size,
                                    void (*cleanup)(HashCacheBucket *)) {
  char *buckets_ = (char *)buckets;
  size_t bucket_i = get_cache_bucket(uid, n_bits);
  size_t n_buckets = HASH_CACHE_BUCKET_ARRAY_SIZE_FROM_HASH_BITS(n_bits);
  const size_t bucket_look_forward = get_bucket_look_forward(n_bits);
  for (size_t i = bucket_i; i < bucket_i + bucket_look_forward; ++i) {
    size_t j = i % n_buckets; // wrap around if we reach the end
    HashCacheBucket *bucket = (HashCacheBucket *)(buckets_ + j * bucket_size);
    uint_fast64_t expected0uint64 = 0;
    int expected0int = 0;

    // There's an empty bucket. In the initial atomic compare/exchange, we set
    // its uid to 1, which will neither match any real uid nor register as
    // 'empty', so that no other thread will attempt to read from or modify this
    // bucket while we mess around with it.
    if (atomic_compare_exchange_strong_explicit(&bucket->uid, &expected0uint64,
                                                1, memory_order_acq_rel,
                                                memory_order_acquire)) {
      atomic_fetch_add_explicit(&bucket->refcount, 1, memory_order_release);
      memcpy((void *)((char *)bucket + object_offset), object, object_size);
      atomic_store_explicit(&bucket->uid, uid, memory_order_release);
      return bucket;
    }

    // The bucket has a refcount of zero, so we can clean it up and then reuse
    // it.
    if (atomic_compare_exchange_strong_explicit(
            &bucket->refcount, &expected0int, 1, memory_order_acq_rel,
            memory_order_acquire)) {
      atomic_fetch_add_explicit(&bucket->update_count, 1, memory_order_release);
      if (cleanup)
        cleanup(bucket);
      memcpy((void *)((char *)bucket + object_offset), object, object_size);
      atomic_store_explicit(&bucket->uid, uid, memory_order_release);
      return bucket;
    }
  }
  return NULL;
}

HashCacheBucket *get_hash_cache_entry_(HashCacheBucket *buckets,
                                       size_t bucket_size, int n_bits,
                                       HashCacheUid uid) {
  char *buckets_ = (char *)buckets;
  size_t bucket_i = get_cache_bucket(uid, n_bits);
  size_t n_buckets = HASH_CACHE_BUCKET_ARRAY_SIZE_FROM_HASH_BITS(n_bits);
  const size_t bucket_look_forward = get_bucket_look_forward(n_bits);
  for (size_t i = bucket_i; i < bucket_i + bucket_look_forward; ++i) {
    size_t j = i % n_buckets; // wrap around if we reach the end
    HashCacheBucket *bucket = (HashCacheBucket *)(buckets_ + j * bucket_size);

    for (int i = 0; i < 1000; ++i) {
      int update_count_before =
          atomic_load_explicit(&bucket->update_count, memory_order_acquire);

      if (atomic_load_explicit(&bucket->uid, memory_order_acquire) != uid)
        break;

      atomic_fetch_add_explicit(&bucket->refcount, 1, memory_order_release);

      if (atomic_load_explicit(&bucket->update_count, memory_order_acquire) !=
          update_count_before)
        continue; // bucket was modified while we were raeding it; try again

      atomic_fetch_add_explicit(&bucket->refcount, -1, memory_order_release);

      return bucket;
    }
  }
  return NULL;
}
