#define _REENTRANT
#include "cmdargs.h"
#include "config.h"
#include "custom_module_loader.h"
#include "fchmod.h"
#include "hash_cache.h"
#include "hex.h"
#include "line_buf.h"
#include "mmap_file.h"
#include "quickjs-libc.h"
#include "quickjs.h"
#include "utils.h"
#include "verify_bytecode.h"
#include "wait_group.h"
#include <assert.h>
#include <err.h>
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <memory.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

// Code for dealing with backteaces and source maps is written in JS and then
// compiled to bytecode in the build process.
extern const uint32_t g_backtrace_module_bytecode_size;
extern const uint8_t g_backtrace_module_bytecode[];

static const uint8_t *g_module_bytecode;
static size_t g_module_bytecode_size;

static atomic_int g_n_threads;

static const uint8_t *g_source_map;
static size_t g_source_map_size;
static atomic_int g_source_map_load_count; // once all threads have loaded the
// source map, we can munmap the file

// Testing scenarios with collisions is less labor intensive if we use a smaller
// number of bits in the debug build.
#define CACHED_FUNCTION_HASH_BITS                                              \
  (CMAKE_BUILD_TYPE_IS_DEBUG ? CACHED_FUNCTIONS_HASH_BITS_DEBUG                \
                             : CACHED_FUNCTIONS_HASH_BITS_RELEASE)
#define CACHED_FUNCTIONS_N_BUCKETS                                             \
  HASH_CACHE_BUCKET_ARRAY_SIZE_FROM_HASH_BITS(CACHED_FUNCTION_HASH_BITS)

static atomic_bool g_interrupted_or_error;

// Global vars that need destruction before exit.
static WaitGroup g_thread_ready_wait_group;
static pthread_mutex_t g_cached_functions_mutex;

static atomic_bool g_global_init_complete;

static void dump_error(JSContext *ctx) {
  if (CMAKE_BUILD_TYPE_IS_DEBUG)
    js_std_dump_error(ctx);
  else
    JS_FreeValue(ctx, JS_GetException(ctx));
}

// The input buffer gets malloced to this size once per thread. Longer inputs
// are truncated. I tried starting with a smaller buffer and reallocing as
// needed, but it doesn't actually improve memory usage. I guess most of the
// large allocation doesn't get paged in till it's needed anyway.
static const int LINE_BUF_BYTES = 1024 * 1024 * 1024;

typedef struct {
  const uint8_t *bytecode;
  size_t bytecode_size;
} cached_function_t;

static HashCacheBucket g_cached_function_buckets[CACHED_FUNCTIONS_N_BUCKETS];
static cached_function_t g_cached_functions[CACHED_FUNCTIONS_N_BUCKETS];

static void add_cached_function(uint64_t uid, const uint8_t *bytecode,
                                size_t bytecode_size) {
  assert(0 != JS_TAG_FUNCTION_BYTECODE);
  assert(bytecode);

  mutex_lock(&g_cached_functions_mutex);

  HashCacheBucket *b = add_to_hash_cache(g_cached_function_buckets,
                                         CACHED_FUNCTION_HASH_BITS, uid);
  size_t bi = b - g_cached_function_buckets;
  if (g_cached_functions[bi].bytecode) {
    debug_log("Hash collision: freeing existing bytecode\n");
    free((void *)(g_cached_functions[bi].bytecode));
  }
  g_cached_functions[bi].bytecode = bytecode;
  g_cached_functions[bi].bytecode_size = bytecode_size;

  mutex_unlock(&g_cached_functions_mutex);
}

static const uint8_t *get_cached_function(uint64_t uid, size_t *psize) {
  mutex_lock(&g_cached_functions_mutex);
  HashCacheBucket *b = get_hash_cache_entry(g_cached_function_buckets,
                                            CACHED_FUNCTION_HASH_BITS, uid);
  mutex_unlock(&g_cached_functions_mutex);
  if (b) {
    size_t bi = b - g_cached_function_buckets;
    *psize = g_cached_functions[bi].bytecode_size;
    return g_cached_functions[bi].bytecode;
  }
  return NULL;
}

// The state for each thread which runs a QuickJS VM.
typedef struct {
  const char *unix_socket_filename;
  JSRuntime *rt;
  JSContext *ctx;
  int sockfd;
  int streamfd;
  int stream_io_err;
  int exit_status;
  pthread_mutex_t doing_js_stuff_mutex;
  int line_n;
  JSValue compiled_module;
  JSValue compiled_query;
  JSValue backtrace_module;
  struct timespec last_js_execution_start;
  char current_uuid[MESSAGE_UUID_MAX_BYTES + 1 /*zeroterm*/];
  size_t current_uuid_len;
  int memory_check_count;
  int memory_increase_count;
  int64_t last_memory_usage;
  char error_msg_buf[8192];
  bool truncated;
  JSValue sourcemap_str;
} ThreadState;

static void js_print_value_debug_write(void *opaque, const char *buf,
                                       size_t len) {
  release_logf("%.*s", len, buf);
}

