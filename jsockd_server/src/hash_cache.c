#include "hash_cache.h"
#include <memory.h>
#include <stdatomic.h>
#include <stdio.h>

size_t get_cache_bucket(HashCacheUid uid, int n_bits) {
  return uid % (1 << n_bits);
}

static size_t get_bucket_look_forward(int n_bits) { return n_bits * 3 / 2; }

HashCacheUid get_hash_cache_uid(const void *data, size_t len) {
  HashCacheUid v = XXH3_64bits(data, len);
  if (v == 0)
    return 1; // reserve 0 as "empty" value
  return v;
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
    if (atomic_compare_exchange_strong_explicit(&bucket->uid, &expected0uint64,
                                                j, memory_order_acq_rel,
                                                memory_order_acquire)) {
      atomic_fetch_add_explicit(&bucket->refcount, 1, memory_order_release);
      memcpy((void *)((char *)bucket + object_offset), object, object_size);
      return bucket;
    } else if (atomic_compare_exchange_strong_explicit(
                   &bucket->refcount, &expected0int, 1, memory_order_acq_rel,
                   memory_order_acquire)) {
      atomic_store_explicit(&bucket->uid, 0, memory_order_release);
      cleanup(bucket);
      memcpy((void *)((char *)bucket + object_offset), object, object_size);
      atomic_store_explicit(&bucket->uid, uid, memory_order_release);
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

    // We don't yet know if this is the bucket for us, but inc its
    // reference count here so that nothing deletes it from under is if it is.
    // If it's not, we can decrement it again.
    atomic_fetch_add_explicit(&bucket->refcount, 1, memory_order_release);

    if (atomic_load_explicit(&bucket->uid, memory_order_acquire) == uid)
      return bucket;

    atomic_fetch_add_explicit(&bucket->refcount, -1, memory_order_release);
  }
  return NULL;
}
