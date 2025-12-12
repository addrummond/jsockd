#ifndef _REENTRANT
#define _REENTRANT
#endif

#include "backtrace.h"
#include "cmdargs.h"
#include "config.h"
#include "console.h"
#include "custom_module_loader.h"
#include "fchmod.h"
#include "globals.h"
#include "hash_cache.h"
#include "hex.h"
#include "line_buf.h"
#include "log.h"
#include "mmap_file.h"
#include "modcompiler.h"
#include "quickjs-libc.h"
#include "quickjs.h"
#include "threadstate.h"
#include "utils.h"
#include "verify_bytecode.h"
#include "version.h"
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
#include <stdarg.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

// Testing scenarios with collisions is less labor intensive if we use a smaller
// number of bits in the debug build.
#define CACHED_FUNCTION_HASH_BITS                                              \
  (CMAKE_BUILD_TYPE_IS_DEBUG ? CACHED_FUNCTIONS_HASH_BITS_DEBUG                \
                             : CACHED_FUNCTIONS_HASH_BITS_RELEASE)
#define CACHED_FUNCTIONS_N_BUCKETS                                             \
  HASH_CACHE_BUCKET_ARRAY_SIZE_FROM_HASH_BITS(CACHED_FUNCTION_HASH_BITS)

static atomic_bool g_global_init_complete;

// 16-byte align may be required for use of native 128-bit atomic instructions.
_Alignas(16) static CachedFunctionBucket
    g_cached_function_buckets[CACHED_FUNCTIONS_N_BUCKETS];
static atomic_int g_n_cached_functions;

static void cleanup_unused_hash_cache_bucket(HashCacheBucket *b) {
  jsockd_logf(LOG_DEBUG, "Freeing bytecode %p\n",
              ((CachedFunctionBucket *)b)->payload.bytecode);
  free((void *)(((CachedFunctionBucket *)b)->payload.bytecode));
  ((CachedFunctionBucket *)b)->payload.bytecode = NULL;
}

static CachedFunctionBucket *add_cached_function(HashCacheUid uid,
                                                 const uint8_t *bytecode,
                                                 size_t bytecode_size) {
  assert(bytecode);

  CachedFunction to_add = {
      .bytecode = bytecode,
      .bytecode_size = bytecode_size,
  };
  CachedFunctionBucket *b =
      add_to_hash_cache(g_cached_function_buckets, CACHED_FUNCTION_HASH_BITS,
                        uid, &to_add, cleanup_unused_hash_cache_bucket);
  if (!b) {
    jsockd_log(LOG_INFO, "No empty slot for cached function\n");
    return NULL;
  }
  return b;
}

static CachedFunction *get_cached_function(HashCacheUid uid) {
  CachedFunctionBucket *b = get_hash_cache_entry(
      g_cached_function_buckets, CACHED_FUNCTION_HASH_BITS, uid);
  if (b)
    return &((CachedFunctionBucket *)b)->payload;
  return NULL;
}

static void init_socket_state(SocketState *ss,
                              const char *unix_socket_filename) {
  ss->unix_socket_filename = unix_socket_filename;
  ss->sockfd = -1;
  ss->streamfd = -1;
  ss->stream_io_err = 0;
  memset(&ss->addr, 0, sizeof(ss->addr));
}

static void cleanup_socket_state(SocketState *socket_state) {
  if (!socket_state)
    return;
  // We're about to exit, so we don't need to check for errors.
  if (socket_state->streamfd != -1)
    close(socket_state->streamfd);
  if (socket_state->sockfd != -1)
    close(socket_state->sockfd);
  unlink(socket_state->unix_socket_filename);
}

// In debug builds we keep track of how many new thread states we've created vs.
// how many we've cleaned up and freed. This can give useful additional context
// for Valgrind errors.
#ifdef CMAKE_BUILD_TYPE_DEBUG
static atomic_int g_new_thread_state_count;
#define debug_inc_new_thread_state_count()                                     \
  do {                                                                         \
    atomic_fetch_add_explicit(&g_new_thread_state_count, 1,                    \
                              memory_order_relaxed);                           \
    jsockd_logf(LOG_DEBUG, "g_new_thread_state_count incremented at %i\n",     \
                __LINE__);                                                     \
  } while (0)
#define debug_dec_new_thread_state_count()                                     \
  do {                                                                         \
    atomic_fetch_add_explicit(&g_new_thread_state_count, -1,                   \
                              memory_order_relaxed);                           \
    jsockd_logf(LOG_DEBUG, "g_new_thread_state_count decremented at %i\n",     \
                __LINE__);                                                     \
  } while (0)
#else
#define debug_inc_new_thread_state_count() 0
#define debug_dec_new_thread_state_count() 0
#endif

#ifdef CMAKE_BUILD_TYPE_DEBUG
#define manually_trigger_thread_state_reset(ts)                                \
  ((ts)->manually_trigger_thread_state_reset)
#else
#define manually_trigger_thread_state_reset(_ts) false
#endif

