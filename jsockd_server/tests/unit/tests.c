// Having all the tests in one module simplifies things a bit when it comes
// to building and running tests. As yet we don't really have enough modules
// to test that this is too big of a problem.

#include "../../src/cmdargs.h"
#include "../../src/hash_cache.h"
#include "../../src/hex.h"
#include "../../src/line_buf.h"
#include "../../src/modcompiler.h"
#include "../../src/utils.h"
#include "../../src/verify_bytecode.h"
#include "../../src/wait_group.h"
#include "lib/acutest.h"
#include "lib/pcg.h"
#include <assert.h>
#include <ed25519/ed25519.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>

#define snprintf_nowarn(...) (snprintf(__VA_ARGS__) < 0 ? abort() : (void)0)

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
  HashCacheUid uid = {.low64 = 123456789};

  HashCacheBucket *bucket = add_to_hash_cache(buckets, 3, uid);

  void *retrieved_bucket = get_hash_cache_entry(buckets, 3, uid);
  TEST_ASSERT(retrieved_bucket == bucket);
}

static void TEST_hash_cache_handles_duplicate_hash_values(void) {
  HashCacheBucket buckets[8] = {0};
  HashCacheUid uid1 = {.low64 = 0x0000000000000001};
  HashCacheUid uid2 = {
      .low64 =
          0xFF00000000000001}; // lower bits identical, so shares same bucket

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
    add_to_hash_cache(buckets, 3, (HashCacheUid){.low64 = (i << 48) | 1});
  }

  int retrieved_count = 0;
  for (uint64_t i = 0; i < 8; ++i) {
    void *retrieved_data = get_hash_cache_entry(
        buckets, 3, (HashCacheUid){.low64 = (i << 48) | 1});
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
  TEST_ASSERT(buckets ==
              add_to_hash_cache(buckets, 0, (HashCacheUid){.low64 = 123}));
  TEST_ASSERT(NULL ==
              get_hash_cache_entry(buckets, 0, (HashCacheUid){.low64 = 123}));
}

static void TEST_hash_cash_size_2_bucket_array(void) {
  HashCacheBucket buckets[2] = {0};
  HashCacheBucket *b =
      add_to_hash_cache(buckets, 1, (HashCacheUid){.low64 = 123});
  TEST_ASSERT(b ==
              get_hash_cache_entry(buckets, 1, (HashCacheUid){.low64 = 123}));
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
      add_to_hash_cache(buckets, 6, (HashCacheUid){.low64 = next_uid++});
    } break;
    case 1:
      if (next_uid != 0) {
        // Retrieve existing entry
        void *v = get_hash_cache_entry(buckets, 6,
                                       (HashCacheUid){.low64 = r % next_uid});
        TEST_CHECK(v != NULL);
        break;
      } // fallthrough
    case 2: {
      // Try to retrieve a (probably) non-existing entry
      void *v = get_hash_cache_entry(buckets, 6, (HashCacheUid){.low64 = r});
      TEST_CHECK(v == NULL);
    } break;
    }
  }

  // Keep adding entries with random UIDs to test code paths with conflicts.
  for (int i = 0; i < 10000; ++i) {
    uint32_t r1 = pcg32_random_r(&rng);
    uint32_t r2 = pcg32_random_r(&rng);
    add_to_hash_cache(
        buckets, 6,
        (HashCacheUid){.low64 = (uint64_t)r1 | ((uint64_t)r2 << 32)});
  }
}

/******************************************************************************
    Tests for line_buf
******************************************************************************/

int line_handler_data;
int line_handler_input_i = 0;

static int line_handler_1(const char *line, size_t line_len, void *data,
                          bool truncated) {
  TEST_ASSERT((int *)data == &line_handler_data);
  TEST_ASSERT(line_len == 5);
  TEST_ASSERT(line[0] == 'l' && line[1] == 'i' && line[2] == 'n' &&
              line[3] == 'e' && line[4] == '0' + *((int *)data) &&
              line[5] == '\0');
  TEST_ASSERT(!truncated);
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
                    &line_handler_data);
  TEST_ASSERT(line_handler_data == -1);
  TEST_ASSERT(r == 6);

  line_handler_data = 2;
  r = line_buf_read(&b, '\n', read_6_from_string, (void *)(input + 6),
                    line_handler_1, &line_handler_data);
  TEST_ASSERT(line_handler_data == -1);
  TEST_ASSERT(r == 6);

  line_handler_data = 3;
  r = line_buf_read(&b, '\n', read_6_from_string, (void *)(input + 12),
                    line_handler_1, &line_handler_data);
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
                    &line_handler_data);
  TEST_ASSERT(line_handler_data ==
              1); // not handled yet as we didn't get to newline
  TEST_ASSERT(r == 4);

  r = line_buf_read(&b, '\n', read_4_from_string, (void *)(input + 4),
                    line_handler_1, &line_handler_data);
  TEST_ASSERT(line_handler_data == -1);
  TEST_ASSERT(r == 4);

  line_handler_data = 2;
  r = line_buf_read(&b, '\n', read_4_from_string, (void *)(input + 8),
                    line_handler_1, &line_handler_data);
  TEST_ASSERT(line_handler_data == -1);
  TEST_ASSERT(r == 4);

  line_handler_data = 3;
  r = line_buf_read(&b, '\n', read_4_from_string, (void *)(input + 12),
                    line_handler_1, &line_handler_data);
  TEST_ASSERT(line_handler_data ==
              3); // not handled yet as we didn't get to newline
  TEST_ASSERT(r == 4);

  r = line_buf_read(&b, '\n', read_4_from_string, (void *)(input + 16),
                    line_handler_1, &line_handler_data);
  TEST_ASSERT(line_handler_data == -1);
  TEST_ASSERT(r == 4);

  free(b.buf);
}

