#include "threadstate.h"
#include "backtrace.h"
#include "config.h"
#include "console.h"
#include "custom_module_loader.h"
#include "globals.h"
#include "inttypes.h"
#include "log.h"
#include "messages.h"
#include "quickjs-libc.h"
#include "textencodedecode.h"
#include "threadstate.h"
#include "utils.h"
#include <assert.h>
#include <errno.h>
#include <stdatomic.h>

static int new_custom_context(JSRuntime *rt, JSContext **out_ctx) {
  JSContext *ctx;
  ctx = JS_NewContext(rt);
  *out_ctx = ctx;
  if (!ctx)
    return -1;

  js_init_module_std(ctx, "std");
  js_init_module_os(ctx, "os");

  JSValue global_obj = JS_GetGlobalObject(ctx);

  if (qjs_add_intrinsic_text_decoder(ctx, global_obj) < 0) {
    JS_FreeValue(ctx, global_obj);
    return -1;
  }

  if (qjs_add_intrinsic_text_encoder(ctx, global_obj) < 0) {
    JS_FreeValue(ctx, global_obj);
    return -1;
  }

  if (add_intrinsic_jsockd(ctx, global_obj) < 0) {
    JS_FreeValue(ctx, global_obj);
    return -1;
  }

  JS_FreeValue(ctx, global_obj);

  return 0;
}

static JSContext *new_custom_context_for_worker(JSRuntime *rt) {
  JSContext *ctx;
  if (0 != new_custom_context(rt, &ctx)) {
    jsockd_log(LOG_ERROR, "Failed to create custom context for worker\n");
    if (ctx != NULL)
      JS_FreeContext(ctx);
    return NULL;
  }
  return ctx;
}

static int interrupt_handler(JSRuntime *rt, void *opaque) {
  ThreadState *state = (ThreadState *)opaque;
  struct timespec *start = &state->last_js_execution_start;
  if (start->tv_sec != 0) {
    struct timespec now;
    if (0 != clock_gettime(MONOTONIC_CLOCK, &now)) {
      jsockd_logf(LOG_ERROR, "Error getting time in interrupt handler: %s\n",
                  strerror(errno));
      return 1;
    }
    int64_t delta_ns = ns_time_diff(&now, start);
    if (delta_ns > 0 &&
        (uint64_t)delta_ns > g_cmd_args.max_command_runtime_us * 1000ULL) {
      jsockd_logf(LOG_WARN,
                  "Command runtime of %" PRIu64 "us exceeded %" PRIu64
                  "us, interrupting\n",
                  delta_ns / 1000ULL, g_cmd_args.max_command_runtime_us);
      return 1;
    }
  }
  return (int)atomic_load_explicit(&g_interrupted_or_error,
                                   memory_order_relaxed);
}

static void write_to_wbuf_wrapper(void *opaque, const char *inp, size_t size) {
  write_to_wbuf((WBuf *)opaque, inp, size);
}

