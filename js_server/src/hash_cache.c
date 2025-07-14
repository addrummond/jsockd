#include "hash_cache.h"
#include <memory.h>
#include <stdio.h>

static size_t get_cache_bucket(uint64_t uid, int n_bits) {
  return uid & (((uint64_t)1 << n_bits) - 1);
}

static size_t get_bucket_look_forward(int n_bits) { return n_bits * 3 / 2; }

// Pleasingly tiny hash function taken from https://github.com/tidwall/th64
uint64_t get_hash_cache_uid(const void *data, size_t len) {
  const uint64_t seed = 0x1234567890123456ULL;
  uint8_t *p = (uint8_t *)data, *e = p + len;
  uint64_t r = 0x14020a57acced8b7, x, h = seed;
  while (p + 8 <= e)
    memcpy(&x, p, 8), x *= r, p += 8, x = x << 31 | x >> 33, h = h * r ^ x,
                                      h = h << 31 | h >> 33;
  while (p < e)
    h = h * r ^ *(p++);
  r = (h = h * r + len, h ^= h >> 31, h *= r, h ^= h >> 31, h *= r,
       h ^= h >> 31, h *= r, h);
  return r == 0 ? 1 : r; // avoid zero hash
}

HashCacheBucket *add_to_hash_cache(HashCacheBucket buckets[], int n_bits,
                                   uint64_t uid) {
  size_t bucket_i = get_cache_bucket(uid, n_bits);

  size_t n_buckets = HASH_CACHE_BUCKET_ARRAY_SIZE_FROM_HASH_BITS(n_bits);
  const size_t bucket_look_forward = get_bucket_look_forward(n_bits);
  size_t i;
  for (i = bucket_i; i < bucket_i + bucket_look_forward; ++i) {
    size_t j = i % n_buckets; // wrap around if we reach the end
    if (buckets[j].uid == 0) {
      bucket_i = j;
      break;
    }
  };

  HashCacheBucket *bucket = buckets + bucket_i;
  bucket->uid = uid;
  return bucket;
}

void *get_hash_cache_entry(HashCacheBucket buckets[], int n_bits,
                           uint64_t uid) {
  size_t bucket = get_cache_bucket(uid, n_bits);

  size_t n_buckets = HASH_CACHE_BUCKET_ARRAY_SIZE_FROM_HASH_BITS(n_bits);
  const size_t bucket_look_forward = get_bucket_look_forward(n_bits);
  size_t i;
  for (i = bucket; i < bucket + bucket_look_forward; ++i) {
    size_t j = i % n_buckets; // wrap around if we reach the end
    if (buckets[j].uid == uid)
      return buckets[j].data;
  }

  return NULL;
}