typedef enum { READY, SIG_INTERRUPT_OR_ERROR, GO_AROUND } PollFdResult;

static PollFdResult poll_fd(int fd) {
  struct pollfd pfd = {.fd = fd, .events = POLLIN | POLLPRI};
  if (!poll(&pfd, 1, SOCKET_POLL_TIMEOUT_MS)) {
    if (atomic_load(&g_interrupted_or_error))
      return SIG_INTERRUPT_OR_ERROR;
    return GO_AROUND;
  }
  return READY;
}

static int lb_read(char *buf, size_t n, void *data) {
  for (;;) {
    int r = read(*(int *)data, buf, n);
    if (r == -1 && errno == EINTR)
      continue; // interrupted, try again
    return r;
  }
}

static const int EXIT_ON_QUIT_COMMAND = -999;

static CmdArgs g_cmd_args;

static void listen_on_unix_socket(const char *unix_socket_filename,
                                  int (*line_handler)(const char *line,
                                                      size_t len, void *data,
                                                      bool truncated),
                                  void *data) {
  ThreadState *ts = (ThreadState *)data;

  char *line_buf_buffer = calloc(LINE_BUF_BYTES, sizeof(char));
  LineBuf line_buf = {.buf = line_buf_buffer, .size = LINE_BUF_BYTES};

  ts->sockfd = socket(AF_UNIX, SOCK_STREAM, 0);
  if (ts->sockfd < 0) {
    ts->exit_status = -1;
    goto error;
  }

  if (0 != socket_fchmod(ts->sockfd, 0600)) {
    release_logf("Error setting permissions 0600 on socket %s: %s\n",
                 unix_socket_filename, strerror(errno));
    ts->exit_status = -1;
    goto error;
  }

  struct sockaddr_un addr = {0};
  addr.sun_family = AF_UNIX;
  if (sizeof(addr.sun_path) / sizeof(addr.sun_path[0]) <
      strlen(unix_socket_filename) + 1 /* zeroterm */) {
    release_logf("Error: unix socket filename %s is too long\n",
                 unix_socket_filename);
    ts->exit_status = -1;
    goto error;
  }
  strncpy(addr.sun_path, unix_socket_filename, sizeof(addr.sun_path) - 1);
  if (-1 == unlink(unix_socket_filename) && errno != ENOENT) {
    release_logf("Error attempting to unlink %s\n", unix_socket_filename);
    ts->exit_status = -1;
    goto error;
  }
  if (0 != bind(ts->sockfd, (struct sockaddr *)&addr, sizeof(addr))) {
    ts->exit_status = -1;
    goto error;
  }
  if (0 != listen(ts->sockfd, SOMAXCONN)) {
    ts->exit_status = -1;
    goto error;
  }

  // On Mac the call to socket_fchmod above is a no-op, so call chmod on the
  // UNIX socket filename. This is theoretically less good, because there's a
  // tiny window where the socket file exists but is not yet chmodded.
  if (0 != chmod(unix_socket_filename, 0600)) {
    release_logf(
        "Error setting permissions 0600 on socket %s via filename: %s\n",
        unix_socket_filename, strerror(errno));
    ts->exit_status = -1;
    goto error;
  }

  if (0 != wait_group_inc(&g_thread_ready_wait_group, 1)) {
    release_log("Error incrementing thread ready wait group\n");
    ts->exit_status = -1;
    goto error_no_inc;
  }

  ts->streamfd = -1;
  for (;;) {
  accept_loop:
    switch (poll_fd(ts->sockfd)) {
    case READY:
      break;
    case GO_AROUND:
      goto accept_loop;
    case SIG_INTERRUPT_OR_ERROR: {
      ts->exit_status = -1;
      goto error_no_inc;
    } break;
    }

    socklen_t streamfd_size = sizeof(struct sockaddr);
    ts->streamfd = accept(ts->sockfd, (struct sockaddr *)&addr, &streamfd_size);
    debug_log("Accepted on ts->socket\n");
    if (ts->streamfd < 0) {
      ts->exit_status = -1;
      goto error_no_inc;
    }
    break;
  }

  for (;;) {
  read_loop:
    switch (poll_fd(ts->streamfd)) {
    case READY:
      break;
    case GO_AROUND:
      goto read_loop;
    case SIG_INTERRUPT_OR_ERROR: {
      ts->exit_status = -1;
      goto error_no_inc;
    } break;
    }

    int exit_value = line_buf_read(&line_buf, g_cmd_args.socket_sep_char,
                                   lb_read, &ts->streamfd, line_handler, data);
    if (exit_value == EXIT_ON_QUIT_COMMAND)
      ; // "?quit"
    else if (exit_value < 0)
      ts->exit_status = -1;
    if (exit_value <= 0)
      goto error_no_inc; // EOF or error
  }

error:
  // Increment the wait group to indicate that this thread is ready, so that
  // all threads can be joined in main. The other threads will notice that
  // g_interrupted_or_error has been set to true, and thus exit gracefully
  // in due course.
  if (0 != wait_group_inc(&g_thread_ready_wait_group, 1))
    release_log(
        "Error incrementing thread ready wait group in error condition\n");
error_no_inc:
  if (ts->streamfd >= 0)
    close(ts->streamfd);
  if (ts->sockfd >= 0)
    close(ts->sockfd);
  // indicate they're closed so we don't try to close them again in the main
  // teardown
  ts->streamfd = -1;
  ts->sockfd = -1;

  free(line_buf_buffer);

  atomic_store(&g_interrupted_or_error, true);
}