static void js_print_value_debug_write(void *opaque, const char *buf,
                                       size_t len) {
  LogLevel l = *(LogLevel *)opaque;
  jsockd_logf(l, "%.*s\n", len, buf);
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
static const int TRAMPOLINE = -9999;

static int initialize_and_listen_on_unix_socket(SocketState *socket_state) {
  socket_state->sockfd = socket(AF_UNIX, SOCK_STREAM, 0);
  if (socket_state->sockfd < 0) {
    jsockd_logf(LOG_ERROR, "Error creating socket %s: %s\n",
                socket_state->unix_socket_filename, strerror(errno));
    return -1;
  }

  if (0 != socket_fchmod(socket_state->sockfd, 0600)) {
    jsockd_logf(LOG_ERROR, "Error setting permissions 0600 on socket %s: %s\n",
                socket_state->unix_socket_filename, strerror(errno));
    return -1;
  }

  socket_state->addr.sun_family = AF_UNIX;
  if (sizeof(socket_state->addr.sun_path) <
      strlen(socket_state->unix_socket_filename) + 1 /* zeroterm */) {
    jsockd_logf(LOG_ERROR,
                "Error: UNIX socket filename %s is too long (UNIX limitation, "
                "not JSockD; max length on this system is %zu)\n",
                socket_state->unix_socket_filename,
                sizeof(socket_state->addr.sun_path) - 1);

    return -1;
  }

  strncpy(socket_state->addr.sun_path, socket_state->unix_socket_filename,
          sizeof(socket_state->addr.sun_path) - 1);
  if (-1 == unlink(socket_state->unix_socket_filename) && errno != ENOENT) {
    jsockd_logf(LOG_ERROR, "Error attempting to unlink %s\n",
                socket_state->unix_socket_filename);
    return -1;
  }

  if (0 != bind(socket_state->sockfd, (struct sockaddr *)&socket_state->addr,
                sizeof(socket_state->addr))) {
    jsockd_logf(LOG_ERROR, "Error binding socket %s: %s\n",
                socket_state->unix_socket_filename, strerror(errno));
    return -1;
  }
  if (0 != listen(socket_state->sockfd, SOMAXCONN)) {
    jsockd_logf(LOG_ERROR, "Error listening on socket %s: %s\n",
                socket_state->unix_socket_filename, strerror(errno));
    return -1;
  }

  // On Mac the call to socket_fchmod above is a no-op, so call chmod on the
  // UNIX socket filename. This is theoretically less good, because there's a
  // tiny window where the socket file exists but is not yet chmodded.
  if (0 != chmod(socket_state->unix_socket_filename, 0600)) {
    jsockd_logf(
        LOG_ERROR,
        "Error setting permissions 0600 on socket %s via filename: %s\n",
        socket_state->unix_socket_filename, strerror(errno));
    return -1;
  }

  return 0;
}

typedef struct {
  ThreadState *ts;
  int (*line_handler)(const char *line, size_t len, ThreadState *data,
                      bool truncated);
} CommandLoopLineHandler;

static int command_loop_line_handler_wrapper(const char *line, size_t len,
                                             void *data, bool truncated) {
  CommandLoopLineHandler *lh = (CommandLoopLineHandler *)data;
  return lh->line_handler(line, len, lh->ts, truncated);
}

static void command_loop(ThreadState *ts,
                         int (*line_handler)(const char *line, size_t len,
                                             ThreadState *data, bool truncated),
                         void (*tick_handler)(ThreadState *ts)) {
  CommandLoopLineHandler louslh = {.ts = ts, .line_handler = line_handler};

  if (0 != initialize_and_listen_on_unix_socket(ts->socket_state)) {
    jsockd_log(LOG_ERROR, "Error initializing UNIX socket\n");
    ts->exit_status = -1;
    goto error;
  }

  if (0 != wait_group_inc(&g_thread_ready_wait_group, 1)) {
    jsockd_log(LOG_ERROR, "Error incrementing thread ready wait group\n");
    ts->exit_status = -1;
    goto error_no_inc;
  }

  ts->socket_state->streamfd = -1;
  for (;;) {
  accept_loop:
    switch (poll_fd(ts->socket_state->sockfd, SOCKET_POLL_TIMEOUT_MS)) {
    case READY:
      break;
    case GO_AROUND:
      goto accept_loop;
    case SIG_INTERRUPT_OR_ERROR: {
      goto error_no_inc;
    } break;
    }

    socklen_t streamfd_size = sizeof(struct sockaddr);
    ts->socket_state->streamfd =
        accept(ts->socket_state->sockfd,
               (struct sockaddr *)&ts->socket_state->addr, &streamfd_size);
    if (ts->socket_state->streamfd < 0) {
      jsockd_logf(LOG_ERROR, "accept failed on UNIX socket: %s\n",
                  strerror(errno));
      ts->exit_status = -1;
      goto error_no_inc;
    }
    jsockd_logf(LOG_DEBUG, "Accepted on ts->socket thread %i\n",
                ts->thread_index);
    if (0 != clock_gettime(MONOTONIC_CLOCK, &ts->last_active_time)) {
      jsockd_logf(LOG_ERROR, "Error getting time after accept: %s",
                  strerror(errno));
      ts->exit_status = -1;
      goto error_no_inc;
    }
    break;
  }

  JS_UpdateStackTop(ts->rt);

  for (;;) {
  read_loop:
    tick_handler(ts);

    switch (poll_fd(ts->socket_state->streamfd, SOCKET_POLL_TIMEOUT_MS)) {
    case READY:
      break;
    case GO_AROUND:
      goto read_loop;
    case SIG_INTERRUPT_OR_ERROR:
      goto error_no_inc;
    }

    if (ts->rt == NULL) {
      jsockd_log(LOG_DEBUG, "Re-initializing shut down thread state\n");
      assert(REPLACEMENT_THREAD_STATE_NONE ==
             atomic_load_explicit(&ts->replacement_thread_state,
                                  memory_order_acquire));
      init_thread_state(ts, ts->socket_state, ts->thread_index);
      atomic_fetch_add_explicit(&g_n_ready_threads, 1, memory_order_relaxed);
      register_thread_state_runtime(ts->rt, ts);
    }

    LineBuf line_buf = {.buf = ts->input_buf, .size = INPUT_BUF_BYTES};
    int exit_value =
        line_buf_read(&line_buf, g_cmd_args.socket_sep_char, lb_read,
                      &ts->socket_state->streamfd,
                      command_loop_line_handler_wrapper, (void *)&louslh);
    while (exit_value == TRAMPOLINE) {
      JS_UpdateStackTop(ts->rt);
      exit_value =
          line_buf_replay(&line_buf, g_cmd_args.socket_sep_char,
                          command_loop_line_handler_wrapper, (void *)&louslh);
    }

    if (exit_value < 0 && exit_value != LINE_BUF_READ_EOF &&
        exit_value != EXIT_ON_QUIT_COMMAND)
      ts->exit_status = -1;
    if (exit_value < 0)
      goto error_no_inc; // EOF or error
  }

error:
  // Increment the wait group to indicate that this thread is ready, so that
  // all threads can be joined in main. The other threads will notice that
  // g_interrupted_or_error has been set to true, and thus exit gracefully
  // in due course.
  if (0 != wait_group_inc(&g_thread_ready_wait_group, 1))
    jsockd_log(
        LOG_ERROR,
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

  atomic_store_explicit(&g_interrupted_or_error, true, memory_order_release);
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
  jsockd_logf(LOG_DEBUG, "Compiled bytecode size: %zu\n", *bytecode_size);

  // We want to preserve the bytecode across runtime contexts, but js_free
  // requires a context for some refcounting. So copy this over to some
  // malloc'd memory.
  const uint8_t *malloc_bytecode = malloc(*bytecode_size);
  jsockd_logf(LOG_DEBUG, "Mallocing bytecode %p (size=%zu)\n", malloc_bytecode,
              *bytecode_size);
  memcpy((void *)malloc_bytecode, bytecode, *bytecode_size);
  js_free(ctx, (void *)bytecode);

  return (const uint8_t *)malloc_bytecode;
}

static JSValue func_from_bytecode(JSContext *ctx, const uint8_t *bytecode,
                                  int len) {
  JSValue val = JS_ReadObject(ctx, bytecode, len, JS_READ_OBJ_BYTECODE);
  if (JS_IsException(val)) {
    jsockd_log(LOG_DEBUG,
               "Exception returned when reading bytecode via JS_ReadObject\n");
    dump_error(ctx);
    return val;
  }
  JSValue evald = JS_EvalFunction(ctx, val); // this call frees val
  evald = js_std_await(ctx, evald);
  if (JS_VALUE_GET_TAG(evald) != JS_TAG_OBJECT) {
    jsockd_log(LOG_DEBUG, "JS_EvalFunction did not return an object\n");
    if (CMAKE_BUILD_TYPE_IS_DEBUG && JS_IsException(evald))
      dump_error(ctx);
    JS_FreeValue(ctx, evald);
    return JS_EXCEPTION;
  }
  JSValue r = JS_GetPropertyStr(ctx, evald, "value");
  JS_FreeValue(ctx, evald);
  return r;
}

static void cleanup_old_runtime(ThreadState *ts) {
  assert(ts->my_replacement);
  JS_UpdateStackTop(ts->my_replacement->rt);
  cleanup_thread_state(ts->my_replacement);
  free(ts->my_replacement);
  debug_dec_new_thread_state_count();
  ts->my_replacement = NULL;
  jsockd_log(LOG_DEBUG, "Thread state cleanup complete\n");
  atomic_store_explicit(&ts->replacement_thread_state,
                        REPLACEMENT_THREAD_STATE_CLEANUP_COMPLETE,
                        memory_order_release);
}

// called only when destroying a thread state before program exit
static void destroy_thread_state(ThreadState *ts) {
  // We know that any running instance of
  // reset_thread_state_cleanup_old_runtime_thread or
  // reset_thread_state_thread has now been joined, so if we're in one of the
  // following states, that means that a new thread state was created but we
  // never got round to using it and then initiating cleanup of the old one
  // before exiting.
  int rts =
      atomic_load_explicit(&ts->replacement_thread_state, memory_order_acquire);
  if (rts == REPLACEMENT_THREAD_STATE_INIT_COMPLETE ||
      rts == REPLACEMENT_THREAD_STATE_CLEANUP) {
    cleanup_old_runtime(ts);
  }

  cleanup_socket_state(ts->socket_state);
  cleanup_thread_state(ts);
}

static void *reset_thread_state_cleanup_old_runtime_thread(void *data) {
  cleanup_old_runtime((ThreadState *)data);
  return NULL;
}

static int handle_line_1_message_uid(ThreadState *ts, const char *line,
                                     int len) {
  if (len > MESSAGE_UUID_MAX_BYTES) {
    jsockd_logf(LOG_WARN,
                "Error: message UUID has length %i and will be truncated to "
                "first %i bytes\n",
                len, MESSAGE_UUID_MAX_BYTES);
    len = MESSAGE_UUID_MAX_BYTES;
  }

  int rts =
      atomic_load_explicit(&ts->replacement_thread_state, memory_order_acquire);

  // Check to see if the thread state has been reinitialized (following a
  // memory increase).
  if (rts == REPLACEMENT_THREAD_STATE_INIT_COMPLETE) {
    // Join the thread that initialized the replacement thread state to
    // reclaim pthread resources.
    if (0 != pthread_join(ts->replacement_thread, NULL)) {
      jsockd_logf(LOG_ERROR, "pthread_join failed: %s\n", strerror(errno));
      return -1;
    }
    jsockd_log(LOG_DEBUG, "Joined replacement thread [2]\n");
    ThreadState *r = ts->my_replacement;
    ts->my_replacement->my_replacement = NULL;
    memswap_small(ts->my_replacement, ts, sizeof(*ts));
    ts->my_replacement = r;
    register_thread_state_runtime(ts->rt, ts);
    atomic_store_explicit(&ts->replacement_thread_state,
                          REPLACEMENT_THREAD_STATE_CLEANUP,
                          memory_order_release);
    if (0 != pthread_create(&ts->replacement_thread, NULL,
                            reset_thread_state_cleanup_old_runtime_thread,
                            ts)) {
      jsockd_logf(LOG_ERROR, "pthread_create failed: %s\n", strerror(errno));
      atomic_store_explicit(&ts->replacement_thread_state,
                            REPLACEMENT_THREAD_STATE_NONE,
                            memory_order_release);
      return -1;
    }
    jsockd_log(LOG_INFO, "Trampolining to new thread state...\n");
    return TRAMPOLINE;
  }

  if (rts == REPLACEMENT_THREAD_STATE_CLEANUP_COMPLETE) {
    atomic_store_explicit(&ts->replacement_thread_state,
                          REPLACEMENT_THREAD_STATE_NONE, memory_order_release);
    // Reclaim pthread resources
    if (0 != pthread_join(ts->replacement_thread, NULL)) {
      jsockd_logf(LOG_ERROR, "pthread_join failed: %s\n", strerror(errno));
      return -1;
    }
    jsockd_log(LOG_DEBUG, "Joined replacement thread [1]\n");
    // We can now continue to process the line
  }

  strncpy(ts->current_uuid, line, len);
  ts->current_uuid_len = len;
  ts->line_n++;
  return 0;
}

static int handle_line_2_query(ThreadState *ts, const char *line, int len) {
  const HashCacheUid uid = get_hash_cache_uid(line, len);
  const CachedFunction *cf = get_cached_function(uid);

#ifdef CMAKE_BUILD_TYPE_DEBUG
  jsockd_logf(LOG_DEBUG,
              "Computed UID: " HASH_CACHE_UID_FORMAT_SPECIFIER
              " [bits=%i, bucket=%zu] for %.*s\n",
              HASH_CACHE_UID_FORMAT_ARGS(uid), CACHED_FUNCTION_HASH_BITS,
              get_cache_bucket(uid, CACHED_FUNCTION_HASH_BITS), len, line);
#endif

  if (cf) {
    jsockd_log(LOG_DEBUG, "Found cached function\n");
    ts->compiled_query =
        func_from_bytecode(ts->ctx, cf->bytecode, cf->bytecode_size);
  } else {
    jsockd_log(LOG_DEBUG, "Compiling...\n");
    // We compile and cache the function.
    size_t bytecode_size;

    const uint8_t *bytecode = compile_buf(ts->ctx, line, len, &bytecode_size);
    if (!bytecode) {
      ts->compiled_query = JS_EXCEPTION;
    } else {
      ts->cached_function_in_use =
          add_cached_function(uid, bytecode, bytecode_size);
      if (!ts->cached_function_in_use) {
        assert(ts->dangling_bytecode == NULL);
        jsockd_log(LOG_DEBUG, "Dangling bytecode\n");
        ts->dangling_bytecode = (uint8_t *)bytecode;
      }
      ts->compiled_query = func_from_bytecode(ts->ctx, bytecode, bytecode_size);
    }
  }

  ts->line_n++;
  return 0;
}

static const char *format_memusage(const JSMemoryUsage *m) {
  const char *fmt =
      "{\"malloc_size\":%" PRId64 ",\"malloc_count\":%" PRId64 "}";
  int n = snprintf(NULL, 0, fmt, m->malloc_size, m->malloc_count);
  char *buf = (char *)malloc((size_t)(n + 1));
  snprintf(buf, n + 1, fmt, m->malloc_size, m->malloc_count);
  return buf;
}

static int64_t memusage(const JSMemoryUsage *m) {
  return m->malloc_count + m->malloc_size;
}

static void *reset_thread_state_thread(void *data) {
  ThreadState *ts = (ThreadState *)data;
  ts->my_replacement = (ThreadState *)malloc(sizeof(ThreadState));
  debug_inc_new_thread_state_count();
  if (0 != init_thread_state((ThreadState *)ts->my_replacement,
                             ts->socket_state, ts->thread_index)) {
    jsockd_log(LOG_ERROR, "Error initializing replacement thread state\n");
    atomic_store_explicit(&g_interrupted_or_error, true, memory_order_release);
    ts->exit_status = 1;
    return NULL;
  }
  atomic_store_explicit(&ts->replacement_thread_state,
                        REPLACEMENT_THREAD_STATE_INIT_COMPLETE,
                        memory_order_release);
  return NULL;
}

static void write_to_stream(ThreadState *ts, const char *buf, size_t len) {
  if (0 != write_all(ts->socket_state->streamfd, buf, len)) {
    ts->socket_state->stream_io_err = -1;
    jsockd_logf(LOG_ERROR, "Error writing to socket: %s\n", strerror(errno));
    return;
  }
}

static void writev_to_stream_helper(ThreadState *ts, struct iovec *iov,
                                    int iovcnt) {
  if (0 != writev_all(ts->socket_state->streamfd, iov, iovcnt)) {
    ts->socket_state->stream_io_err = -1;
    jsockd_logf(LOG_ERROR, "Error writing to socket: %s\n", strerror(errno));
    return;
  }
}

#define writev_to_stream(ts, ...)                                              \
  writev_to_stream_helper((ts), (struct iovec[]){__VA_ARGS__},                 \
                          sizeof((struct iovec[]){__VA_ARGS__}) /              \
                              sizeof(struct iovec))

static void write_to_wbuf_wrapper(void *opaque, const char *inp, size_t size) {
  write_to_wbuf((WBuf *)opaque, inp, size);
}

#define write_const_to_stream(ts, str)                                         \
  write_to_stream((ts), (str), sizeof(str) - 1)

static int handle_line_3_parameter_helper(ThreadState *ts, const char *line,
                                          int len) {
  const JSPrintValueOptions js_print_value_options = {.show_hidden = false,
                                                      .raw_dump = false,
                                                      .max_depth = 0,
                                                      .max_string_length = 0,
                                                      .max_item_count = 0};

  ts->line_n = 0;

  if (JS_IsException(ts->compiled_query)) {
    if (CMAKE_BUILD_TYPE_IS_DEBUG) {
      LogLevel l = LOG_ERROR;
      JS_PrintValue(ts->ctx, js_print_value_debug_write, (void *)&l,
                    ts->compiled_query, &js_print_value_options);
    }
    writev_to_stream(
        ts,
        {.iov_base = (void *)ts->current_uuid, .iov_len = ts->current_uuid_len},
        STRCONST_IOVEC(" exception \"error compiling command\"\n"));
    return ts->socket_state->stream_io_err;
  }

  if (0 != clock_gettime(MONOTONIC_CLOCK, &ts->last_js_execution_start)) {
    jsockd_log(LOG_ERROR,
               "Error getting time in handle_line_3_parameter [1]\n");
    return -1;
  }

  JSValue parsed_arg = JS_ParseJSON(ts->ctx, line, len, "<input>");
  if (JS_IsException(parsed_arg)) {
    jsockd_logf(LOG_DEBUG, "Error parsing JSON argument: <<END\n%.*s\nEND\n",
                len, line);
    dump_error(ts->ctx);
    JS_FreeValue(ts->ctx, parsed_arg);
    writev_to_stream(
        ts,
        {.iov_base = (void *)ts->current_uuid, .iov_len = ts->current_uuid_len},
        STRCONST_IOVEC(" exception \"JSON input parse error\"\n"));
    return ts->socket_state->stream_io_err;
  }

  JSValue argv[] = {ts->compiled_module, parsed_arg};
  JSValue ret = JS_Call(ts->ctx, ts->compiled_query, JS_NULL,
                        sizeof(argv) / sizeof(argv[0]), argv);
  ret = js_std_await(ts->ctx, ret); // allow return of a promise
  if (JS_IsException(ret)) {
    jsockd_log(LOG_DEBUG, "Error calling cached function\n");

    char *error_msg_buf = calloc(ERROR_MSG_MAX_BYTES, sizeof(char));
    WBuf emb = {
        .buf = error_msg_buf, .index = 0, .length = ERROR_MSG_MAX_BYTES};
    JSValue exception = JS_GetException(ts->ctx);
    JS_PrintValue(ts->ctx, write_to_wbuf_wrapper, &emb, exception, NULL);

    const char *bt_str = "{}";
    size_t bt_length = 0;
    const char *bt_str_ =
        get_backtrace(ts, emb.buf, emb.index, &bt_length, BACKTRACE_JSON);
    if (bt_str_)
      bt_str = bt_str_;

    writev_to_stream(
        ts,
        {.iov_base = (void *)ts->current_uuid, .iov_len = ts->current_uuid_len},
        STRCONST_IOVEC(" exception "),
        {.iov_base = (void *)bt_str, .iov_len = bt_length},
        STRCONST_IOVEC("\n"));

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
    writev_to_stream(
        ts,
        {.iov_base = (void *)ts->current_uuid, .iov_len = ts->current_uuid_len},
        STRCONST_IOVEC(" exception \"error attempting to "
                       "JSON serialize return value\"\n"));
    return ts->socket_state->stream_io_err;
  }

  if (JS_IsUndefined(stringified)) {
    JS_FreeValue(ts->ctx, stringified);
    JS_FreeValue(ts->ctx, parsed_arg);
    JS_FreeValue(ts->ctx, ret);
    writev_to_stream(
        ts,
        {.iov_base = (void *)ts->current_uuid, .iov_len = ts->current_uuid_len},
        STRCONST_IOVEC(" exception \"unserializable return value\"\n"));
    return ts->socket_state->stream_io_err;
  }

  struct timespec now;
  if (0 != clock_gettime(MONOTONIC_CLOCK, &now)) {
    JS_FreeValue(ts->ctx, parsed_arg);
    JS_FreeValue(ts->ctx, ret);
    JS_FreeValue(ts->ctx, stringified);
    jsockd_log(LOG_ERROR,
               "Error getting time in handle_line_3_parameter [2]\n");
    return -1;
  }

  ts->last_command_exec_time_ns =
      ns_time_diff(&now, &ts->last_js_execution_start);

  size_t sz;
  const char *str = JS_ToCStringLen(ts->ctx, &sz, stringified);

  writev_to_stream(
      ts,
      {.iov_base = (void *)ts->current_uuid, .iov_len = ts->current_uuid_len},
      STRCONST_IOVEC(" ok "), {.iov_base = (void *)str, .iov_len = sz},
      STRCONST_IOVEC("\n"));

  JS_FreeValue(ts->ctx, parsed_arg);
  JS_FreeValue(ts->ctx, ret);
  JS_FreeCString(ts->ctx, str);
  JS_FreeValue(ts->ctx, stringified);

  ts->memory_check_count =
      ((ts->memory_check_count + 1) % MEMORY_CHECK_INTERVAL);
  if ((manually_trigger_thread_state_reset(ts) ||
       0 == ts->memory_check_count) &&
      REPLACEMENT_THREAD_STATE_NONE ==
          atomic_load_explicit(&ts->replacement_thread_state,
                               memory_order_acquire)) {
    JSMemoryUsage mu;
    JS_ComputeMemoryUsage(ts->rt, &mu);
    int64_t current_usage = memusage(&mu);
    jsockd_logf(LOG_DEBUG, "Memory usage %" PRId64 "\n", current_usage);
    if (manually_trigger_thread_state_reset(ts) ||
        (atomic_load_explicit(&g_n_cached_functions, memory_order_relaxed) <=
             ts->last_n_cached_functions &&
         current_usage > ts->last_memory_usage)) {
      ts->last_memory_usage = current_usage;
      ts->last_n_cached_functions =
          atomic_load_explicit(&g_n_cached_functions, memory_order_relaxed);
      ts->memory_increase_count++;
      if (manually_trigger_thread_state_reset(ts) ||
          ts->memory_increase_count > MEMORY_INCREASE_MAX_COUNT) {
        jsockd_logf(LOG_ERROR,
                    "Memory usage has increased over the last %i commands. "
                    "Resetting interpreter state.\n",
                    MEMORY_INCREASE_MAX_COUNT * MEMORY_CHECK_INTERVAL);
        // To avoid latency, we do the following:
        //   (i)   create a new thread state in a background thread,
        //   (ii)  swap the old and new thread states the next time we're in
        //         the line_1 handler and the new thread state has finished
        //         initializing, and then
        //   (iii) clean up the old thread state in a background thread.
        atomic_store_explicit(&ts->replacement_thread_state,
                              REPLACEMENT_THREAD_STATE_INIT,
                              memory_order_release);
        if (0 != pthread_create(&ts->replacement_thread, NULL,
                                reset_thread_state_thread, (void *)ts)) {
          jsockd_logf(LOG_ERROR, "pthread_create failed: %s\n",
                      strerror(errno));
          atomic_store_explicit(&ts->replacement_thread_state,
                                REPLACEMENT_THREAD_STATE_NONE,
                                memory_order_release);
          return -1;
        }

        ts->memory_increase_count = 0;

#ifdef CMAKE_BUILD_TYPE_DEBUG
        ts->manually_trigger_thread_state_reset = false;
#endif
      }
    } else {
      ts->memory_increase_count = 0;
    }
  }

  return ts->socket_state->stream_io_err;
}

static int handle_line_3_parameter(ThreadState *ts, const char *line, int len) {
  int r = handle_line_3_parameter_helper(ts, line, len);
  cleanup_command_state(ts);
  return r;
}

static int line_handler(const char *line, size_t len, ThreadState *ts,
                        bool truncated) {
  jsockd_logf(LOG_DEBUG, "LINE %i on %s: %s\n", ts->line_n,
              ts->socket_state->unix_socket_filename, line);

  if (0 != clock_gettime(MONOTONIC_CLOCK, &ts->last_active_time)) {
    jsockd_logf(LOG_ERROR, "Error getting time in line_handler thread %i: %s\n",
                ts->thread_index, strerror(errno));
    return -1;
  }

  // Allow the truncation logic to be triggered by a special '?truncated'
  // line in debug builds so that we don't have to generate large inputs
  // in the Valgrind tests.
  if (truncated || (CMAKE_BUILD_TYPE_IS_DEBUG && !strcmp(line, "?truncated")))
    ts->truncated = true;

  if (truncated) {
    if (ts->line_n == 2) {
      ts->truncated = false;
      ts->line_n = 0;
      JS_FreeValue(ts->ctx, ts->compiled_query);
      ts->compiled_query = JS_UNDEFINED;
      writev_to_stream(
          ts,
          {.iov_base = (void *)ts->current_uuid,
           .iov_len = ts->current_uuid_len},
          STRCONST_IOVEC(" exception \"jsockd command was too long\n"));
    } else {
      // we'll signal an error once the client has sent the third line
      ts->line_n++;
    }
    return 0;
  }

  if (!strcmp("?quit", line)) {
    JS_FreeValue(ts->ctx, ts->compiled_query);
    ts->compiled_query = JS_UNDEFINED;
    atomic_store_explicit(&g_interrupted_or_error, true, memory_order_release);
    write_const_to_stream(ts, "quit\n");
    return EXIT_ON_QUIT_COMMAND;
  }
  if (!strcmp("?reset", line)) {
    cleanup_command_state(ts);
    ts->line_n = 0;
    ts->truncated = false;
    write_const_to_stream(ts, "reset\n");
    return 0;
  }
  if (!strcmp("?exectime", line)) {
    char exec_time_buf[21]; // 20 digits for int64_t, + 1 for zeroterm
    int exec_time_len = snprintf(
        exec_time_buf, sizeof(exec_time_buf) / sizeof(exec_time_buf[0]),
        "%" PRId64, ts->last_command_exec_time_ns);
    writev_to_stream(
        ts,
        {.iov_base = (void *)exec_time_buf, .iov_len = (size_t)exec_time_len},
        STRCONST_IOVEC("\n"));
    return 0;
  }
  if (!strcmp("?memusage", line)) {
    JSMemoryUsage mu;
    JS_ComputeMemoryUsage(ts->rt, &mu);
    const char *memusage_str = format_memusage(&mu);
    writev_to_stream(
        ts, {.iov_base = (void *)memusage_str, .iov_len = strlen(memusage_str)},
        STRCONST_IOVEC("\n"));
    free((void *)memusage_str);
    return 0;
  }
#ifdef CMAKE_BUILD_TYPE_DEBUG
  if (!strcmp("?tsreset", line)) {
    ts->manually_trigger_thread_state_reset = true;
    write_const_to_stream(ts, "tsreset\n");
    return 0;
  }
#endif

  if (line[0] == '?') {
    write_const_to_stream(ts, "bad command\n");
    return 0;
  }

  jsockd_logf(LOG_DEBUG, "Line handler: line %i\n", ts->line_n + 1);
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

static void tick_handler(ThreadState *ts) {
  if (g_cmd_args.max_idle_time_us == 0 || ts->line_n != 0 || ts->rt == NULL)
    return;
  int n_ready_threads =
      atomic_load_explicit(&g_n_ready_threads, memory_order_acquire);
  if (n_ready_threads <= 1 || n_ready_threads != ts->thread_index + 1)
    return;
  struct timespec now;
  if (0 != clock_gettime(MONOTONIC_CLOCK, &now)) {
    jsockd_logf(LOG_ERROR, "Error getting time in tick_handler thread %i: %s\n",
                ts->thread_index, strerror(errno));
    return;
  }
  uint64_t ns_diff = ns_time_diff(&now, &ts->last_active_time);
  if (ns_diff / 1000ULL >= g_cmd_args.max_idle_time_us &&
      REPLACEMENT_THREAD_STATE_NONE ==
          atomic_load_explicit(&ts->replacement_thread_state,
                               memory_order_acquire)) {
    atomic_fetch_add_explicit(&g_n_ready_threads, -1, memory_order_relaxed);
    jsockd_logf(LOG_DEBUG, "Shutting down QuickJS on thread %s\n",
                ts->socket_state->unix_socket_filename);
    cleanup_thread_state(ts);
  }
}

static void *listen_thread_func(void *data) {
  ThreadState *ts = (ThreadState *)data;
  command_loop(ts, line_handler, tick_handler);
  jsockd_log(LOG_DEBUG, "Listen thread terminating...\n");
  return NULL;
}

static pthread_t *g_threads;
static SocketState *g_socket_states;

static const uint8_t *load_module_bytecode(const char *filename,
                                           size_t *out_size) {
  const uint8_t *module_bytecode = mmap_file(filename, out_size);
  if (!module_bytecode)
    return NULL;
  if (*out_size < VERSION_STRING_SIZE + ED25519_SIGNATURE_SIZE + 1) {
    jsockd_logf(LOG_ERROR | LOG_INTERACTIVE,
                "Module bytecode file is only %zu bytes. Too small!",
                *out_size);
    munmap_or_warn((void *)module_bytecode, *out_size);
    return NULL;
  }

  const char *pubkey = getenv("JSOCKD_BYTECODE_MODULE_PUBLIC_KEY");
  if (!pubkey)
    pubkey = "";

  char version_string[VERSION_STRING_SIZE];
  memcpy(version_string,
         module_bytecode + *out_size - ED25519_SIGNATURE_SIZE -
             VERSION_STRING_SIZE,
         VERSION_STRING_SIZE);
  version_string[VERSION_STRING_SIZE - 1] = '\0';
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wconstant-logical-operand"
  if (strcmp(version_string, STRINGIFY(VERSION)) &&
      (!(!strcmp(version_string, "unknown_version") &&
         !strcmp(STRINGIFY(VERSION), "unknown_version") &&
         CMAKE_BUILD_TYPE_IS_DEBUG)) &&
      !(!strcmp(pubkey, MAGIC_KEY_TO_ALLOW_INVALID_SIGNATURES) &&
        CMAKE_BUILD_TYPE_IS_DEBUG)) {
#pragma clang diagnostic pop
    for (int i = 0; i < VERSION_STRING_SIZE && version_string[i] != '\0'; ++i) {
      if (version_string[i] < 32 || version_string[i] > 126)
        version_string[i] = '?';
    }
    jsockd_logf(
        LOG_ERROR | LOG_INTERACTIVE,
        "Module bytecode version string '%s' does not match expected '%s'\n",
        version_string, STRINGIFY(VERSION));
    munmap_or_warn((void *)module_bytecode, *out_size);
    return NULL;
  }

  if (CMAKE_BUILD_TYPE_IS_DEBUG &&
      !strcmp(pubkey, MAGIC_KEY_TO_ALLOW_INVALID_SIGNATURES)) {
    *out_size = *out_size - VERSION_STRING_SIZE - ED25519_SIGNATURE_SIZE;
    return module_bytecode;
  }

  uint8_t pubkey_bytes[ED25519_PUBLIC_KEY_SIZE];
  size_t decoded_size = hex_decode(
      pubkey_bytes, sizeof(pubkey_bytes) / sizeof(pubkey_bytes[0]), pubkey);
  if (decoded_size != ED25519_PUBLIC_KEY_SIZE) {
    jsockd_logf(LOG_ERROR | LOG_INTERACTIVE,
                "Error decoding public key hex from environment variable "
                "JSOCKD_BYTECODE_MODULE_PUBLIC_KEY; decoded size=%zu\n",
                decoded_size);
    munmap_or_warn((void *)module_bytecode, *out_size);
    return NULL;
  }
  if (!verify_bytecode(module_bytecode, *out_size, pubkey_bytes)) {
    jsockd_logf(LOG_ERROR | LOG_INTERACTIVE,
                "Error verifying bytecode module %s with public key %s\n",
                filename, pubkey);
    munmap_or_warn((void *)module_bytecode, *out_size);
    return NULL;
  }

  *out_size = *out_size - VERSION_STRING_SIZE - ED25519_SIGNATURE_SIZE;

  return module_bytecode;
}

static void global_cleanup(void) {
  for (size_t i = 0; i < CACHED_FUNCTIONS_N_BUCKETS; ++i)
    free((void *)g_cached_function_buckets[i].payload.bytecode);

  // These can fail, but we're calling this when we're about to exit, so
  // there is no useful error handling to be done.
  wait_group_destroy(&g_thread_ready_wait_group);

  if (g_module_bytecode_size != 0 && g_module_bytecode)
    munmap_or_warn((void *)g_module_bytecode,
                   g_module_bytecode_size + ED25519_SIGNATURE_SIZE);
  if (g_source_map_size != 0 && g_source_map)
    munmap_or_warn((void *)g_source_map, g_source_map_size);
}

static void SIGINT_and_SIGTERM_handler(int sig) {
  static atomic_int already_called = 0;
  int n = atomic_fetch_add_explicit(&already_called, 1, memory_order_relaxed);
  if (n > 0)
    return;

  atomic_store_explicit(&g_sig_triggered, sig, memory_order_relaxed);
  atomic_store_explicit(&g_interrupted_or_error, true, memory_order_release);

  // Using stdio inside an interrupt handler is not safe, but calls to write
  // are explicilty allowed.
  const char int_msg[] = "$ jsockd 0000-00-00T00:00:00.000000Z [INFO] SIGINT "
                         "received, cleaning up...\n";
  const char term_msg[] = "$ jsockd 0000-00-00T00:00:00.000000Z [INFO] "
                          "SIGTERM received, cleaning up...\n";
  if (sig == SIGINT)
    write_all(2, int_msg, sizeof(int_msg) - 1);
  else
    write_all(2, term_msg, sizeof(term_msg) - 1);
}

static void log_to_stderr(const char *fmt, ...) {
  va_list vl;
  va_start(vl, fmt);
  vfprintf(stderr, fmt, vl);
}

static void set_log_prefix(void) {
  const char *lp = getenv("JSOCKD_LOG_PREFIX");
  if (lp && lp[0] != '\0') {
    const char *nl = strchr(lp, '\n');
    const char *cr = strchr(lp, '\r');
    if (!nl && !cr)
      g_log_prefix = lp;
  }
}

static int eval(void) {
  g_interactive_logging_mode = true;

  if (g_cmd_args.es6_module_bytecode_file) {
    g_module_bytecode = load_module_bytecode(
        g_cmd_args.es6_module_bytecode_file, &g_module_bytecode_size);
    // load_module_bytecode will log an error
    if (g_module_bytecode == NULL)
      return EXIT_FAILURE;
  }
  if (g_cmd_args.source_map_file) {
    g_source_map = mmap_file(g_cmd_args.source_map_file, &g_source_map_size);
    if (!g_source_map) {
      if (g_module_bytecode && g_module_bytecode_size != 0)
        munmap_or_warn((void *)g_module_bytecode,
                       g_module_bytecode_size + ED25519_SIGNATURE_SIZE);
      jsockd_logf(LOG_ERROR | LOG_INTERACTIVE,
                  "Error loading source map file %s: %s\n", strerror(errno));
      jsockd_log(LOG_INFO | LOG_INTERACTIVE, "Continuing without source map\n");
    }
  }

  g_thread_states = malloc(sizeof(ThreadState));
  memset(g_thread_states, 0, sizeof(ThreadState));
  ThreadState *ts = &g_thread_states[0];
  init_thread_state(ts, NULL, 0);
  JSContext *ctx = g_thread_states[0].ctx;
  int exit_status = EXIT_SUCCESS;
  JSValue glob = JS_UNDEFINED;
  JSValue result = JS_UNDEFINED;

  const char *eval_input = g_cmd_args.eval_input;
  if (eval_input == EVAL_INPUT_STDIN_SENTINEL) {
    eval_input = read_all_stdin();
    if (!eval_input) {
      jsockd_logf(LOG_ERROR | LOG_INTERACTIVE,
                  "Error reading stdin for eval input: %s\n", strerror(errno));
      exit_status = EXIT_FAILURE;
      goto cleanup;
    }
  }

  glob = JS_GetGlobalObject(ctx);
  /*if (JS_SetPropertyStr(ctx, glob, "M", ts->compiled_module) < 0) {
    jsockd_logf(LOG_ERROR | LOG_INTERACTIVE,
                "Error setting M property for eval\n");
    dump_error(ctx);
    exit_status = EXIT_FAILURE;
    goto cleanup;
  }*/

  result =
      JS_Eval(g_thread_states[0].ctx, eval_input, strlen(g_cmd_args.eval_input),
              "<cmdline>", JS_EVAL_TYPE_GLOBAL);

  if (JS_IsException(result)) {
    dump_error(ts->ctx);
    exit_status = EXIT_FAILURE;
  }

  JS_PrintValue(ts->ctx, print_value_to_stdout, NULL, result, NULL);

cleanup:
  JS_FreeValue(ts->ctx, result);
  JS_FreeValue(ts->ctx, glob);
  // JS_FreeValue(ts->ctx, ts->compiled_module);
  if (eval_input && eval_input != g_cmd_args.eval_input)
    free((void *)eval_input);
  cleanup_thread_state(&g_thread_states[0]);
  if (g_module_bytecode && g_module_bytecode_size != 0)
    munmap_or_warn((void *)g_module_bytecode,
                   g_module_bytecode_size + ED25519_SIGNATURE_SIZE);
  if (g_source_map_size != 0 && g_source_map)
    munmap_or_warn((void *)g_source_map, g_source_map_size);

  return exit_status;
}

int main(int argc, char **argv) {
  struct sigaction sa = {.sa_handler = SIGINT_and_SIGTERM_handler};
  sigaction(SIGINT, &sa, NULL);
  sigaction(SIGTERM, &sa, NULL);

  set_log_prefix();

  if (0 != parse_cmd_args(argc, argv, log_to_stderr, &g_cmd_args)) {
    return EXIT_FAILURE;
  }

  if (g_cmd_args.version) {
    printf("jsockd %s", STRINGIFY(VERSION));
    return EXIT_SUCCESS;
  }

  if (g_cmd_args.eval)
    return eval();

  int strip_flags = 0;
  if (g_cmd_args.compile_opts == COMPILE_OPTS_STRIP_DEBUG)
    strip_flags = JS_STRIP_DEBUG;
  else if (g_cmd_args.compile_opts == COMPILE_OPTS_STRIP_SOURCE)
    strip_flags = JS_STRIP_SOURCE;
  if (g_cmd_args.mod_to_compile) {
    return compile_module_file(
        g_cmd_args.mod_to_compile, g_cmd_args.key_file_prefix,
        g_cmd_args.mod_output_file, STRINGIFY(VERSION), strip_flags);
  }

  if (g_cmd_args.key_file_prefix)
    return output_key_file(g_cmd_args.key_file_prefix);

  if (g_cmd_args.es6_module_bytecode_file) {
    g_module_bytecode = load_module_bytecode(
        g_cmd_args.es6_module_bytecode_file, &g_module_bytecode_size);
    if (g_module_bytecode == NULL)
      return EXIT_FAILURE;
  }

  if (g_cmd_args.source_map_file) {
    g_source_map = mmap_file(g_cmd_args.source_map_file, &g_source_map_size);
    if (!g_source_map) {
      jsockd_logf(LOG_ERROR, "Error loading source map file %s: %s\n",
                  strerror(errno));
      jsockd_log(LOG_INFO, "Continuing without source map\n");
    }
  }

  int n_threads = MIN(g_cmd_args.n_sockets, MAX_THREADS);
  atomic_store_explicit(&g_n_threads, g_cmd_args.n_sockets,
                        memory_order_relaxed);
  atomic_store_explicit(&g_n_ready_threads, g_cmd_args.n_sockets,
                        memory_order_relaxed);

  g_thread_states = calloc(n_threads, sizeof(ThreadState));
  memset(g_thread_states, 0, sizeof(ThreadState) * n_threads);
  g_threads = calloc(n_threads, sizeof(pthread_t));
  g_socket_states = calloc(n_threads, sizeof(SocketState));

  if (0 != wait_group_init(&g_thread_ready_wait_group, n_threads)) {
    jsockd_logf(LOG_ERROR, "Error initializing wait group: %s\n",
                strerror(errno));
    if (g_module_bytecode_size != 0 && g_module_bytecode)
      munmap_or_warn((void *)g_module_bytecode,
                     g_module_bytecode_size + ED25519_SIGNATURE_SIZE);
    goto cleanup_on_error;
  }
  atomic_init(&g_global_init_complete, true);

  int thread_init_n = 0;
  for (thread_init_n = 0; thread_init_n < n_threads; ++thread_init_n) {
    jsockd_logf(LOG_DEBUG, "Creating thread %i\n", thread_init_n);
    g_thread_state_input_buffers[thread_init_n] =
        calloc(INPUT_BUF_BYTES, sizeof(char));
    init_socket_state(&g_socket_states[thread_init_n],
                      g_cmd_args.socket_path[thread_init_n]);
    if (0 != init_thread_state(&g_thread_states[thread_init_n],
                               &g_socket_states[thread_init_n],
                               thread_init_n)) {
      jsockd_logf(LOG_ERROR, "Error initializing thread %i\n", thread_init_n);
      if (g_module_bytecode_size != 0 && g_module_bytecode)
        munmap_or_warn((void *)g_module_bytecode, g_module_bytecode_size);
      goto thread_init_error;
    }
    register_thread_state_runtime(g_thread_states[thread_init_n].rt,
                                  &g_thread_states[thread_init_n]);
    pthread_attr_t attr;
    if (0 != pthread_attr_init(&attr)) {
      jsockd_logf(LOG_ERROR, "pthread_attr_init failed: %s\n", strerror(errno));
      goto thread_init_error;
    }
    if (0 != pthread_attr_setstacksize(&attr, QUICKS_THREAD_STACK_SIZE)) {
      jsockd_logf(LOG_ERROR, "pthread_attr_setstacksize failed: %s\n",
                  strerror(errno));
      pthread_attr_destroy(&attr); // can fail, but no point in checking
      goto thread_init_error;
    }
    if (0 != pthread_create(&g_threads[thread_init_n], &attr,
                            listen_thread_func,
                            &g_thread_states[thread_init_n])) {
      jsockd_logf(LOG_ERROR, "pthread_create failed; exiting: %s",
                  strerror(errno));
      pthread_attr_destroy(&attr); // can fail, but no point in checking
      goto thread_init_error;
    }
    pthread_attr_destroy(&attr); // can fail, but no point in checking
  }

  // Wait for all threads to be ready
  if (0 != wait_group_timed_wait(&g_thread_ready_wait_group,
                                 10000000000 /* 10 sec in ns */)) {
    jsockd_logf(
        LOG_ERROR,
        "Error waiting for threads to be ready, or timeout; n_remaining=%i\n",
        wait_group_n_remaining(&g_thread_ready_wait_group));
    goto thread_init_error;
  }

  printf("READY %i %s\n", n_threads, STRINGIFY(VERSION));
  fflush(stdout);

  for (int i = 0; i < atomic_load_explicit(&g_n_threads, memory_order_relaxed);
       ++i) {
    if (0 != pthread_join(g_threads[i], NULL)) {
      jsockd_logf(LOG_ERROR, "Error joining thread %i: %s\n", i,
                  strerror(errno));
      continue;
    }
    int rts = atomic_load_explicit(&g_thread_states[i].replacement_thread_state,
                                   memory_order_acquire);
    if (rts != REPLACEMENT_THREAD_STATE_NONE) {
      // As we've just joined the thread, we know it won't be concurrently
      // updating replacement_thread.
      jsockd_logf(LOG_DEBUG, "Joining replacement thread for thread %i\n", i);
      pthread_join(g_thread_states[i].replacement_thread, NULL);
    }
  }

  jsockd_log(LOG_DEBUG, "All threads joined\n");

  for (int i = 0; i < atomic_load_explicit(&g_n_threads, memory_order_relaxed);
       ++i) {
    destroy_thread_state(&g_thread_states[i]);
    free(g_thread_state_input_buffers[i]);
  }
  jsockd_log(LOG_DEBUG, "All thread states destroyed\n");

  global_cleanup();
  jsockd_log(LOG_DEBUG, "Global cleanup complete\n");

#ifdef CMAKE_BUILD_TYPE_DEBUG
  int tsc =
      atomic_load_explicit(&g_new_thread_state_count, memory_order_relaxed);
  jsockd_logf(LOG_DEBUG, "g_new_thread_state_count: %i\n", tsc);
  if (tsc != 0) {
    jsockd_log(LOG_DEBUG,
               "Something's up with g_new_thread_state_count (see above)\n");
    return EXIT_FAILURE;
  }
#endif

  for (int i = 0; i < atomic_load_explicit(&g_n_threads, memory_order_relaxed);
       ++i) {
    if (g_thread_states[i].exit_status != 0)
      return EXIT_FAILURE;
  }

  if (SIGINT == atomic_load_explicit(&g_sig_triggered, memory_order_relaxed))
    return EXIT_FAILURE;

  return EXIT_SUCCESS;

thread_init_error:
  for (int i = 0; i <= thread_init_n; ++i)
    destroy_thread_state(&g_thread_states[i]);
cleanup_on_error:
  global_cleanup();
  free(g_thread_states);
  free(g_threads);
  free(g_socket_states);
  return EXIT_FAILURE;
}

// supress the annoying warning that inttypes.h is not used directly
static const char *supress_unused_header_warning UNUSED = PRId64;
