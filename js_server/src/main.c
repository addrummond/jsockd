#ifndef _REENTRANT
#define _REENTRANT
#endif

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

extern const uint32_t g_backtrace_module_bytecode_size;
extern const uint8_t g_backtrace_module_bytecode[];

extern const uint32_t g_shims_module_bytecode_size;
extern const uint8_t g_shims_module_bytecode[];

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

typedef struct {
  const uint8_t *bytecode;
  size_t bytecode_size;
} cached_function_t;

static HashCacheBucket g_cached_function_buckets[CACHED_FUNCTIONS_N_BUCKETS];
static cached_function_t g_cached_functions[CACHED_FUNCTIONS_N_BUCKETS];
static atomic_int g_n_cached_functions;

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
  } else {
    atomic_fetch_add_explicit(&g_n_cached_functions, 1, memory_order_relaxed);
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

// values for ThreadState.replacement_thread_state
enum {
  REPLACEMENT_THREAD_STATE_NONE,
  REPLACEMENT_THREAD_STATE_INIT,
  REPLACEMENT_THREAD_STATE_INIT_COMPLETE,
  REPLACEMENT_THREAD_STATE_CLEANUP
};

typedef struct {
  const char *unix_socket_filename;
  int sockfd;
  int streamfd;
  int stream_io_err;
} SocketState;

static void init_socket_state(SocketState *ss,
                              const char *unix_socket_filename) {
  ss->unix_socket_filename = unix_socket_filename;
  ss->sockfd = -1;
  ss->streamfd = -1;
  ss->stream_io_err = 0;
}

static void cleanup_socket_state(SocketState *socket_state) {
  // Don't add logs to this function as it may be called from a signal
  // handler.

  // We're about to exit, so we don't need to check for errors.
  if (socket_state->streamfd != -1)
    close(socket_state->streamfd);
  if (socket_state->sockfd != -1)
    close(socket_state->sockfd);
  unlink(socket_state->unix_socket_filename);
}

// The state for each thread which runs a QuickJS VM.
typedef struct ThreadState {
  SocketState *socket_state;
  JSRuntime *rt;
  JSContext *ctx;
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
  int last_n_cached_functions;
  bool truncated;
  JSValue sourcemap_str;
  struct ThreadState *my_replacement;
  atomic_int replacement_thread_state;
  pthread_t replacement_thread;
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

  ts->socket_state->sockfd = socket(AF_UNIX, SOCK_STREAM, 0);
  if (ts->socket_state->sockfd < 0) {
    ts->exit_status = -1;
    goto error;
  }

  if (0 != socket_fchmod(ts->socket_state->sockfd, 0600)) {
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
  if (0 !=
      bind(ts->socket_state->sockfd, (struct sockaddr *)&addr, sizeof(addr))) {
    ts->exit_status = -1;
    goto error;
  }
  if (0 != listen(ts->socket_state->sockfd, SOMAXCONN)) {
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

  ts->socket_state->streamfd = -1;
  for (;;) {
  accept_loop:
    switch (poll_fd(ts->socket_state->sockfd)) {
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
    ts->socket_state->streamfd = accept(
        ts->socket_state->sockfd, (struct sockaddr *)&addr, &streamfd_size);
    debug_log("Accepted on ts->socket\n");
    if (ts->socket_state->streamfd < 0) {
      ts->exit_status = -1;
      goto error_no_inc;
    }
    break;
  }

  for (;;) {
  read_loop:
    switch (poll_fd(ts->socket_state->streamfd)) {
    case READY:
      break;
    case GO_AROUND:
      goto read_loop;
    case SIG_INTERRUPT_OR_ERROR: {
      ts->exit_status = -1;
      goto error_no_inc;
    } break;
    }

    // This is cheap, and we need to reset it if we've reset the ThreadState, so
    // just call it on every loop.
    JS_UpdateStackTop(ts->rt);

    int exit_value =
        line_buf_read(&line_buf, g_cmd_args.socket_sep_char, lb_read,
                      &ts->socket_state->streamfd, line_handler, data);
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
  if (ts->socket_state->streamfd >= 0)
    close(ts->socket_state->streamfd);
  if (ts->socket_state->sockfd >= 0)
    close(ts->socket_state->sockfd);
  // indicate they're closed so we don't try to close them again in the main
  // teardown
  ts->socket_state->streamfd = -1;
  ts->socket_state->sockfd = -1;

  free(line_buf_buffer);

  atomic_store_explicit(&g_interrupted_or_error, true, memory_order_relaxed);
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
    if (0 != clock_gettime(CLOCK_MONOTONIC_RAW, &now)) {
      release_log("Error getting time in interrupt handler\n");
      return 0;
    }
    int64_t delta_ns = ns_time_diff(&now, start);
    if (delta_ns > 0 &&
        (uint64_t)delta_ns > g_cmd_args.max_command_runtime_us * 1000ULL) {
      release_logf("Command runtime of %" PRIu64 "us exceeded %" PRIu64
                   "us, interrupting\n",
                   delta_ns / 1000ULL, g_cmd_args.max_command_runtime_us);
      return 1;
    }
  }
  return (int)atomic_load_explicit(&g_interrupted_or_error,
                                   memory_order_relaxed);
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

typedef enum { BACKTRACE_JSON, BACKTRACE_PRETTY } BacktraceFormat;

static const char *get_backtrace(ThreadState *ts, const char *backtrace,
                                 size_t backtrace_length,
                                 size_t *out_json_backtrace_length,
                                 BacktraceFormat backtrace_format) {
  const char *bt_func_name =
      backtrace_format == BACKTRACE_JSON ? "parseBacktrace" : "formatBacktrace";
  JSValue bt_func =
      JS_GetPropertyStr(ts->ctx, ts->backtrace_module, bt_func_name);
  if (!JS_IsFunction(ts->ctx, bt_func)) {
    JS_FreeValue(ts->ctx, bt_func);
    release_logf("Internal error: %s is not a function\n", bt_func_name);
    return NULL;
  }
  if (JS_IsUndefined(ts->sourcemap_str)) {
    ts->sourcemap_str =
        g_source_map_size == 0
            ? JS_UNDEFINED
            : JS_NewStringLen(ts->ctx, (const char *)g_source_map,
                              g_source_map_size);
    int c = atomic_fetch_add_explicit(&g_source_map_load_count, 1,
                                      memory_order_relaxed);
    if (c + 1 == g_n_threads && g_source_map_size != 0 && g_source_map) {
      debug_log("All threads have loaded the sourcemap, calling munmap...\n");
      munmap_or_warn((void *)g_source_map, g_source_map_size);
      g_source_map = NULL;
    }
  }
  JSValue backtrace_str = JS_NewStringLen(ts->ctx, backtrace, backtrace_length);
  JSValue argv[] = {ts->sourcemap_str, backtrace_str};
  JSValue parsed_backtrace_js = JS_Call(ts->ctx, bt_func, JS_UNDEFINED,
                                        sizeof(argv) / sizeof(argv[0]), argv);

  const char *bt_str;
  if (JS_IsException(parsed_backtrace_js)) {
    release_log("Error parsing backtrace:\n");
    dump_error(ts->ctx);
    release_logf("The backtrace that could not be parsed:\n%.*s",
                 backtrace_length, backtrace);
    bt_str = NULL;
  } else {
    bt_str = JS_ToCStringLen(ts->ctx, out_json_backtrace_length,
                             parsed_backtrace_js);
  }

  JS_FreeValue(ts->ctx, parsed_backtrace_js);
  JS_FreeValue(ts->ctx, backtrace_str);
  JS_FreeValue(ts->ctx, bt_func);

  return bt_str;
}

static int init_thread_state(ThreadState *ts, SocketState *socket_state) {
  ts->rt = JS_NewRuntime();
  if (!ts->rt) {
    release_log("Failed to create JS runtime\n");
    return -1;
  }

  js_std_set_worker_new_context_func(JS_NewCustomContext);
  js_std_init_handlers(ts->rt);
  ts->ctx = JS_NewCustomContext(ts->rt);
  if (!ts->ctx) {
    release_log("Failed to create JS context\n");
    JS_FreeRuntime(ts->rt);
    return -1;
  }

  JS_SetModuleLoaderFunc2(ts->rt, NULL, jsockd_js_module_loader,
                          js_module_check_attributes, NULL);

  JSValue shims_module = load_binary_module(ts->ctx, g_shims_module_bytecode,
                                            g_shims_module_bytecode_size);
  assert(!JS_IsException(shims_module));
  JS_FreeValue(ts->ctx, shims_module); // imported just for side effects

  ts->backtrace_module = load_binary_module(
      ts->ctx, g_backtrace_module_bytecode, g_backtrace_module_bytecode_size);
  assert(!JS_IsException(ts->backtrace_module));

  ts->sourcemap_str = JS_UNDEFINED;

  // Load the precompiled module.
  if (g_module_bytecode)
    ts->compiled_module =
        load_binary_module(ts->ctx, g_module_bytecode, g_module_bytecode_size);
  else
    ts->compiled_module = JS_UNDEFINED;
  if (JS_IsException(ts->compiled_module)) {
    release_log("Failed to load precompiled module\n");
    char *error_msg_buf = calloc(ERROR_MSG_MAX_BYTES, sizeof(char));
    WBuf emb = {
        .buf = error_msg_buf, .index = 0, .length = ERROR_MSG_MAX_BYTES};
    JS_PrintValue(ts->ctx, write_to_buf, &emb.buf, JS_GetException(ts->ctx),
                  NULL);
    size_t bt_length;
    const char *bt_str =
        get_backtrace(ts, emb.buf, emb.index, &bt_length, BACKTRACE_PRETTY);
    if (!bt_str) {
      release_logf("<no backtrace available>\n");
    } else {
      release_logf("%.*s\n", bt_length, bt_str);
      JS_FreeCString(ts->ctx, bt_str);
    }

    JS_FreeValue(ts->ctx, ts->compiled_module);
    free(error_msg_buf);

    // This return value will eventually lead to stuff getting
    // cleaned up by cleanup_js_runtime
    return -1;
  }

  mutex_init(&ts->doing_js_stuff_mutex);

  JS_SetInterruptHandler(ts->rt, interrupt_handler, ts);

  ts->socket_state = socket_state;
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
  ts->last_n_cached_functions = 1;
  ts->my_replacement = NULL;
  atomic_init(&ts->replacement_thread_state, REPLACEMENT_THREAD_STATE_NONE);

  return 0;
}

static void cleanup_thread_state(ThreadState *ts) {
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

  // This could fail, but no useful error handling to be done (we're exiting
  // anyway).
  pthread_mutex_destroy(&ts->doing_js_stuff_mutex);
}

static void destroy_thread_state(ThreadState *ts) {
  // Don't add logs to this function as it may be called from a signal
  // handler.

  cleanup_socket_state(ts->socket_state);

  cleanup_thread_state(ts);

  int rts =
      atomic_load_explicit(&ts->replacement_thread_state, memory_order_relaxed);
  if (ts->my_replacement && (rts == REPLACEMENT_THREAD_STATE_INIT_COMPLETE ||
                             rts == REPLACEMENT_THREAD_STATE_CLEANUP)) {
    free(ts->my_replacement);
    ts->my_replacement = NULL;
  }
}

static void write_to_stream(ThreadState *ts, const char *buf, size_t len) {
  if (0 != write_all(ts->socket_state->streamfd, buf, len)) {
    ts->socket_state->stream_io_err = -1;
    release_logf("Error writing to socket: %s\n", strerror(errno));
    return;
  }
}

#define write_const_to_stream(ts, str)                                         \
  write_to_stream((ts), (str), sizeof(str) - 1)

static void *reset_thread_state_cleanup_old_runtime_thread(void *data) {
  ThreadState *ts = (ThreadState *)data;
  cleanup_thread_state(ts->my_replacement);
  if (ts->my_replacement) {
    free(ts->my_replacement);
    ts->my_replacement = NULL;
  }
  atomic_store_explicit(&ts->replacement_thread_state,
                        REPLACEMENT_THREAD_STATE_NONE, memory_order_relaxed);
  return NULL;
}

static int handle_line_1_message_uid(ThreadState *ts, const char *line,
                                     int len) {
  if (len > MESSAGE_UUID_MAX_BYTES) {
    debug_logf("Error: message UUID has length %i and will be truncated to "
               "first %i bytes\n",
               len, MESSAGE_UUID_MAX_BYTES);
    len = MESSAGE_UUID_MAX_BYTES;
  }

  // Check to see if the thread state has been reinitialized (following a memory
  // increase).
  if (REPLACEMENT_THREAD_STATE_INIT_COMPLETE ==
      atomic_load_explicit(&ts->replacement_thread_state,
                           memory_order_relaxed)) {
    ThreadState *r = ts->my_replacement;
    memswap_small(ts->my_replacement, ts, sizeof(*ts));
    if (0 != pthread_create(&ts->replacement_thread, NULL,
                            reset_thread_state_cleanup_old_runtime_thread, r)) {
      release_logf("pthread_create failed: %s\n", strerror(errno));
      return -1;
    }
    atomic_store_explicit(&ts->replacement_thread_state,
                          REPLACEMENT_THREAD_STATE_CLEANUP,
                          memory_order_relaxed);
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

static int64_t memusage(const JSMemoryUsage *m) {
  return m->malloc_count + m->memory_used_count + m->atom_count + m->str_count +
         m->obj_count + m->prop_count + m->shape_count + m->js_func_count +
         m->js_func_pc2line_count + m->c_func_count + m->fast_array_count +
         m->binary_object_count;
}

static void *reset_thread_state_thread(void *data) {
  ThreadState *ts = (ThreadState *)data;
  ts->my_replacement = (ThreadState *)malloc(sizeof(ThreadState));
  init_thread_state((ThreadState *)ts->my_replacement, ts->socket_state);
  atomic_store_explicit(&ts->replacement_thread_state,
                        REPLACEMENT_THREAD_STATE_INIT_COMPLETE,
                        memory_order_relaxed);
  return NULL;
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
    return ts->socket_state->stream_io_err;
  }

  mutex_lock(&ts->doing_js_stuff_mutex);
  if (0 != clock_gettime(CLOCK_MONOTONIC_RAW, &ts->last_js_execution_start)) {
    mutex_lock(&ts->doing_js_stuff_mutex);
    JS_FreeValue(ts->ctx, ts->compiled_query);
    mutex_unlock(&ts->doing_js_stuff_mutex);
    release_logf("Error getting time in handle_line_3_parameter [1]\n");
    return -1;
  }

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
    return ts->socket_state->stream_io_err;
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
    char *error_msg_buf = calloc(ERROR_MSG_MAX_BYTES, sizeof(char));
    WBuf emb = {
        .buf = error_msg_buf, .index = 0, .length = ERROR_MSG_MAX_BYTES};
    JSValue exception = JS_GetException(ts->ctx);
    JS_PrintValue(ts->ctx, write_to_buf, &emb, exception, NULL);

    size_t json_bt_length;
    const char *json_bt_str =
        get_backtrace(ts, emb.buf, emb.index, &json_bt_length, BACKTRACE_JSON);
    if (!json_bt_str) {
      write_const_to_stream(ts, "{}\n");
    } else {
      write_to_stream(ts, json_bt_str, json_bt_length);
      write_const_to_stream(ts, "\n");
      JS_FreeCString(ts->ctx, json_bt_str);
    }

    JS_FreeValue(ts->ctx, exception);
    JS_FreeValue(ts->ctx, parsed_arg);
    JS_FreeValue(ts->ctx, ret);

    free(error_msg_buf);

    return ts->socket_state->stream_io_err;
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
    return ts->socket_state->stream_io_err;
  }

  if (JS_IsUndefined(stringified)) {
    JS_FreeValue(ts->ctx, stringified);
    JS_FreeValue(ts->ctx, parsed_arg);
    JS_FreeValue(ts->ctx, ret);
    write_to_stream(ts, ts->current_uuid, ts->current_uuid_len);
    write_const_to_stream(ts, " exception \"unserializable return value\"\n");
    mutex_unlock(&ts->doing_js_stuff_mutex);
    return ts->socket_state->stream_io_err;
  }

  struct timespec now;
  if (0 != clock_gettime(CLOCK_MONOTONIC_RAW, &now)) {
    JS_FreeValue(ts->ctx, parsed_arg);
    JS_FreeValue(ts->ctx, ret);
    JS_FreeValue(ts->ctx, stringified);
    mutex_unlock(&ts->doing_js_stuff_mutex);
    release_logf("Error getting time in handle_line_3_parameter [2]\n");
    return -1;
  }

  int64_t exec_time_ns = ns_time_diff(&now, &ts->last_js_execution_start);
  char exec_time_buf[21]; // 20 digits for int64_t + 1 for zeroterm
  int exec_time_len =
      snprintf(exec_time_buf, sizeof(exec_time_buf), "%" PRId64, exec_time_ns);

  size_t sz;
  const char *str = JS_ToCStringLen(ts->ctx, &sz, stringified);

  write_to_stream(ts, ts->current_uuid, ts->current_uuid_len);
  write_const_to_stream(ts, " ");
  write_to_stream(ts, exec_time_buf,
                  MIN(exec_time_len, (int)(sizeof(exec_time_buf) - 1)));
  write_const_to_stream(ts, " ");
  write_to_stream(ts, str, sz);
  write_const_to_stream(ts, "\n");

  JS_FreeValue(ts->ctx, parsed_arg);
  JS_FreeValue(ts->ctx, ret);
  JS_FreeCString(ts->ctx, str);
  JS_FreeValue(ts->ctx, stringified);

  if (0 == (ts->memory_check_count =
                ((ts->memory_check_count + 1) % MEMORY_CHECK_INTERVAL))) {
    JSMemoryUsage mu;
    JS_ComputeMemoryUsage(ts->rt, &mu);
    debug_logf("Memory usage memory_used_size=%" PRId64 "\n",
               mu.memory_used_size);
    int64_t current_usage = memusage(&mu);
    if (atomic_load_explicit(&g_n_cached_functions, memory_order_relaxed) <=
            ts->last_n_cached_functions &&
        current_usage > ts->last_memory_usage) {
      ts->last_memory_usage = current_usage;
      ts->last_n_cached_functions =
          atomic_load_explicit(&g_n_cached_functions, memory_order_relaxed);
      ts->memory_increase_count++;
      if (ts->memory_increase_count > MEMORY_INCREASE_MAX_COUNT) {
        release_logf("Memory usage has increased over the last %i commands. "
                     "Resetting interpreter state.\n",
                     MEMORY_INCREASE_MAX_COUNT * MEMORY_CHECK_INTERVAL);
        // To avoid latency, we do the following:
        //   (i)   create a new thread state in a background thread,
        //   (ii)  swap the old and new thread states the next time we're in
        //         the line_1 handler and the new thread state has finished
        //         initializing, and then
        //   (iii) clean up the old thread state in a background thread.
        if (0 != pthread_create(&ts->replacement_thread, NULL,
                                reset_thread_state_thread, (void *)ts)) {
          release_logf("pthread_create failed: %s\n", strerror(errno));
          mutex_unlock(&ts->doing_js_stuff_mutex);
          return -1;
        }
        atomic_store_explicit(&ts->replacement_thread_state,
                              REPLACEMENT_THREAD_STATE_INIT,
                              memory_order_relaxed);
      }
    } else {
      ts->memory_increase_count = 0;
    }
  }

  mutex_unlock(&ts->doing_js_stuff_mutex);

  return ts->socket_state->stream_io_err;
}

static int line_handler(const char *line, size_t len, void *data,
                        bool truncated) {
  ThreadState *ts = (ThreadState *)data;
  debug_logf("LINE %i on %s: %s\n", ts->line_n,
             ts->socket_state->unix_socket_filename, line);

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
      // we'll signal an error once the client has sent the third line
      ts->line_n++;
    }
    return 0;
  }

  if (!strcmp("?quit", line)) {
    if (!JS_IsUndefined(ts->compiled_query)) {
      JS_FreeValue(ts->ctx, ts->compiled_query);
      ts->compiled_query = JS_UNDEFINED;
    }
    atomic_store_explicit(&g_interrupted_or_error, true, memory_order_relaxed);
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
  listen_on_unix_socket(ts->socket_state->unix_socket_filename, line_handler,
                        (void *)ts);
  return NULL;
}

static pthread_t g_threads[MAX_THREADS];
static SocketState g_socket_states[MAX_THREADS];
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

  if (g_module_bytecode_size != 0 && g_module_bytecode)
    munmap_or_warn((void *)g_module_bytecode,
                   g_module_bytecode_size + ED25519_SIGNATURE_SIZE);
  if (g_source_map_size != 0 && g_source_map)
    munmap_or_warn((void *)g_source_map, g_source_map_size);
}

static void SIGINT_handler(int sig) {
  atomic_store_explicit(&g_interrupted_or_error, true, memory_order_relaxed);

  // Using stdio inside an interrupt handler is not safe, but calls to write
  // are explicilty allowed.
  const char msg[] = "\nSIGINT received, cleaning up...\n";
  write_all(2, msg, sizeof(msg) - 1);

  // We received SIGINT while we were still in the middle of waiting for
  // threads to be ready. Give up on trying to do a proper teardown.
  if (wait_group_n_remaining(&g_thread_ready_wait_group) > 0)
    exit(1);

  for (int i = 0; i < atomic_load_explicit(&g_n_threads, memory_order_relaxed);
       ++i)
    mutex_lock(&g_thread_states[i].doing_js_stuff_mutex);

  for (int i = 0; i < atomic_load_explicit(&g_n_threads, memory_order_relaxed);
       ++i)
    pthread_join(g_threads[i], NULL); // can fail, but no useful error handling
                                      // to be done

  for (int i = 0; i < atomic_load_explicit(&g_n_threads, memory_order_relaxed);
       ++i)
    mutex_unlock(&g_thread_states[i].doing_js_stuff_mutex);

  if (atomic_load_explicit(&g_global_init_complete, memory_order_relaxed))
    global_cleanup();

  for (int i = 0; i < atomic_load_explicit(&g_n_threads, memory_order_relaxed);
       ++i)
    destroy_thread_state(&g_thread_states[i]);

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
      release_logf("Error loading module bytecode from %s: %s\n",
                   g_cmd_args.es6_module_bytecode_file, strerror(errno));
      pthread_mutex_destroy(&g_log_mutex);
      pthread_mutex_destroy(&g_cached_functions_mutex);
      return 1;
    }
  }

  if (g_cmd_args.source_map_file) {
    g_source_map = mmap_file(g_cmd_args.source_map_file, &g_source_map_size);
    if (!g_source_map) {
      release_logf("Error loading source map file %s: %s\n", strerror(errno));
      release_log("Continuing without source map\n");
    }
  }

  int n_threads = MIN(g_cmd_args.n_sockets, MAX_THREADS);
  atomic_store_explicit(&g_n_threads, g_cmd_args.n_sockets,
                        memory_order_relaxed);

  if (0 != wait_group_init(&g_thread_ready_wait_group, n_threads)) {
    release_logf("Error initializing wait group: %s\n", strerror(errno));
    if (g_module_bytecode_size != 0 && g_module_bytecode)
      munmap_or_warn((void *)g_module_bytecode,
                     g_module_bytecode_size + ED25519_SIGNATURE_SIZE);
    pthread_mutex_destroy(&g_log_mutex);
    pthread_mutex_destroy(&g_cached_functions_mutex);
    return 1;
  }
  atomic_init(&g_global_init_complete, true);

  for (int n = 0; n < n_threads; ++n) {
    debug_logf("Creating thread %i\n", n);
    init_socket_state(&g_socket_states[n], g_cmd_args.socket_path[n]);
    if (0 != init_thread_state(&g_thread_states[n], &g_socket_states[n])) {
      release_logf("Error initializing thread %i\n", n);
      if (g_module_bytecode_size != 0 && g_module_bytecode)
        munmap_or_warn((void *)g_module_bytecode, g_module_bytecode_size);
      pthread_mutex_destroy(&g_log_mutex);
      pthread_mutex_destroy(&g_cached_functions_mutex);
      for (int i = n - 1; i >= 0; --i)
        destroy_thread_state(&g_thread_states[i]);
      wait_group_destroy(&g_thread_ready_wait_group);
      return 1;
    }
    if (0 != pthread_create(&g_threads[n], NULL, listen_thread_func,
                            &g_thread_states[n])) {
      release_logf("pthread_create failed; exiting: %s", strerror(errno));
      for (int i = 0; i <= n; ++i)
        destroy_thread_state(&g_thread_states[i]);
      global_cleanup();
      exit(1);
    }
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
  for (int i = 0; i < atomic_load_explicit(&g_n_threads, memory_order_relaxed);
       ++i) {
    pthread_join(g_threads[i], NULL);
    int rts = atomic_load_explicit(&g_thread_states[i].replacement_thread_state,
                                   memory_order_relaxed);
    if (rts == REPLACEMENT_THREAD_STATE_INIT ||
        rts == REPLACEMENT_THREAD_STATE_CLEANUP) {
      // As we've just joined the thread, we know it won't be concurrently
      // updating replacement_thread.
      pthread_join(g_thread_states[i].replacement_thread, NULL);
    }
  }

  debug_log("All threads joined\n");

  global_cleanup();

  for (int i = 0; i < atomic_load_explicit(&g_n_threads, memory_order_relaxed);
       ++i)
    destroy_thread_state(&g_thread_states[i]);

  for (int i = 0; i < atomic_load_explicit(&g_n_threads, memory_order_relaxed);
       ++i) {
    if (g_thread_states[i].exit_status != 0)
      return 1;
  }

  return 0;
}

// supress the annoying warning that inttypes.h is not used directly
static const char *supress_unused_header_warning UNUSED = PRId64;