static JSContext *JS_NewCustomContext(JSRuntime *rt) {
  JSContext *ctx;
  ctx = JS_NewContext(rt);
  if (!ctx)
    return NULL;
  js_init_module_std(ctx, "std");
  js_init_module_os(ctx, "os");
  return ctx;
}

static const uint8_t *compile_buf(JSContext *ctx, const char *buf, int buf_len,
                                  size_t *bytecode_size) {
  JSValue val = JS_Eval(ctx, (const char *)buf, buf_len, "<buffer>",
                        JS_EVAL_FLAG_ASYNC | JS_EVAL_FLAG_COMPILE_ONLY);
  if (JS_IsException(val)) {
    dump_error(ctx);
    JS_FreeValue(ctx, val);
    return NULL;
  }

  const uint8_t *bytecode =
      JS_WriteObject(ctx, bytecode_size, val, JS_WRITE_OBJ_BYTECODE);
  JS_FreeValue(ctx, val);
  debug_logf("Compiled bytecode size: %zu\n", *bytecode_size);

  // We want to preserve the bytecode across runtime contexts, but js_free
  // requires a context for some refcounting. So copy this over to some malloc'd
  // memory.
  const uint8_t *malloc_bytecode = malloc(*bytecode_size);
  if (!malloc_bytecode) {
    js_free(ctx, (void *)bytecode);
    return NULL;
  }
  memcpy((void *)malloc_bytecode, bytecode, *bytecode_size);
  js_free(ctx, (void *)bytecode);

  return (const uint8_t *)malloc_bytecode;
}

static JSValue func_from_bytecode(JSContext *ctx, const uint8_t *bytecode,
                                  int len) {
  JSValue val = JS_ReadObject(ctx, bytecode, len, JS_READ_OBJ_BYTECODE);
  if (JS_IsException(val)) {
    debug_log("Exception returned when reading bytecode via JS_ReadObject\n");
    dump_error(ctx);
    return val;
  }
  JSValue evald = JS_EvalFunction(ctx, val); // this call frees val
  evald = js_std_await(ctx, evald);
  if (JS_VALUE_GET_TAG(evald) != JS_TAG_OBJECT) {
    debug_log("JS_EvalFunction did not return an object\n");
    if (CMAKE_BUILD_TYPE_IS_DEBUG && JS_IsException(evald))
      dump_error(ctx);
    JS_FreeValue(ctx, evald);
    return JS_EXCEPTION;
  }
  JSValue r = JS_GetPropertyStr(ctx, evald, "value");
  JS_FreeValue(ctx, evald);
  return r;
}

static int interrupt_handler(JSRuntime *rt, void *opaque) {
  ThreadState *state = (ThreadState *)opaque;
  struct timespec *start = &state->last_js_execution_start;
  if (start->tv_sec != 0) {
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC_RAW, &now);
    uint64_t delta_us = (uint64_t)(now.tv_sec - start->tv_sec) * 1000000ULL +
                        (uint64_t)(now.tv_nsec - start->tv_nsec) / 1000ULL;
    return delta_us > g_cmd_args.max_command_runtime_us;
  }
  return (int)atomic_load(&g_interrupted_or_error);
}

