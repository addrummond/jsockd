// Having all the tests in one module simplifies things a bit when it comes
// to building and running tests. As yet we don't really have enough modules
// to test that this is too big of a problem.

#include "../../src/hash_cache.h"
#include "../../src/hex.h"
#include "../../src/line_buf.h"
#include "../../src/wait_group.h"
#include "lib/acutest.h"
#include "lib/pcg.h"
#include <assert.h>
#include <pthread.h>
#include <stdbool.h>

/******************************************************************************
    Tests for wait_group
******************************************************************************/

static void *inc_wait_group_pthread_func(void *data) {
  WaitGroup *wg = (WaitGroup *)data;
  for (int i = 0; i < 10; ++i)
    assert(0 == wait_group_inc(wg, 1));
  return NULL;
}

static void TEST_wait_group_inc_and_wait_basic_use_case(void) {
  WaitGroup wg;
  wait_group_init(&wg, 10);
  pthread_t inc_thread;
  pthread_create(&inc_thread, NULL, inc_wait_group_pthread_func, &wg);
  int wait_return = wait_group_timed_wait(&wg, 1000000000);
  TEST_ASSERT(0 == wait_return);
  pthread_join(inc_thread, NULL);
}

/******************************************************************************
    Tests for hash_cache
******************************************************************************/

static void TEST_hash_cache_add_and_retrieve(void) {
  HashCacheBucket buckets[8] = {0};
  uint64_t uid = 123456789;

  HashCacheBucket *bucket = add_to_hash_cache(buckets, 3, uid);

  void *retrieved_bucket = get_hash_cache_entry(buckets, 3, uid);
  TEST_ASSERT(retrieved_bucket == bucket);
}

static void TEST_hash_cache_handles_duplicate_hash_values(void) {
  HashCacheBucket buckets[8] = {0};
  uint64_t uid1 = 0x0000000000000001;
  uint64_t uid2 =
      0xFF00000000000001; // lower bits identical, so shares same bucket

  HashCacheBucket *bucket1 = add_to_hash_cache(buckets, 3, uid1);

  HashCacheBucket *bucket2 = add_to_hash_cache(buckets, 3, uid2);

  HashCacheBucket *retrieved_bucket1 = get_hash_cache_entry(buckets, 3, uid1);
  TEST_ASSERT(retrieved_bucket1 == bucket1);

  HashCacheBucket *retrieved_bucket2 = get_hash_cache_entry(buckets, 3, uid2);
  TEST_ASSERT(retrieved_bucket2 == bucket2);
}

static void TEST_hash_cash_values_with_same_bucket_id_eventually_booted(void) {
  HashCacheBucket buckets[8] = {0};

  for (uint64_t i = 0; i < 8; ++i) {
    add_to_hash_cache(buckets, 3, (i << 48) | 1);
  }

  int retrieved_count = 0;
  for (uint64_t i = 0; i < 8; ++i) {
    void *retrieved_data = get_hash_cache_entry(buckets, 3, (i << 48) | 1);
    if (retrieved_data != NULL)
      ++retrieved_count;
  }

  // It won't have been possible to find an unoccupied bucket for all values, so
  // some of the buckets will still have NULL data.
  TEST_ASSERT(retrieved_count > 0 && retrieved_count < 8);
}

static void TEST_hash_cash_empty_bucket_array(void) {
  // empty arrays not standardized, so use size 1
  HashCacheBucket buckets[1] = {0};
  TEST_ASSERT(buckets == add_to_hash_cache(buckets, 0, 123));
  TEST_ASSERT(NULL == get_hash_cache_entry(buckets, 0, 123));
}

static void TEST_hash_cash_size_2_bucket_array(void) {
  HashCacheBucket buckets[2] = {0};
  HashCacheBucket *b = add_to_hash_cache(buckets, 1, 123);
  TEST_ASSERT(b == get_hash_cache_entry(buckets, 1, 123));
}

