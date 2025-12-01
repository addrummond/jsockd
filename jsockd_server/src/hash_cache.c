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
  if (r == 0) // reserve 0 as empty value
    return 1;
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
    int expected0int = 0;

    atomic_fetch_add_explicit(&bucket->update_count, 1, memory_order_release);

    // The bucket has a refcount of zero, so we can clean it up and then reuse
    // it.
    if (atomic_compare_exchange_strong_explicit(
            &bucket->refcount, &expected0int, 1, memory_order_acq_rel,
            memory_order_acquire)) {
      // This is the only code path that updates a bucket, so because of the
      // atomic compare/exchange, we know that no other thread is currently
      // doing so.
      HashCacheUid existing_uid =
          atomic_load_explicit(&bucket->uid, memory_order_acquire);
      if (existing_uid && cleanup)
        cleanup(bucket);
      memcpy((void *)((char *)bucket + object_offset), object, object_size);
      atomic_store_explicit(&bucket->uid, uid, memory_order_release);
      return bucket;
    }

    // We didn't actually update the bucket, so decrement the update count.
    atomic_fetch_add_explicit(&bucket->update_count, -1, memory_order_release);
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

    // Try up to 1000 times to get a consistent read of the bucket. This is a
    // reasonable approach because we expect low contention and short contention
    // times when contentions do occur. If we fail, we can just 'pretend' that
    // the item is not in the cache.
    for (int i = 0; i < 1000; ++i) {
      int update_count_before =
          atomic_load_explicit(&bucket->update_count, memory_order_acquire);

      if (uid != atomic_load_explicit(&bucket->uid, memory_order_relaxed))
        break;

      atomic_fetch_add_explicit(&bucket->refcount, 1, memory_order_release);

      bool updated =
          update_count_before !=
          atomic_load_explicit(&bucket->update_count, memory_order_acquire);

      atomic_fetch_add_explicit(&bucket->refcount, -1, memory_order_release);

      if (updated)
        continue;

      return bucket;
    }
  }
  return NULL;
}