static int init_thread_state(ThreadState *ts,
                             const char *unix_socket_filename) {
  JSRuntime *rt = JS_NewRuntime();
  if (!rt) {
    release_log("Failed to create JS runtime\n");
    return -1;
  }

  js_std_set_worker_new_context_func(JS_NewCustomContext);
  js_std_init_handlers(rt);
  JSContext *ctx = JS_NewCustomContext(rt);
  if (!ctx) {
    release_log("Failed to create JS context\n");
    JS_FreeRuntime(rt);
    return -1;
  }

  JS_SetModuleLoaderFunc2(rt, NULL, jsockd_js_module_loader,
                          js_module_check_attributes, NULL);
  JS_SetInterruptHandler(rt, interrupt_handler, ts);

  // Load the precompiled module.
  if (g_module_bytecode)
    ts->compiled_module =
        load_binary_module(ctx, g_module_bytecode, g_module_bytecode_size);
  else
    ts->compiled_module = JS_UNDEFINED;
  if (JS_IsException(ts->compiled_module)) {
    release_log("Failed to load precompiled module\n");
    js_std_dump_error(ctx);
    JS_FreeValue(ctx, ts->compiled_module);
    JS_FreeContext(ctx);
    JS_FreeRuntime(rt);
    return -1;
  }

  if (unix_socket_filename) { // it's not a reinit
    ts->unix_socket_filename = unix_socket_filename;
    ts->sockfd = -1;   // to be set later
    ts->streamfd = -1; // to be set later
    mutex_init(&ts->doing_js_stuff_mutex);
  } else {
    // It is a reinit.
    assert(JS_IsUndefined(ts->compiled_query));
  }

  ts->backtrace_module = load_binary_module(ctx, g_backtrace_module_bytecode,
                                            g_backtrace_module_bytecode_size);
  assert(!JS_IsException(ts->backtrace_module));

  ts->rt = rt;
  ts->ctx = ctx;
  ts->stream_io_err = 0;
  // set to nonzero if program should eventually exit with non-zero exit code
  ts->exit_status = 0;
  ts->line_n = 0;
  ts->compiled_query = JS_UNDEFINED;
  ts->last_js_execution_start.tv_sec = 0;
  ts->last_js_execution_start.tv_nsec = 0;
  ts->current_uuid[0] = '\0';
  ts->current_uuid_len = 0;
  ts->memory_check_count = 0;
  ts->memory_increase_count = 0;
  ts->last_memory_usage = 0;
  ts->sourcemap_str = JS_UNDEFINED;

  return 0;
}

static void cleanup_js_runtime(ThreadState *ts) {
  JS_FreeValue(ts->ctx, ts->backtrace_module);
  JS_FreeValue(ts->ctx, ts->compiled_query);
  JS_FreeValue(ts->ctx, ts->compiled_module);
  JS_FreeValue(ts->ctx, ts->sourcemap_str);

  // Valgrind seems to correctly have caught a memory leak in quickjs-libc.
  // It's an inconsequential one, as it's an object that's allocated once at
  // the start of execution and that lives for the life of the application.
  // But to keep our valgrind output clean, let's fix it... See
  // quickjs-libc.c:4086 for the offending allocation.
  js_free(ts->ctx, JS_GetRuntimeOpaque(ts->rt));

  JS_FreeContext(ts->ctx);
  JS_FreeRuntime(ts->rt);
}

static void cleanup_thread_state(ThreadState *ts) {
  // Don't add logs to this function as it may be called from a signal
  // handler.

  // We're about to exit, so we don't need to check for errors.
  if (ts->streamfd != -1)
    close(ts->streamfd);
  unlink(ts->unix_socket_filename);

  cleanup_js_runtime(ts);

  // This could fail, but no useful error handling to be done (we're exiting
  // anyway).
  pthread_mutex_destroy(&ts->doing_js_stuff_mutex);
}

static void write_to_stream(ThreadState *ts, const char *buf, size_t len) {
  if (0 != write_all(ts->streamfd, buf, len)) {
    ts->stream_io_err = -1;
    release_logf("Error writing to socket: %s\n", strerror(errno));
    return;
  }
}

#define write_const_to_stream(ts, str)                                         \
  write_to_stream((ts), (str), sizeof(str) - 1)

static int handle_line_1_message_uid(ThreadState *ts, const char *line,
                                     int len) {
  if (len > MESSAGE_UUID_MAX_BYTES) {
    debug_logf("Error: message UUID has length %i and will be truncated to "
               "first %i bytes\n",
               len, MESSAGE_UUID_MAX_BYTES);
    len = MESSAGE_UUID_MAX_BYTES;
  }
  strncpy(ts->current_uuid, line, len);
  ts->current_uuid_len = len;
  ts->line_n++;
  return 0;
}

static int handle_line_2_query(ThreadState *ts, const char *line, int len) {
  const uint64_t uid = get_hash_cache_uid(line, len);
  size_t bytecode_size = 0;
  const uint8_t *bytecode = get_cached_function(uid, &bytecode_size);

  if (bytecode) {
    debug_log("Found cached function\n");
    ts->compiled_query = func_from_bytecode(ts->ctx, bytecode, bytecode_size);
  } else {
    debug_log("Compiling...\n");
    mutex_lock(&ts->doing_js_stuff_mutex);
    // We compile and cache the function.
    size_t bytecode_size;

    const uint8_t *bytecode = compile_buf(ts->ctx, line, len, &bytecode_size);
    if (!bytecode) {
      ts->compiled_query = JS_EXCEPTION;
    } else {
      add_cached_function(uid, bytecode, bytecode_size);
      ts->compiled_query = func_from_bytecode(ts->ctx, bytecode, bytecode_size);
    }
  }
  mutex_unlock(&ts->doing_js_stuff_mutex);

  ts->line_n++;
  return 0;
}

typedef struct {
  char *buf;
  size_t index;
  size_t length;
} WBuf;

static void write_to_buf(void *opaque_buf, const char *inp, size_t size) {
  WBuf *buf = (WBuf *)opaque_buf;
  size_t to_write =
      buf->length >= buf->index ? MIN(buf->length - buf->index, size) : 0;
  memcpy(buf->buf + buf->index, inp, to_write);
  buf->index += to_write;
}