static void TEST_hash_cash_fuzz(void) {
  HashCacheBucket buckets[64] = {0};
  pcg32_random_t rng = {.inc = 0x12345678, .state = 0x87654321};
  uint64_t next_uid = 0;

  // Adding at most 64 entries with sequential UIDs to a 64 bucket hash cache,
  // so we know that no old entries will be booted.
  for (int i = 0; i < 64; ++i) {
    uint32_t r = pcg32_random_r(&rng);
    switch (r % 3) {
    case 0: {
      // Add a new entry
      add_to_hash_cache(buckets, 6, next_uid++);
    } break;
    case 1:
      if (next_uid != 0) {
        // Retrieve existing entry
        void *v = get_hash_cache_entry(buckets, 6, r % next_uid);
        TEST_CHECK(v != NULL);
        break;
      } // fallthrough
    case 2: {
      // Try to retrieve a (probably) non-existing entry
      void *v = get_hash_cache_entry(buckets, 6, r);
      TEST_CHECK(v == NULL);
    } break;
    }
  }

  // Keep adding entries with random UIDs to test code paths with conflicts.
  for (int i = 0; i < 10000; ++i) {
    uint32_t r1 = pcg32_random_r(&rng);
    uint32_t r2 = pcg32_random_r(&rng);
    add_to_hash_cache(buckets, 6, (uint64_t)r1 | ((uint64_t)r2 << 32));
  }
}

/******************************************************************************
    Tests for line_buf
******************************************************************************/

int line_handler_data;
int line_handler_input_i = 0;

static int line_handler_1(const char *line, size_t line_len, void *data) {
  TEST_ASSERT((int *)data == &line_handler_data);
  TEST_ASSERT(line_len == 5);
  TEST_ASSERT(line[0] == 'l' && line[1] == 'i' && line[2] == 'n' &&
              line[3] == 'e' && line[4] == '0' + *((int *)data) &&
              line[5] == '\0');
  *((int *)data) = -1; // Mark as handled
  return 0;
}

static int read_6_from_string(char *buf, size_t n, void *data) {
  int nn = n < 6 ? n : 6;
  strncpy(buf, (char *)data, nn);
  return nn;
}

static void TEST_line_buf_simple_case(void) {
  LineBuf b = {.buf = malloc(sizeof(char) * 64), .size = 64};
  const char *input = "line1\nline2\nline3\n";

  int r;
  line_handler_data = 1;
  r = line_buf_read(&b, '\n', read_6_from_string, (void *)input, line_handler_1,
                    &line_handler_data, "");
  TEST_ASSERT(line_handler_data == -1);
  TEST_ASSERT(r == 6);

  line_handler_data = 2;
  r = line_buf_read(&b, '\n', read_6_from_string, (void *)(input + 6),
                    line_handler_1, &line_handler_data, "");
  TEST_ASSERT(line_handler_data == -1);
  TEST_ASSERT(r == 6);

  line_handler_data = 3;
  r = line_buf_read(&b, '\n', read_6_from_string, (void *)(input + 12),
                    line_handler_1, &line_handler_data, "");
  TEST_ASSERT(line_handler_data == -1);
  TEST_ASSERT(r == 6);

  free(b.buf);
}

static int read_4_from_string(char *buf, size_t n, void *data) {
  int nn = n < 4 ? n : 4;
  strncpy(buf, (char *)data, nn);
  return nn;
}

static void TEST_line_buf_awkward_chunking(void) {
  LineBuf b = {.buf = malloc(sizeof(char) * 64), .size = 64};
  const char *input = "line1\nline2\nline3\n";

  int r;
  line_handler_data = 1;
  r = line_buf_read(&b, '\n', read_4_from_string, (void *)input, line_handler_1,
                    &line_handler_data, "");
  TEST_ASSERT(line_handler_data ==
              1); // not handled yet as we didn't get to newline
  TEST_ASSERT(r == 4);

  r = line_buf_read(&b, '\n', read_4_from_string, (void *)(input + 4),
                    line_handler_1, &line_handler_data, "");
  TEST_ASSERT(line_handler_data == -1);
  TEST_ASSERT(r == 4);

  line_handler_data = 2;
  r = line_buf_read(&b, '\n', read_4_from_string, (void *)(input + 8),
                    line_handler_1, &line_handler_data, "");
  TEST_ASSERT(line_handler_data == -1);
  TEST_ASSERT(r == 4);

  line_handler_data = 3;
  r = line_buf_read(&b, '\n', read_4_from_string, (void *)(input + 12),
                    line_handler_1, &line_handler_data, "");
  TEST_ASSERT(line_handler_data ==
              3); // not handled yet as we didn't get to newline
  TEST_ASSERT(r == 4);

  r = line_buf_read(&b, '\n', read_4_from_string, (void *)(input + 16),
                    line_handler_1, &line_handler_data, "");
  TEST_ASSERT(line_handler_data == -1);
  TEST_ASSERT(r == 4);

  free(b.buf);
}