static int line_handler_never_called(const char *line, size_t line_len,
                                     void *data, bool truncated) {
  // This handler should never be called.
  TEST_ASSERT(0);
  return 0;
}

static void TEST_line_buf_truncation(void) {
  LineBuf b = {.buf = malloc(sizeof(char) * 8), .size = 8};
  const char *input = "123456";

  line_handler_data = 0;

  int r;
  r = line_buf_read(&b, '\n', read_6_from_string, (void *)input,
                    line_handler_never_called, &line_handler_data);
  TEST_ASSERT(r == LINE_BUF_READ_EOF || r > 0);

  r = line_buf_read(&b, '\n', read_6_from_string, (void *)(input),
                    line_handler_never_called, &line_handler_data);
  TEST_ASSERT(r == LINE_BUF_READ_EOF || r > 0);

  // Read more than the buffer size, should truncate
  r = line_buf_read(&b, '\n', read_6_from_string, (void *)(input),
                    line_handler_never_called, &line_handler_data);
  TEST_ASSERT(r == LINE_BUF_READ_EOF || r > 0);
  TEST_ASSERT(b.truncated == true);

  free(b.buf);
}

static int line_handler_trunc_then_not_trunc(const char *line, size_t line_len,
                                             void *data, bool truncated) {
  static bool first_call = true;
  TEST_ASSERT((first_call && truncated) ||
              (!first_call && !truncated && line[0] == '1' && line[1] == '2' &&
               line[2] == '3' && line[3] == '4' && line[4] == '5' &&
               line[5] == '\0'));
  first_call = false;
  return 0;
}

static void TEST_line_buf_truncation_then_normal_read(void) {
  LineBuf b = {.buf = malloc(sizeof(char) * 8), .size = 8};
  const char *input = "12345612345\n";

  line_handler_data = 0;

  int r;
  r = line_buf_read(&b, '\n', read_6_from_string, (void *)input,
                    line_handler_never_called, &line_handler_data);
  TEST_ASSERT(r == LINE_BUF_READ_EOF || r > 0);

  r = line_buf_read(&b, '\n', read_6_from_string, (void *)(input + 6),
                    line_handler_never_called, &line_handler_data);
  TEST_ASSERT(r == LINE_BUF_READ_EOF || r > 0);

  // Read more than the buffer size, should truncate
  r = line_buf_read(&b, '\n', read_6_from_string, (void *)(input + 6),
                    line_handler_trunc_then_not_trunc, &line_handler_data);
  TEST_ASSERT(r == LINE_BUF_READ_EOF || r > 0);
  r = line_buf_read(&b, '\n', read_6_from_string, (void *)(input + 6),
                    line_handler_trunc_then_not_trunc, &line_handler_data);
  TEST_ASSERT(r == LINE_BUF_READ_EOF || r > 0);

  free(b.buf);
}

static int line_handler_inc_count(const char *line, size_t line_len, void *data,
                                  bool truncated) {
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
                    line_handler_inc_count, &count);
  TEST_ASSERT(r == LINE_BUF_READ_EOF || r > 0);
  TEST_ASSERT(count == 3);

  free(b.buf);
}

static void TEST_line_buf_replay_empty_case(void) {
  LineBuf b = {.buf = malloc(sizeof(char) * 64), .size = 64};
  const char *input = "line1\nline2\nline3\n";

  int r;
  int count = 0;
  r = line_buf_read(&b, '\n', read_all_from_string, (void *)input,
                    line_handler_inc_count, &count);
  TEST_ASSERT(r == LINE_BUF_READ_EOF || r > 0);
  TEST_ASSERT(count == 3);

  r = line_buf_replay(&b, '\n', line_handler_never_called, NULL);

  free(b.buf);
}

static int line_handler_fail_on_2(const char *line, size_t line_len, void *data,
                                  bool truncated) {
  if (line[4] == '2')
    return -1;
  ++*((int *)data);
  return 0;
}

static int line_handler_inc_by_line_num(const char *line, size_t line_len,
                                        void *data, bool truncated) {
  *((int *)data) += (line[4] - '0');
  return 0;
}