static int handle_line_3_parameter(ThreadState *ts, const char *line, int len) {
  const JSPrintValueOptions js_print_value_options = {.show_hidden = false,
                                                      .raw_dump = false,
                                                      .max_depth = 0,
                                                      .max_string_length = 0,
                                                      .max_item_count = 0};

  ts->line_n = 0;

  if (JS_IsException(ts->compiled_query)) {
    mutex_lock(&ts->doing_js_stuff_mutex);
    if (CMAKE_BUILD_TYPE_IS_DEBUG) {
      JS_PrintValue(ts->ctx, js_print_value_debug_write, ts, ts->compiled_query,
                    &js_print_value_options);
      release_log("\n");
    }
    JS_FreeValue(ts->ctx, ts->compiled_query);
    ts->compiled_query = JS_UNDEFINED;
    mutex_unlock(&ts->doing_js_stuff_mutex);
    write_to_stream(ts, ts->current_uuid, ts->current_uuid_len);
    write_const_to_stream(ts, " exception \"error compiling command\"\n");
    return ts->stream_io_err;
  }

  mutex_lock(&ts->doing_js_stuff_mutex);
  clock_gettime(CLOCK_MONOTONIC_RAW, &ts->last_js_execution_start);
  JSValue parsed_arg = JS_ParseJSON(ts->ctx, line, len, "<input>");
  if (JS_IsException(parsed_arg)) {
    debug_logf("Error parsing JSON argument: <<END\n%.*sEND\n", len, line);
    dump_error(ts->ctx);
    JS_FreeValue(ts->ctx, parsed_arg);
    JS_FreeValue(ts->ctx, ts->compiled_query);
    ts->compiled_query = JS_UNDEFINED;
    mutex_unlock(&ts->doing_js_stuff_mutex);
    write_to_stream(ts, ts->current_uuid, ts->current_uuid_len);
    write_const_to_stream(ts, " exception \"JSON input parse error\"\n");
    return ts->stream_io_err;
  }

  JSValue argv[] = {ts->compiled_module, parsed_arg};
  JSValue ret = JS_Call(ts->ctx, ts->compiled_query, JS_NULL,
                        sizeof(argv) / sizeof(argv[0]), argv);
  ret = js_std_await(ts->ctx, ret); // allow return of a promise
  JS_FreeValue(ts->ctx, ts->compiled_query);
  ts->compiled_query = JS_UNDEFINED;
  if (JS_IsException(ret)) {
    debug_log("Error calling cached function\n");
    mutex_unlock(&ts->doing_js_stuff_mutex);
    write_to_stream(ts, ts->current_uuid, ts->current_uuid_len);
    write_const_to_stream(ts, " exception ");
    WBuf emb = {.buf = ts->error_msg_buf,
                .index = 0,
                .length = sizeof(ts->error_msg_buf)};
    JSValue exception = JS_GetException(ts->ctx);
    JS_PrintValue(ts->ctx, write_to_buf, &emb, exception, NULL);

    // Parse the error message and backtrace and encode it as JSON using
    // the code in src/js/backtrace.mjs. Not super efficient, but we only run
    // this code in the error case, so it doesn't matter too much.
    JSValue parseBacktrace =
        JS_GetPropertyStr(ts->ctx, ts->backtrace_module, "parseBacktrace");
    if (JS_IsUndefined(ts->sourcemap_str)) {
      ts->sourcemap_str =
          g_source_map_size == 0
              ? JS_UNDEFINED
              : JS_NewStringLen(ts->ctx, (const char *)g_source_map,
                                g_source_map_size);
      int c = atomic_fetch_add(&g_source_map_load_count, 1);
      if (c + 1 == g_n_threads) {
        debug_log("All threads have loaded the sourcemap, calling munmap...\n");
        munmap_or_warn((void *)g_source_map, g_source_map_size);
        g_source_map = NULL;
      }
    }
    JSValue backtrace_str = JS_NewStringLen(ts->ctx, emb.buf, emb.index);
    JSValue argv[] = {ts->sourcemap_str, backtrace_str};
    JSValue parsed_backtrace_js = JS_Call(ts->ctx, parseBacktrace, JS_UNDEFINED,
                                          sizeof(argv) / sizeof(argv[0]), argv);
    if (JS_IsException(parsed_backtrace_js)) {
      release_log("Error parsing backtrace\n");
      dump_error(ts->ctx);
      write_const_to_stream(ts, "{}\n");
    } else {
      size_t parsed_backtrace_len;
      const char *parsed_backtrace =
          JS_ToCStringLen(ts->ctx, &parsed_backtrace_len, parsed_backtrace_js);
      write_to_stream(ts, parsed_backtrace, parsed_backtrace_len);
      write_const_to_stream(ts, "\n");
      JS_FreeCString(ts->ctx, parsed_backtrace);
    }

    JS_FreeValue(ts->ctx, parsed_backtrace_js);
    JS_FreeValue(ts->ctx, backtrace_str);
    JS_FreeValue(ts->ctx, parseBacktrace);
    JS_FreeValue(ts->ctx, exception);
    JS_FreeValue(ts->ctx, parsed_arg);
    JS_FreeValue(ts->ctx, ret);
    return ts->stream_io_err;
  }

  JSValue stringified =
      JS_JSONStringify(ts->ctx, ret, JS_UNDEFINED, JS_UNDEFINED);
  if (JS_IsException(stringified)) {
    JS_FreeValue(ts->ctx, parsed_arg);
    JS_FreeValue(ts->ctx, ret);
    JS_FreeValue(ts->ctx, stringified);
    dump_error(ts->ctx);
    mutex_unlock(&ts->doing_js_stuff_mutex);
    write_to_stream(ts, ts->current_uuid, ts->current_uuid_len);
    write_const_to_stream(
        ts, " exception \"error attempting to JSON serialize return value\"\n");
    return ts->stream_io_err;
  }

  if (JS_IsUndefined(stringified)) {
    JS_FreeValue(ts->ctx, stringified);
    JS_FreeValue(ts->ctx, parsed_arg);
    JS_FreeValue(ts->ctx, ret);
    write_to_stream(ts, ts->current_uuid, ts->current_uuid_len);
    write_const_to_stream(ts, " exception \"unserializable return value\"\n");
    mutex_unlock(&ts->doing_js_stuff_mutex);
    return ts->stream_io_err;
  }

  size_t sz;
  const char *str = JS_ToCStringLen(ts->ctx, &sz, stringified);

  write_to_stream(ts, ts->current_uuid, ts->current_uuid_len);
  write_const_to_stream(ts, " ");
  write_to_stream(ts, str, sz);
  write_const_to_stream(ts, "\n");

  JS_FreeValue(ts->ctx, parsed_arg);
  JS_FreeValue(ts->ctx, ret);

  // Freeing twice because we create two refs via JS_ToCStringLen.
  // (`JS_FreeValue` is a refcount decrement.)
  JS_FreeValue(ts->ctx, stringified);
  JS_FreeValue(ts->ctx, stringified);

  if (0 == (ts->memory_check_count =
                ((ts->memory_check_count + 1) % MEMORY_CHECK_INTERVAL))) {
    JSMemoryUsage mu;
    JS_ComputeMemoryUsage(ts->rt, &mu);
    debug_logf("Memory usage memory_used_size=%" PRId64 "\n",
               mu.memory_used_size);
    if (mu.memory_used_size > ts->last_memory_usage) {
      ts->last_memory_usage = mu.memory_used_size;
      ts->memory_increase_count++;
      if (ts->memory_increase_count > MEMORY_INCREASE_MAX_COUNT) {
        release_logf("Memory usage has increased over the last %i commands. "
                     "Resetting interpreter state.\n",
                     MEMORY_INCREASE_MAX_COUNT * MEMORY_CHECK_INTERVAL);
        cleanup_js_runtime(ts);
        debug_log("Runtime cleaned up.\n");
        if (0 != init_thread_state(ts, NULL)) {
          release_log(
              "Error re-initializing JS runtime after memory increase\n");
          mutex_unlock(&ts->doing_js_stuff_mutex);
          return -1;
        }
        debug_log("Thread state reinitialized.\n");
      }
    } else {
      ts->memory_increase_count = 0;
    }
  }

  mutex_unlock(&ts->doing_js_stuff_mutex);

  return ts->stream_io_err;
}