static int line_handler_never_called(const char *line, size_t line_len,
                                     void *data) {
  // This handler should never be called.
  TEST_ASSERT(0);
  return 0;
}

static void TEST_line_buf_truncation(void) {
  LineBuf b = {.buf = malloc(sizeof(char) * 8), .size = 8};
  const char *input = "12345678901234567";

  line_handler_data = 0;

  int r;
  r = line_buf_read(&b, '\n', read_6_from_string, (void *)input,
                    line_handler_never_called, &line_handler_data, "trunc");
  TEST_ASSERT(r >= 0);

  r = line_buf_read(&b, '\n', read_6_from_string, (void *)(input + 6),
                    line_handler_never_called, &line_handler_data, "trunc");
  TEST_ASSERT(r >= 0);

  // Read more than the buffer size, should truncate
  r = line_buf_read(&b, '\n', read_6_from_string, (void *)(input + 6),
                    line_handler_never_called, &line_handler_data, "trunc");
  TEST_ASSERT(r >= 0);

  TEST_ASSERT(b.buf[0] == 't' && b.buf[1] == 'r' && b.buf[2] == 'u' &&
              b.buf[3] == 'n' && b.buf[4] == 'c');

  free(b.buf);
}

static int line_handler_inc_count(const char *line, size_t line_len,
                                  void *data) {
  ++*((int *)data);
  return 0;
}

static int read_all_from_string(char *buf, size_t n, void *data) {
  strcpy(buf, (const char *)data);
  return strlen((const char *)data);
}

static void TEST_line_buf_one_shot(void) {
  LineBuf b = {.buf = malloc(sizeof(char) * 64), .size = 64};
  const char *input = "line1\nline2\nline3\n";

  int r;
  int count = 0;
  r = line_buf_read(&b, '\n', read_all_from_string, (void *)input,
                    line_handler_inc_count, &count, "trunc");
  TEST_ASSERT(r >= 0);
  TEST_ASSERT(count == 3);

  free(b.buf);
}

/******************************************************************************
    Tests for hex
******************************************************************************/

static void TEST_hex_decode_empty_string_zero_length(void) {
  uint8_t buf[1];
  TEST_ASSERT(0 == hex_decode(buf, 0, ""));
}

static void TEST_hex_decode_nonempty_string_zero_length(void) {
  uint8_t buf[1];
  TEST_ASSERT(0 == hex_decode(buf, 0, "ab12ffff"));
}

static void TEST_hex_decode_gives_expected_result(void) {
  uint8_t buf[4];
  TEST_ASSERT(4 == hex_decode(buf, 4, "01021Ff5"));
  TEST_ASSERT(buf[0] == 0x01 && buf[1] == 0x02 && buf[2] == 0x1F &&
              buf[3] == 0xF5);
}

/******************************************************************************
    Add all tests to the list below.
******************************************************************************/

#define T(name) {#name, TEST_##name}

TEST_LIST = {T(wait_group_inc_and_wait_basic_use_case),
             T(hash_cache_add_and_retrieve),
             T(hash_cache_handles_duplicate_hash_values),
             T(hash_cash_values_with_same_bucket_id_eventually_booted),
             T(hash_cash_empty_bucket_array),
             T(hash_cash_size_2_bucket_array),
             T(hash_cash_fuzz),
             T(line_buf_simple_case),
             T(line_buf_awkward_chunking),
             T(line_buf_truncation),
             T(line_buf_one_shot),
             T(hex_decode_empty_string_zero_length),
             T(hex_decode_nonempty_string_zero_length),
             T(hex_decode_gives_expected_result),
             {NULL, NULL}};