int init_thread_state(ThreadState *ts, SocketState *socket_state,
                      int thread_index) {
  jsockd_logf(LOG_DEBUG, "Calling init_thread_state for thread %i\n",
              thread_index);

  ts->thread_index = thread_index;
  ts->socket_state = socket_state;
  // set to nonzero if program should eventually exit with non-zero exit code
  ts->exit_status = 0;
  ts->line_n = 0;
  ts->compiled_query = JS_UNDEFINED;
  ts->last_js_execution_start.tv_sec = 0;
  ts->last_js_execution_start.tv_nsec = 0;
  ts->input_buf = calloc(INPUT_BUF_BYTES, sizeof(char));
  ts->current_uuid[0] = '\0';
  ts->current_uuid_len = 0;
  ts->memory_check_count = 0;
  ts->memory_increase_count = 0;
  ts->last_memory_usage = 0;
  ts->last_n_cached_functions = 1;
  ts->truncated = false;
  ts->last_command_exec_time_ns = 0;
  ts->my_replacement = NULL;
  ts->dangling_bytecode = NULL;
  ts->cached_function_in_use = NULL;
  atomic_init(&ts->replacement_thread_state, REPLACEMENT_THREAD_STATE_NONE);

  if (0 != clock_gettime(MONOTONIC_CLOCK, &ts->last_active_time)) {
    jsockd_logf(LOG_ERROR,
                "Error getting time while initializing thread %i state: %s\n",
                thread_index, strerror(errno));
    return -1;
  }

  ts->rt = JS_NewRuntime();
  if (!ts->rt) {
    jsockd_log(LOG_ERROR, "Failed to create JS runtime\n");
    return -1;
  }

  js_std_set_worker_new_context_func(new_custom_context_for_worker);
  js_std_init_handlers(ts->rt);
  if (0 != new_custom_context(ts->rt, &ts->ctx)) {
    jsockd_log(LOG_ERROR, "Failed to create JS context\n");
    return -1;
  }

  JS_SetModuleLoaderFunc2(ts->rt, NULL, jsockd_js_module_loader,
                          js_module_check_attributes, NULL);

  JSValue shims_module = load_binary_module(ts->ctx, g_shims_module_bytecode,
                                            g_shims_module_bytecode_size);
  assert(!JS_IsException(shims_module));
  JS_FreeValue(ts->ctx, shims_module); // imported just for side effects

  // Override console.log
  JSValue global_obj = JS_GetGlobalObject(ts->ctx);
  assert(JS_IsObject(global_obj));
  JSValue console = JS_GetPropertyStr(ts->ctx, global_obj, "console");
  assert(JS_IsObject(console));
  JS_FreeValue(ts->ctx, JS_GetPropertyStr(ts->ctx, console, "log"));
  assert(1 == JS_SetPropertyStr(
                  ts->ctx, console, "log",
                  JS_NewCFunction(ts->ctx, my_js_console_log, "log", 1)));
  JS_FreeValue(ts->ctx, console);
  JS_FreeValue(ts->ctx, global_obj);

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
    jsockd_log(LOG_ERROR, "Failed to load precompiled module\n");
    char *error_msg_buf = calloc(ERROR_MSG_MAX_BYTES, sizeof(char));
    WBuf emb = {
        .buf = error_msg_buf, .index = 0, .length = ERROR_MSG_MAX_BYTES};
    JSValue exception = JS_GetException(ts->ctx);
    JS_PrintValue(ts->ctx, write_to_wbuf_wrapper, &emb.buf, exception, NULL);
    JS_FreeValue(ts->ctx, exception);
    size_t bt_length;
    const char *bt_str =
        get_backtrace(ts, emb.buf, emb.index, &bt_length, BACKTRACE_PRETTY);
    if (!bt_str) {
      jsockd_logf(LOG_ERROR, "<no backtrace available>\n");
    } else {
      jsockd_logf(LOG_ERROR, "%.*s\n", (int)bt_length, bt_str);
      JS_FreeCString(ts->ctx, bt_str);
    }

    JS_FreeValue(ts->ctx, ts->compiled_module);
    free(error_msg_buf);

    // This return value will eventually lead to stuff getting
    // cleaned up by cleanup_js_runtime
    return -1;
  }

  register_thread_state_runtime(ts->rt, ts);

  JS_SetInterruptHandler(ts->rt, interrupt_handler, ts);

#ifdef CMAKE_BUILD_TYPE_DEBUG
  ts->manually_trigger_thread_state_reset = false;
#endif

  return 0;
}

// QuickJS LibC makes its own use of JS_Get/SetRuntimeOpaque, so we implement
// our own simple mapping from JSRuntime pointers to ThreadState pointers.

static JSRuntime *ts_rt_mapping[MAX_THREADS];

// We assume the functions below are called only from the thread associated with
// each runtime, so no need for locking/atomics.

void register_thread_state_runtime(JSRuntime *rt, ThreadState *ts) {
  assert(ts->thread_index < MAX_THREADS);
  ts_rt_mapping[ts->thread_index] = rt;
}

ThreadState *get_runtime_thread_state(JSRuntime *rt) {
  for (size_t i = 0; i < MAX_THREADS; i++) {
    if (ts_rt_mapping[i] == rt)
      return &g_thread_states[i];
  }
  assert(0 && "ThreadState not found for given JSRuntime");
  return NULL;
}