static int line_handler(const char *line, size_t len, void *data,
                        bool truncated) {
  ThreadState *ts = (ThreadState *)data;
  debug_logf("LINE %i on %s: %s\n", ts->line_n, ts->unix_socket_filename, line);

  if (!truncated && !strcmp("?reset", line)) {
    if (!JS_IsUndefined(ts->compiled_query)) {
      JS_FreeValue(ts->ctx, ts->compiled_query);
      ts->compiled_query = JS_UNDEFINED;
    }
    ts->line_n = 0;
    ts->truncated = false;
    write_const_to_stream(ts, "reset\n");
    return 0;
  }

  // Allow the truncation logic to be triggered by a special '?truncated' line
  // in debug builds so that we don't have to generate large inputs in the
  // Valgrind tests.
  if (truncated || (CMAKE_BUILD_TYPE_IS_DEBUG && !strcmp(line, "?truncated")))
    ts->truncated = true;

  if (ts->truncated) {
    if (ts->line_n == 2) {
      ts->truncated = false;
      ts->line_n = 0;
      if (!JS_IsUndefined(ts->compiled_query)) {
        JS_FreeValue(ts->ctx, ts->compiled_query);
        ts->compiled_query = JS_UNDEFINED;
      }
      write_to_stream(ts, ts->current_uuid, ts->current_uuid_len);
      write_const_to_stream(ts, " exception \"js_server command was too long "
                                "and had to be truncated\"\n");
    } else {
      ts->line_n++;
    }
    return 0;
  }

  if (!strcmp("?quit", line)) {
    if (!JS_IsUndefined(ts->compiled_query)) {
      JS_FreeValue(ts->ctx, ts->compiled_query);
      ts->compiled_query = JS_UNDEFINED;
    }
    atomic_store(&g_interrupted_or_error, true);
    write_const_to_stream(ts, "quit\n");
    return EXIT_ON_QUIT_COMMAND;
  }
  if (line[0] == '?') {
    write_const_to_stream(ts, "bad command\n");
    return 0;
  }

  switch (ts->line_n) {
  case 0:
    return handle_line_1_message_uid(ts, line, len);
  case 1:
    return handle_line_2_query(ts, line, len);
  case 2:
    return handle_line_3_parameter(ts, line, len);
  default: {
    assert(ts->line_n >= 0 && ts->line_n <= 2);
    return -1; // won't get here, but avoids compiler warning
  }
  }
}