static void TEST_line_buf_replay_error_case(void) {
  LineBuf b = {.buf = malloc(sizeof(char) * 64), .size = 64};
  const char *input = "line1\nline2\nline3\n";

  int r;
  int count = 0;
  r = line_buf_read(&b, '\n', read_all_from_string, (void *)input,
                    line_handler_fail_on_2, &count);
  TEST_ASSERT(r < 0);
  TEST_ASSERT(count == 1);

  count = 0;
  r = line_buf_replay(&b, '\n', line_handler_inc_by_line_num, &count);
  TEST_ASSERT(count == 2 + 3);

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
    Tests for cmdargs
******************************************************************************/

static char cmdargs_errlog_buf[1024];
static char *cmdargs_errlog_ptr = cmdargs_errlog_buf;

static void cmdargs_errlog(const char *fmt, ...) {
  va_list args;
  va_start(args, fmt);
  int n = vsnprintf(cmdargs_errlog_ptr,
                    sizeof(cmdargs_errlog_buf) / sizeof(cmdargs_errlog_buf[0]) -
                        (cmdargs_errlog_ptr - cmdargs_errlog_buf),
                    fmt, args);
  va_end(args);
  cmdargs_errlog_ptr += n;
}

static void TEST_cmdargs_returns_error_for_no_args(void) {
  CmdArgs cmdargs = {0};
  int r = parse_cmd_args(0, NULL, cmdargs_errlog, &cmdargs);
  TEST_ASSERT(r == -1);
}

static void TEST_cmdargs_returns_error_for_one_arg(void) {
  CmdArgs cmdargs = {0};
  char *argv[] = {"jsockd"};
  int r = parse_cmd_args(sizeof(argv) / sizeof(argv[0]), argv, cmdargs_errlog,
                         &cmdargs);
  TEST_ASSERT(r == -1);
}

static void TEST_cmdargs_returns_success_for_just_one_socket(void) {
  CmdArgs cmdargs = {0};
  char *argv[] = {"jsockd", "-s", "my_socket"};
  int r = parse_cmd_args(sizeof(argv) / sizeof(argv[0]), argv, cmdargs_errlog,
                         &cmdargs);
  TEST_ASSERT(r == 0);
  TEST_ASSERT(cmdargs.n_sockets == 1);
  TEST_ASSERT(0 == strcmp(cmdargs.socket_path[0], "my_socket"));
}

static void TEST_cmdargs_returns_success_for_multiple_sockets(void) {
  CmdArgs cmdargs = {0};
  char *argv[] = {"jsockd", "-s", "my_socket1", "-b",
                  "1F",     "-s", "my_socket2", "my_socket3",
                  "-s",     "--", "my_socket4", "-my_socket5"};
  int r = parse_cmd_args(sizeof(argv) / sizeof(argv[0]), argv, cmdargs_errlog,
                         &cmdargs);
  TEST_ASSERT(r == 0);
  TEST_ASSERT(cmdargs.n_sockets == 5);
  TEST_ASSERT(0 == strcmp(cmdargs.socket_path[0], "my_socket1"));
  TEST_ASSERT(0 == strcmp(cmdargs.socket_path[1], "my_socket2"));
  TEST_ASSERT(0 == strcmp(cmdargs.socket_path[2], "my_socket3"));
  TEST_ASSERT(0 == strcmp(cmdargs.socket_path[3], "my_socket4"));
  TEST_ASSERT(0 == strcmp(cmdargs.socket_path[4], "-my_socket5"));
}

static void TEST_cmdargs_returns_error_for_multiple_dash_b(void) {
  CmdArgs cmdargs = {0};
  char *argv[] = {"jsockd", "-s", "my_socket1", "-b", "FF", "-b", "00"};
  int r = parse_cmd_args(sizeof(argv) / sizeof(argv[0]), argv, cmdargs_errlog,
                         &cmdargs);
  TEST_ASSERT(r != 0);
  TEST_ASSERT(strstr(cmdargs_errlog_buf, "-b"));
  TEST_ASSERT(strstr(cmdargs_errlog_buf, "at most once"));
}

static void TEST_cmdargs_returns_error_for_multiple_dash_m(void) {
  CmdArgs cmdargs = {0};
  char *argv[] = {"jsockd",     "-s", "my_socket1", "-m",
                  "mod1.qjsbc", "-m", "mod2.qjsbc"};
  int r = parse_cmd_args(sizeof(argv) / sizeof(argv[0]), argv, cmdargs_errlog,
                         &cmdargs);
  TEST_ASSERT(r != 0);
  TEST_ASSERT(strstr(cmdargs_errlog_buf, "-m"));
  TEST_ASSERT(strstr(cmdargs_errlog_buf, "at most once"));
}

static void TEST_cmdargs_returns_success_for_full_set_of_options(void) {
  CmdArgs cmdargs = {0};
  char *argv[] = {"jsockd",         "-s", "my_socket1", "-s",
                  "my_socket2",     "-s", "my_socket3", "-m",
                  "my_module.qjsc", "-b", "1F"};
  int r = parse_cmd_args(sizeof(argv) / sizeof(argv[0]), argv, cmdargs_errlog,
                         &cmdargs);
  TEST_ASSERT(r == 0);
  TEST_ASSERT(cmdargs.n_sockets == 3);
  TEST_ASSERT(0 == strcmp(cmdargs.socket_path[0], "my_socket1"));
  TEST_ASSERT(0 == strcmp(cmdargs.socket_path[1], "my_socket2"));
  TEST_ASSERT(0 == strcmp(cmdargs.socket_path[2], "my_socket3"));
  TEST_ASSERT(cmdargs.socket_sep_char == 0x1F);
  TEST_ASSERT(0 == strcmp(cmdargs.es6_module_bytecode_file, "my_module.qjsc"));
}

static void TEST_cmdargs_returns_error_if_no_sockets_specified(void) {
  CmdArgs cmdargs = {0};
  char *argv[] = {"jsockd", "-m", "my_module.qjsc", "-b", "1F"};
  int r = parse_cmd_args(sizeof(argv) / sizeof(argv[0]), argv, cmdargs_errlog,
                         &cmdargs);
  TEST_ASSERT(r != 0);
  TEST_ASSERT(strstr(cmdargs_errlog_buf, "No sockets specified"));
}

static void TEST_cmdargs_returns_error_if_s_has_no_arg(void) {
  CmdArgs cmdargs = {0};
  char *argv[] = {"jsockd", "-m", "my_module.qjsc", "-s", "-b", "1F"};
  int r = parse_cmd_args(sizeof(argv) / sizeof(argv[0]), argv, cmdargs_errlog,
                         &cmdargs);
  TEST_ASSERT(r != 0);
  TEST_ASSERT(strstr(cmdargs_errlog_buf, "-s requires at least"));
}

static void TEST_cmdargs_handles_more_sockets_than_MAX_THREADS(void) {
  CmdArgs cmdargs = {0};
  char *argv[MAX_THREADS + 2 + 10];
  argv[0] = "jsockd";
  argv[1] = "-s";
  for (int i = 2; i < (int)(sizeof(argv) / sizeof(argv[0])); ++i)
    argv[i] = "mysocket";
  int r = parse_cmd_args(sizeof(argv) / sizeof(argv[0]), argv, cmdargs_errlog,
                         &cmdargs);
  TEST_ASSERT(r == 0);
}

static void TEST_cmdargs_dash_v(void) {
  CmdArgs cmdargs = {0};
  char *argv[] = {"jsockd", "-v"};
  int r = parse_cmd_args(sizeof(argv) / sizeof(argv[0]), argv, cmdargs_errlog,
                         &cmdargs);
  TEST_ASSERT(r == 0);
}

static void TEST_cmdargs_dash_sm(void) {
  CmdArgs cmdargs = {0};
  char *argv[] = {"jsockd",    "-s",  "/tmp/sock",           "-m",
                  "foo.qjsbc", "-sm", "my_source_map.js.map"};
  int r = parse_cmd_args(sizeof(argv) / sizeof(argv[0]), argv, cmdargs_errlog,
                         &cmdargs);
  TEST_ASSERT(r == 0);
  TEST_ASSERT(0 == strcmp(cmdargs.source_map_file, "my_source_map.js.map"));
}

static void TEST_cmdargs_dash_sm_returns_error_if_dash_m_not_present(void) {
  CmdArgs cmdargs = {0};
  char *argv[] = {"jsockd", "-s", "/tmp/sock", "-sm", "my_source_map.js.map"};
  int r = parse_cmd_args(sizeof(argv) / sizeof(argv[0]), argv, cmdargs_errlog,
                         &cmdargs);
  TEST_ASSERT(r != 0);
  TEST_ASSERT(strstr(cmdargs_errlog_buf, "can only be used with -m"));
}

static void
TEST_cmdargs_returns_error_if_dash_v_combined_with_other_opts(void) {
  CmdArgs cmdargs = {0};
  char *argv[] = {"jsockd", "-v", "-s", "my_socket"};
  int r = parse_cmd_args(sizeof(argv) / sizeof(argv[0]), argv, cmdargs_errlog,
                         &cmdargs);
  TEST_ASSERT(r != 0);
}

static void TEST_cmdargs_returns_error_if_dash_v_has_arg(void) {
  CmdArgs cmdargs = {0};
  char *argv[] = {"jsockd", "-v", "spurious_arg"};
  int r = parse_cmd_args(sizeof(argv) / sizeof(argv[0]), argv, cmdargs_errlog,
                         &cmdargs);
  TEST_ASSERT(r != 0);
}

static void TEST_cmdargs_dash_t(void) {
  CmdArgs cmdargs = {0};
  char *argv[] = {"jsockd", "-s", "/tmp/sock", "-m", "foo.qjsbc", "-t", "500"};
  int r = parse_cmd_args(sizeof(argv) / sizeof(argv[0]), argv, cmdargs_errlog,
                         &cmdargs);
  TEST_ASSERT(r == 0);
  TEST_ASSERT(cmdargs.max_command_runtime_us == 500);
}

static void TEST_cmdargs_dash_t_error_on_0(void) {
  CmdArgs cmdargs = {0};
  char *argv[] = {"jsockd", "-s", "/tmp/sock", "-m", "foo.qjsbc", "-t", "0"};
  int r = parse_cmd_args(sizeof(argv) / sizeof(argv[0]), argv, cmdargs_errlog,
                         &cmdargs);
  TEST_ASSERT(r != 0);
  TEST_ASSERT(strstr(cmdargs_errlog_buf, "-t "));
}

static void TEST_cmdargs_dash_t_error_on_negative(void) {
  CmdArgs cmdargs = {0};
  char *argv[] = {"jsockd", "-s", "/tmp/sock", "-m", "foo.qjsbc", "-t", "-1"};
  int r = parse_cmd_args(sizeof(argv) / sizeof(argv[0]), argv, cmdargs_errlog,
                         &cmdargs);
  TEST_ASSERT(r != 0);
  TEST_ASSERT(strstr(cmdargs_errlog_buf, "-t "));
}

static void TEST_cmdargs_dash_t_error_on_non_numeric(void) {
  CmdArgs cmdargs = {0};
  char *argv[] = {"jsockd",    "-s", "/tmp/sock", "-m",
                  "foo.qjsbc", "-t", "sfdsff"};
  int r = parse_cmd_args(sizeof(argv) / sizeof(argv[0]), argv, cmdargs_errlog,
                         &cmdargs);
  TEST_ASSERT(r != 0);
  TEST_ASSERT(strstr(cmdargs_errlog_buf, "-t "));
}

static void TEST_cmdargs_dash_t_error_on_double_flag(void) {
  CmdArgs cmdargs = {0};
  char *argv[] = {"jsockd", "-t",        "500", "-s", "/tmp/sock",
                  "-m",     "foo.qjsbc", "-t",  "50"};
  int r = parse_cmd_args(sizeof(argv) / sizeof(argv[0]), argv, cmdargs_errlog,
                         &cmdargs);
  TEST_ASSERT(r != 0);
  TEST_ASSERT(strstr(cmdargs_errlog_buf, "-t "));
}

static void TEST_cmdargs_dash_i(void) {
  CmdArgs cmdargs = {0};
  char *argv[] = {"jsockd", "-s", "/tmp/sock", "-m", "foo.qjsbc", "-i", "500"};
  int r = parse_cmd_args(sizeof(argv) / sizeof(argv[0]), argv, cmdargs_errlog,
                         &cmdargs);
  TEST_ASSERT(r == 0);
  TEST_ASSERT(cmdargs.max_idle_time_us == 500);
}

static void TEST_cmdargs_dash_i_allows_0(void) {
  CmdArgs cmdargs = {0};
  char *argv[] = {"jsockd", "-s", "/tmp/sock", "-m", "foo.qjsbc", "-i", "0"};
  int r = parse_cmd_args(sizeof(argv) / sizeof(argv[0]), argv, cmdargs_errlog,
                         &cmdargs);
  TEST_ASSERT(r == 0);
  TEST_ASSERT(cmdargs.max_idle_time_us == 0 && cmdargs.max_idle_time_set);
}

static void TEST_cmdargs_dash_i_error_on_negative(void) {
  CmdArgs cmdargs = {0};
  char *argv[] = {"jsockd", "-s", "/tmp/sock", "-m", "foo.qjsbc", "-i", "-1"};
  int r = parse_cmd_args(sizeof(argv) / sizeof(argv[0]), argv, cmdargs_errlog,
                         &cmdargs);
  TEST_ASSERT(r != 0);
  TEST_ASSERT(strstr(cmdargs_errlog_buf, "-i "));
}

static void TEST_cmdargs_dash_i_error_on_non_numeric(void) {
  CmdArgs cmdargs = {0};
  char *argv[] = {"jsockd",    "-s", "/tmp/sock", "-m",
                  "foo.qjsbc", "-i", "sfdsff"};
  int r = parse_cmd_args(sizeof(argv) / sizeof(argv[0]), argv, cmdargs_errlog,
                         &cmdargs);
  TEST_ASSERT(r != 0);
  TEST_ASSERT(strstr(cmdargs_errlog_buf, "-i "));
}

static void TEST_cmdargs_dash_i_error_on_double_flag(void) {
  CmdArgs cmdargs = {0};
  char *argv[] = {"jsockd", "-i",        "500", "-s", "/tmp/sock",
                  "-m",     "foo.qjsbc", "-i",  "50"};
  int r = parse_cmd_args(sizeof(argv) / sizeof(argv[0]), argv, cmdargs_errlog,
                         &cmdargs);
  TEST_ASSERT(r != 0);
  TEST_ASSERT(strstr(cmdargs_errlog_buf, "-i "));
}

static void TEST_cmdargs_dash_k(void) {
  CmdArgs cmdargs = {0};
  char *argv[] = {"jsockd", "-k", "keyfileprefix"};
  int r = parse_cmd_args(sizeof(argv) / sizeof(argv[0]), argv, cmdargs_errlog,
                         &cmdargs);
  TEST_ASSERT(r == 0);
  TEST_ASSERT(!strcmp(cmdargs.key_file_prefix, "keyfileprefix"));
}

static void TEST_cmdargs_dash_k_error_on_missing_arg(void) {
  CmdArgs cmdargs = {0};
  char *argv[] = {"jsockd", "-k"};
  int r = parse_cmd_args(sizeof(argv) / sizeof(argv[0]), argv, cmdargs_errlog,
                         &cmdargs);
  TEST_ASSERT(r != 0);
  TEST_ASSERT(strstr(cmdargs_errlog_buf, "-k "));
}

static void TEST_cmdargs_dash_k_error_if_combined_with_other_flags(void) {
  CmdArgs cmdargs = {0};
  char *argv[] = {"jsockd", "-k", "prefix", "-m", "foo.qjsbc"};
  int r = parse_cmd_args(sizeof(argv) / sizeof(argv[0]), argv, cmdargs_errlog,
                         &cmdargs);
  TEST_ASSERT(r != 0);
  TEST_ASSERT(strstr(cmdargs_errlog_buf, "-k "));
}

static void TEST_cmdargs_dash_c(void) {
  CmdArgs cmdargs = {0};
  char *argv[] = {"jsockd", "-c", "module.mjs", "out.qjsbc"};
  int r = parse_cmd_args(sizeof(argv) / sizeof(argv[0]), argv, cmdargs_errlog,
                         &cmdargs);
  TEST_ASSERT(r == 0);
  TEST_ASSERT(!strcmp(cmdargs.mod_to_compile, "module.mjs"));
  TEST_ASSERT(!strcmp(cmdargs.mod_output_file, "out.qjsbc"));
}

static void TEST_cmdargs_dash_c_with_dash_k(void) {
  CmdArgs cmdargs = {0};
  char *argv[] = {"jsockd", "-c", "module.mjs", "out.qjsbc", "-k", "keyprefix"};
  int r = parse_cmd_args(sizeof(argv) / sizeof(argv[0]), argv, cmdargs_errlog,
                         &cmdargs);
  TEST_ASSERT(r == 0);
  TEST_ASSERT(!strcmp(cmdargs.mod_to_compile, "module.mjs"));
  TEST_ASSERT(!strcmp(cmdargs.mod_output_file, "out.qjsbc"));
  TEST_ASSERT(!strcmp(cmdargs.key_file_prefix, "keyprefix"));
}

static void TEST_cmdargs_dash_c_with_dash_k_error_if_dash_k_has_no_arg(void) {
  CmdArgs cmdargs = {0};
  char *argv[] = {"jsockd", "-c", "module.mjs", "out.qjsbc", "-k"};
  int r = parse_cmd_args(sizeof(argv) / sizeof(argv[0]), argv, cmdargs_errlog,
                         &cmdargs);
  TEST_ASSERT(r != 0);
  TEST_ASSERT(strstr(cmdargs_errlog_buf, "-k "));
}

static void TEST_cmdargs_dash_c_error_on_no_args(void) {
  CmdArgs cmdargs = {0};
  char *argv[] = {"jsockd", "-c"};
  int r = parse_cmd_args(sizeof(argv) / sizeof(argv[0]), argv, cmdargs_errlog,
                         &cmdargs);
  TEST_ASSERT(r != 0);
  TEST_ASSERT(strstr(cmdargs_errlog_buf, "-c "));
}

static void TEST_cmdargs_dash_c_error_on_only_one_arg(void) {
  CmdArgs cmdargs = {0};
  char *argv[] = {"jsockd", "-c", "module.mjs"};
  int r = parse_cmd_args(sizeof(argv) / sizeof(argv[0]), argv, cmdargs_errlog,
                         &cmdargs);
  TEST_ASSERT(r != 0);
  TEST_ASSERT(strstr(cmdargs_errlog_buf, "-c "));
}

static void TEST_cmdargs_dash_c_error_if_combined_with_other_flags(void) {
  CmdArgs cmdargs = {0};
  char *argv[] = {"jsockd", "-c", "module.mjs", "out.qjsbc", "-m", "foo.qjsbc"};
  int r = parse_cmd_args(sizeof(argv) / sizeof(argv[0]), argv, cmdargs_errlog,
                         &cmdargs);
  TEST_ASSERT(r != 0);
  TEST_ASSERT(strstr(cmdargs_errlog_buf, "-c "));
}

static void TEST_cmdargs_dash_c_can_be_combined_with_dash_ss(void) {
  CmdArgs cmdargs = {0};
  char *argv[] = {"jsockd", "-c", "module.mjs", "out.qjsbc", "-ss"};
  int r = parse_cmd_args(sizeof(argv) / sizeof(argv[0]), argv, cmdargs_errlog,
                         &cmdargs);
  TEST_ASSERT(r == 0);
}

static void TEST_cmdargs_dash_c_can_be_combined_with_dash_sd(void) {
  CmdArgs cmdargs = {0};
  char *argv[] = {"jsockd", "-c", "module.mjs", "out.qjsbc", "-sd"};
  int r = parse_cmd_args(sizeof(argv) / sizeof(argv[0]), argv, cmdargs_errlog,
                         &cmdargs);
  TEST_ASSERT(r == 0);
}

static void TEST_cmdargs_dash_ss_must_be_combined_with_dash_c(void) {
  CmdArgs cmdargs = {0};
  char *argv[] = {"jsockd", "-ss"};
  int r = parse_cmd_args(sizeof(argv) / sizeof(argv[0]), argv, cmdargs_errlog,
                         &cmdargs);
  TEST_ASSERT(r != 0);
  TEST_ASSERT(strstr(cmdargs_errlog_buf, "-ss "));
}

static void TEST_cmdargs_dash_sd_must_be_combined_with_dash_c(void) {
  CmdArgs cmdargs = {0};
  char *argv[] = {"jsockd", "-sd"};
  int r = parse_cmd_args(sizeof(argv) / sizeof(argv[0]), argv, cmdargs_errlog,
                         &cmdargs);
  TEST_ASSERT(r != 0);
  TEST_ASSERT(strstr(cmdargs_errlog_buf, "-sd "));
}

static void TEST_cmdargs_dash_ss_and_dash_sd_are_mutually_exclusive(void) {
  CmdArgs cmdargs = {0};
  char *argv[] = {"jsockd", "-c", "module.mjs", "out.qjsbc", "-sd", "-ss"};
  int r = parse_cmd_args(sizeof(argv) / sizeof(argv[0]), argv, cmdargs_errlog,
                         &cmdargs);
  TEST_ASSERT(r != 0);
  TEST_ASSERT(strstr(cmdargs_errlog_buf, "-sd "));
  TEST_ASSERT(strstr(cmdargs_errlog_buf, "-ss "));
}

/******************************************************************************
    Tests for modcompiler
******************************************************************************/

static void TEST_compile_module_file(void) {
  const char privkey[] =
      "6FC575BE3557E26A42B1A09BEEF0F4BFC9C6BBFBDAABB1E2098387A03599CE2B780EA777"
      "5430EE1083077387B652921E9D1C5CCB5DA3634BFC7B6B46C84E8578920D6160618D2961"
      "537E4B1DF6F3215F1AD186A1B45FED9869AFA636F5E10910";

  TEST_ASSERT(sizeof(privkey) - 1 ==
              2 * (ED25519_PUBLIC_KEY_SIZE + ED25519_PRIVATE_KEY_SIZE));

  char tmpdir[1024];
  TEST_ASSERT(0 == make_temp_dir(tmpdir, sizeof(tmpdir),
                                 "jsockd_TEST_compile_module_file_XXXXXX"));

  char module_filename[sizeof(tmpdir) + 128];
  char key_filename[sizeof(tmpdir) + 128];
  char output_filename[sizeof(tmpdir) + 128];
  snprintf_nowarn(module_filename, sizeof(module_filename), "%s/mod.mjs",
                  tmpdir);
  snprintf_nowarn(key_filename, sizeof(key_filename), "%s/key", tmpdir);
  snprintf_nowarn(output_filename, sizeof(output_filename), "%s/out.qjsbc",
                  tmpdir);

  FILE *modulef = fopen(module_filename, "w");
  FILE *keyf = fopen(key_filename, "w");

  TEST_ASSERT(modulef && keyf);

  TEST_ASSERT(0 <= fprintf(modulef, "export const foo = 17;\n"));
  TEST_ASSERT(1 == fwrite(privkey,
                          sizeof(privkey) / sizeof(char) - sizeof(char), 1,
                          keyf));

  fclose(modulef);
  fclose(keyf);

  TEST_ASSERT(EXIT_SUCCESS == compile_module_file(module_filename, key_filename,
                                                  output_filename, "99.99.0",
                                                  0));

  FILE *outf = fopen(output_filename, "r");
  TEST_ASSERT(outf);

  TEST_ASSERT(0 == fseek(outf, 0, SEEK_END));
  size_t output_file_size = ftell(outf);
  TEST_ASSERT(0 == fseek(outf, 0, SEEK_SET));

  uint8_t signature[64];
  char version[VERSION_STRING_SIZE];
  size_t bytecode_size = output_file_size - ED25519_SIGNATURE_SIZE;
  uint8_t bytecode[bytecode_size];
  TEST_ASSERT(0 == fseek(outf, -ED25519_SIGNATURE_SIZE, SEEK_END));
  TEST_ASSERT(1 == fread(signature, sizeof(signature) / sizeof(char), 1, outf));
  TEST_ASSERT(0 == fseek(outf, -ED25519_SIGNATURE_SIZE - VERSION_STRING_SIZE,
                         SEEK_END));
  TEST_ASSERT(1 == fread(version, sizeof(version) / sizeof(char), 1, outf));
  TEST_ASSERT(version[VERSION_STRING_SIZE - 1] == '\0');
  TEST_ASSERT(!strcmp(version, "99.99.0"));
  TEST_ASSERT(0 == fseek(outf, 0, SEEK_SET));
  TEST_ASSERT(1 == fread(bytecode, bytecode_size, 1, outf));

  uint8_t pubkey_raw[ED25519_PUBLIC_KEY_SIZE];
  // we store public key as first 32 bytes of private key file
  TEST_ASSERT(ED25519_PUBLIC_KEY_SIZE ==
              hex_decode(pubkey_raw, sizeof(pubkey_raw), privkey));

  TEST_ASSERT(ed25519_verify(signature, bytecode, bytecode_size, pubkey_raw));

  fclose(outf);
  remove(module_filename);
  remove(key_filename);
  remove(output_filename);
  rmdir(tmpdir);
}

static void TEST_compile_module_file_without_key(void) {
  char tmpdir[1024];
  TEST_ASSERT(
      0 == make_temp_dir(tmpdir, sizeof(tmpdir),
                         "jsockd_TEST_compile_module_file_without_key_XXXXXX"));

  char module_filename[sizeof(tmpdir) + 128];
  char output_filename[sizeof(tmpdir) + 128];
  snprintf_nowarn(module_filename, sizeof(module_filename), "%s/mod.mjs",
                  tmpdir);
  snprintf_nowarn(output_filename, sizeof(output_filename), "%s/out.qjsbc",
                  tmpdir);

  FILE *modulef = fopen(module_filename, "w");

  TEST_ASSERT(modulef);

  TEST_ASSERT(0 <= fprintf(modulef, "export const foo = 17;\n"));

  fclose(modulef);

  TEST_ASSERT(EXIT_SUCCESS == compile_module_file(module_filename, NULL,
                                                  output_filename, "99.99.0",
                                                  0));

  FILE *outf = fopen(output_filename, "r");
  TEST_ASSERT(outf);

  TEST_ASSERT(0 == fseek(outf, 0, SEEK_END));
  TEST_ASSERT(0 == fseek(outf, 0, SEEK_SET));

  uint8_t signature[64];
  char version[VERSION_STRING_SIZE];
  TEST_ASSERT(0 == fseek(outf, -ED25519_SIGNATURE_SIZE, SEEK_END));
  TEST_ASSERT(1 == fread(signature, sizeof(signature) / sizeof(char), 1, outf));
  TEST_ASSERT(0 == fseek(outf, -ED25519_SIGNATURE_SIZE - VERSION_STRING_SIZE,
                         SEEK_END));
  TEST_ASSERT(1 == fread(version, sizeof(version) / sizeof(char), 1, outf));
  TEST_ASSERT(version[VERSION_STRING_SIZE - 1] == '\0');
  TEST_ASSERT(!strcmp(version, "99.99.0"));

  // absent signature should be padded with zeros
  for (size_t i = 0; i < sizeof(signature); ++i) {
    TEST_ASSERT(signature[i] == 0);
  }

  fclose(outf);
  remove(module_filename);
  remove(output_filename);
  rmdir(tmpdir);
}

static void TEST_output_key_file(void) {
  char tmpdir[1024];
  TEST_ASSERT(0 == make_temp_dir(tmpdir, sizeof(tmpdir),
                                 "jsockd_TEST_modcompiler_XXXXXX"));
  char keyprefixpath[sizeof(tmpdir) + sizeof("my_key")];
  snprintf_nowarn(keyprefixpath, sizeof(keyprefixpath), "%s/my_key", tmpdir);

  output_key_file(keyprefixpath);

  char pubkeypath[sizeof(keyprefixpath) + sizeof(".pubkey")];
  snprintf_nowarn(pubkeypath, sizeof(keyprefixpath), "%s/my_key.pubkey",
                  tmpdir);
  FILE *pubhexf = fopen(pubkeypath, "r");
  char privkeypath[sizeof(keyprefixpath) + sizeof(".privkey")];
  snprintf_nowarn(privkeypath, sizeof(keyprefixpath), "%s/my_key.privkey",
                  tmpdir);
  FILE *privhexf = fopen(privkeypath, "r");
  TEST_ASSERT(pubhexf && privhexf);

  char pubhex[ED25519_PUBLIC_KEY_SIZE * 2];
  char privhex[2 * (ED25519_PRIVATE_KEY_SIZE + ED25519_PUBLIC_KEY_SIZE)];

  TEST_ASSERT(1 == fread(pubhex, sizeof(pubhex) / sizeof(char), 1, pubhexf));
  TEST_ASSERT(1 == fread(privhex, sizeof(privhex) / sizeof(char), 1, privhexf));

  fseek(pubhexf, 0, SEEK_END);
  fseek(privhexf, 0, SEEK_END);
  TEST_ASSERT(ED25519_PUBLIC_KEY_SIZE * 2 == ftell(pubhexf));
  TEST_ASSERT(2 * (ED25519_PRIVATE_KEY_SIZE + ED25519_PUBLIC_KEY_SIZE) ==
              ftell(privhexf));

  fclose(pubhexf);
  fclose(privhexf);
  remove(privkeypath);
  remove(pubkeypath);
  rmdir(tmpdir);

  // First 32 bytes of privkey file should match pubkey file
  TEST_ASSERT(0 == memcmp(pubhex, privhex, ED25519_PUBLIC_KEY_SIZE * 2));
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
             T(line_buf_truncation_then_normal_read),
             T(line_buf_replay_empty_case),
             T(line_buf_replay_error_case),
             T(hex_decode_empty_string_zero_length),
             T(hex_decode_nonempty_string_zero_length),
             T(hex_decode_gives_expected_result),
             T(cmdargs_returns_error_for_no_args),
             T(cmdargs_returns_error_for_one_arg),
             T(cmdargs_returns_success_for_just_one_socket),
             T(cmdargs_returns_success_for_multiple_sockets),
             T(cmdargs_returns_error_for_multiple_dash_b),
             T(cmdargs_returns_success_for_full_set_of_options),
             T(cmdargs_returns_error_for_multiple_dash_m),
             T(cmdargs_returns_error_if_no_sockets_specified),
             T(cmdargs_returns_error_if_s_has_no_arg),
             T(cmdargs_handles_more_sockets_than_MAX_THREADS),
             T(cmdargs_dash_v),
             T(cmdargs_dash_sm),
             T(cmdargs_dash_sm_returns_error_if_dash_m_not_present),
             T(cmdargs_returns_error_if_dash_v_combined_with_other_opts),
             T(cmdargs_returns_error_if_dash_v_has_arg),
             T(cmdargs_dash_t),
             T(cmdargs_dash_t_error_on_0),
             T(cmdargs_dash_t_error_on_negative),
             T(cmdargs_dash_t_error_on_non_numeric),
             T(cmdargs_dash_t_error_on_double_flag),
             T(cmdargs_dash_i),
             T(cmdargs_dash_i_allows_0),
             T(cmdargs_dash_i_error_on_negative),
             T(cmdargs_dash_i_error_on_non_numeric),
             T(cmdargs_dash_i_error_on_double_flag),
             T(cmdargs_dash_k),
             T(cmdargs_dash_c_with_dash_k),
             T(cmdargs_dash_c_with_dash_k_error_if_dash_k_has_no_arg),
             T(cmdargs_dash_k_error_on_missing_arg),
             T(cmdargs_dash_k_error_if_combined_with_other_flags),
             T(cmdargs_dash_c),
             T(cmdargs_dash_c_error_on_no_args),
             T(cmdargs_dash_c_error_on_only_one_arg),
             T(cmdargs_dash_c_error_if_combined_with_other_flags),
             T(cmdargs_dash_ss_and_dash_sd_are_mutually_exclusive),
             T(cmdargs_dash_sd_must_be_combined_with_dash_c),
             T(cmdargs_dash_ss_must_be_combined_with_dash_c),
             T(cmdargs_dash_c_can_be_combined_with_dash_sd),
             T(cmdargs_dash_c_can_be_combined_with_dash_ss),
             T(compile_module_file),
             T(compile_module_file_without_key),
             T(output_key_file),
             {NULL, NULL}};
