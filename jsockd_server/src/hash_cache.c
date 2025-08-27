#include "hash_cache.h"
#include <memory.h>
#include <stdio.h>

static size_t get_cache_bucket(HashCacheUid uid, int n_bits) {
  return uid.low64 & (((uint64_t)1 << n_bits) - 1);
}

static size_t get_bucket_look_forward(int n_bits) { return n_bits * 3 / 2; }

static const HashCacheUid zero_uid;

static bool hash_cache_uid_is_zero(const HashCacheUid *hcu) {
  return memcmp(hcu, &zero_uid, sizeof(*hcu)) == 0;
}

HashCacheUid get_hash_cache_uid(const void *data, size_t len) {
  return XXH3_128bits(data, len);
}

HashCacheBucket *add_to_hash_cache(HashCacheBucket buckets[], int n_bits,
                                   HashCacheUid uid) {
  size_t bucket_i = get_cache_bucket(uid, n_bits);

  size_t n_buckets = HASH_CACHE_BUCKET_ARRAY_SIZE_FROM_HASH_BITS(n_bits);
  const size_t bucket_look_forward = get_bucket_look_forward(n_bits);
  for (size_t i = bucket_i; i < bucket_i + bucket_look_forward; ++i) {
    size_t j = i % n_buckets; // wrap around if we reach the end
    if (hash_cache_uid_is_zero(&buckets[j].uid)) {
      bucket_i = j;
      break;
    }
  };

  HashCacheBucket *bucket = buckets + bucket_i;
  bucket->uid = uid;
  return bucket;
}

HashCacheBucket *get_hash_cache_entry(HashCacheBucket buckets[], int n_bits,
                                      HashCacheUid uid) {
  size_t bucket = get_cache_bucket(uid, n_bits);

  size_t n_buckets = HASH_CACHE_BUCKET_ARRAY_SIZE_FROM_HASH_BITS(n_bits);
  const size_t bucket_look_forward = get_bucket_look_forward(n_bits);
  for (size_t i = bucket; i < bucket + bucket_look_forward; ++i) {
    size_t j = i % n_buckets; // wrap around if we reach the end
    if (0 == memcmp(&buckets[j].uid, &uid, sizeof(uid)))
      return &buckets[j];
  }

  return NULL;
}