static void *listen_thread_func(void *data) {
  ThreadState *ts = (ThreadState *)data;
  JS_UpdateStackTop(ts->rt);
  listen_on_unix_socket(ts->unix_socket_filename, line_handler, (void *)ts);
  return NULL;
}

static pthread_t g_threads[MAX_THREADS];
static ThreadState g_thread_states[MAX_THREADS];

static const uint8_t *load_module_bytecode(const char *filename,
                                           size_t *out_size) {
  const uint8_t *module_bytecode = mmap_file(filename, out_size);
  if (!module_bytecode)
    return NULL;
  if (*out_size < ED25519_SIGNATURE_SIZE + 1) {
    release_logf("Module bytecode file is only %zu bytes. Too small!",
                 *out_size);
    munmap_or_warn((void *)module_bytecode, *out_size);
    return NULL;
  }

  const char *pubkey = getenv("JSOCKD_BYTECODE_MODULE_PUBLIC_KEY");
  if (!pubkey)
    pubkey = "";

  // Not documented because we allow this in Debug builds only, and binaries
  // uploaded to the GitHub release are Release builds.
  if (CMAKE_BUILD_TYPE_IS_DEBUG &&
      !strcmp(pubkey, "dangerously_allow_invalid_signatures")) {
    *out_size = *out_size - ED25519_SIGNATURE_SIZE;
    return module_bytecode;
  }

  uint8_t pubkey_bytes[ED25519_PUBLIC_KEY_SIZE];
  size_t decoded_size = hex_decode(
      pubkey_bytes, sizeof(pubkey_bytes) / sizeof(pubkey_bytes[0]), pubkey);
  if (decoded_size != ED25519_PUBLIC_KEY_SIZE) {
    release_logf("Error decoding public key hex from environment variable "
                 "JSOCKD_BYTECODE_MODULE_PUBLIC_KEY; decoded size=%zu\n",
                 decoded_size);
    munmap_or_warn((void *)module_bytecode, *out_size);
    return NULL;
  }
  if (!verify_bytecode(module_bytecode, *out_size, pubkey_bytes)) {
    release_logf("Error verifying bytecode module %s with public key %s\n",
                 filename, pubkey);
    munmap_or_warn((void *)module_bytecode, *out_size);
    return NULL;
  }

  *out_size = *out_size - ED25519_SIGNATURE_SIZE;

  return module_bytecode;
}

static void global_cleanup(void) {
  mutex_lock(&g_cached_functions_mutex);
  for (size_t i = 0;
       i < sizeof(g_cached_functions) / sizeof(g_cached_functions[0]); ++i) {
    if (g_cached_functions[i].bytecode) {
      free((void *)g_cached_functions[i].bytecode);
    }
  }
  mutex_unlock(&g_cached_functions_mutex);

  // These can fail, but we're calling this when we're about to exit, so there
  // is no useful error handling to be done.
  pthread_mutex_destroy(&g_cached_functions_mutex);
  pthread_mutex_destroy(&g_log_mutex);
  wait_group_destroy(&g_thread_ready_wait_group);

  // if it's zero, we know mem was never mapped
  // (because mmap_file errors on empty files)
  if (g_module_bytecode_size != 0)
    munmap_or_warn((void *)g_module_bytecode,
                   g_module_bytecode_size + ED25519_SIGNATURE_SIZE);
  if (g_source_map_size != 0 && g_source_map)
    munmap_or_warn((void *)g_source_map, g_source_map_size);
}

static void SIGINT_handler(int sig) {
  atomic_store(&g_interrupted_or_error, true);

  // Using stdio inside an interrupt handler is not safe, but calls to write
  // are explicilty allowed.
  const char msg[] = "\nSIGINT received, cleaning up...\n";
  write_all(2, msg, sizeof(msg) - 1);

  // We received SIGINT while we were still in the middle of waiting for
  // threads to be ready. Give up on trying to do a proper teardown.
  if (wait_group_n_remaining(&g_thread_ready_wait_group) > 0)
    exit(1);

  for (int i = 0; i < atomic_load(&g_n_threads); ++i)
    mutex_lock(&g_thread_states[i].doing_js_stuff_mutex);

  for (int i = 0; i < atomic_load(&g_n_threads); ++i)
    pthread_join(g_threads[i], NULL); // can fail, but no useful error handling
                                      // to be done

  for (int i = 0; i < atomic_load(&g_n_threads); ++i)
    mutex_unlock(&g_thread_states[i].doing_js_stuff_mutex);

  if (atomic_load(&g_global_init_complete))
    global_cleanup();

  for (int i = 0; i < atomic_load(&g_n_threads); ++i)
    cleanup_thread_state(&g_thread_states[i]);

  exit(1);
}

#ifndef VERSION
#define VERSION unknown_version
#endif

int main(int argc, char *argv[]) {
  struct sigaction sa = {.sa_handler = SIGINT_handler};
  sigaction(SIGINT, &sa, NULL);

  mutex_init(&g_log_mutex);
  mutex_init(&g_cached_functions_mutex);

  if (0 != parse_cmd_args(argc, argv, release_logf, &g_cmd_args)) {
    pthread_mutex_destroy(&g_log_mutex);
    pthread_mutex_destroy(&g_cached_functions_mutex);
    return 1;
  }

  if (g_cmd_args.version) {
    printf("JSockD js_server %s", STRINGIFY(VERSION));
    pthread_mutex_destroy(&g_log_mutex);
    pthread_mutex_destroy(&g_cached_functions_mutex);
    return 0;
  }

  if (g_cmd_args.es6_module_bytecode_file) {
    g_module_bytecode = load_module_bytecode(
        g_cmd_args.es6_module_bytecode_file, &g_module_bytecode_size);
    if (g_module_bytecode == NULL) {
      release_logf("Error loading module bytecode from %s\n",
                   g_cmd_args.es6_module_bytecode_file);
      pthread_mutex_destroy(&g_log_mutex);
      pthread_mutex_destroy(&g_cached_functions_mutex);
      return 1;
    }
  }

  if (g_cmd_args.source_map_file) {
    g_source_map = mmap_file(g_cmd_args.source_map_file, &g_source_map_size);
    if (!g_source_map)
      release_logf(
          "Error loading source map file %s; continuing without source map\n");
  }

  int n_threads = MIN(g_cmd_args.n_sockets, MAX_THREADS);
  atomic_store(&g_n_threads, g_cmd_args.n_sockets);

  if (0 != wait_group_init(&g_thread_ready_wait_group, n_threads)) {
    release_logf("Error initializing wait group: %s", strerror(errno));
    if (g_module_bytecode)
      munmap_or_warn((void *)g_module_bytecode,
                     g_module_bytecode_size + ED25519_SIGNATURE_SIZE);
    pthread_mutex_destroy(&g_log_mutex);
    pthread_mutex_destroy(&g_cached_functions_mutex);
    return 1;
  }
  atomic_init(&g_global_init_complete, true);

  for (int n = 0; n < n_threads; ++n) {
    debug_logf("Creating thread %i\n", n);
    if (0 !=
        init_thread_state(&g_thread_states[n], g_cmd_args.socket_path[n])) {
      release_logf("Error initializing thread %i", n);
      if (g_module_bytecode)
        munmap_or_warn((void *)g_module_bytecode, g_module_bytecode_size);
      pthread_mutex_destroy(&g_log_mutex);
      pthread_mutex_destroy(&g_cached_functions_mutex);
      for (int i = n - 1; i >= 0; --i)
        cleanup_thread_state(&g_thread_states[i]);
      wait_group_destroy(&g_thread_ready_wait_group);
      return 1;
    }
    pthread_create(&g_threads[n], NULL, listen_thread_func,
                   &g_thread_states[n]);
  }

  // Wait for all threads to be ready
  if (0 != wait_group_timed_wait(&g_thread_ready_wait_group,
                                 10000000000 /* 10 sec in ns */)) {
    release_logf(
        "Error waiting for threads to be ready, or timeout; n_remaining=%i\n",
        wait_group_n_remaining(&g_thread_ready_wait_group));
    global_cleanup();
    return 1;
  }

  printf("READY %i\n", n_threads);
  fflush(stdout);

  // pthread_join can fail, but we can't do any useful error handling
  for (int i = 0; i < atomic_load(&g_n_threads); ++i)
    pthread_join(g_threads[i], NULL);

  debug_log("All threads joined\n");

  global_cleanup();

  for (int i = 0; i < atomic_load(&g_n_threads); ++i)
    cleanup_thread_state(&g_thread_states[i]);

  for (int i = 0; i < atomic_load(&g_n_threads); ++i) {
    if (g_thread_states[i].exit_status != 0)
      return 1;
  }

  return 0;
}

// supress the annoying warning that inttypes.h is not used directly
static const char *supress_unused_header_warning UNUSED = PRId64;
