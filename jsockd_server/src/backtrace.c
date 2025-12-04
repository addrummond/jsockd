#include "backtrace.h"
#include "globals.h"
#include "log.h"
#include "quickjs.h"
#include "utils.h"
#include <stdlib.h>

const char *get_backtrace(ThreadState *ts, const char *backtrace,
                          size_t backtrace_length,
                          size_t *out_json_backtrace_length,
                          BacktraceFormat backtrace_format) {
  const char *bt_func_name =
      backtrace_format == BACKTRACE_JSON ? "parseBacktrace" : "formatBacktrace";
  JSValue bt_func =
      JS_GetPropertyStr(ts->ctx, ts->backtrace_module, bt_func_name);
  if (!JS_IsFunction(ts->ctx, bt_func)) {
    JS_FreeValue(ts->ctx, bt_func);
    jsockd_logf(LOG_ERROR, "Internal error: %s is not a function\n",
                bt_func_name);
    return NULL;
  }
  if (JS_IsUndefined(ts->sourcemap_str)) {
    ts->sourcemap_str =
        g_source_map_size == 0
            ? JS_UNDEFINED
            : JS_NewStringLen(ts->ctx, (const char *)g_source_map,
                              g_source_map_size);
    int c = atomic_fetch_add_explicit(&g_source_map_load_count, 1,
                                      memory_order_acq_rel);
    if (c + 1 == g_n_threads && g_source_map_size != 0 && g_source_map) {
      jsockd_log(LOG_DEBUG,
                 "All threads have loaded the sourcemap, calling munmap...\n");
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
    jsockd_log(LOG_ERROR, "Error parsing backtrace:\n");
    dump_error(ts->ctx);
    jsockd_logf(LOG_ERROR, "The backtrace that could not be parsed:\n%.*s",
                (int)backtrace_length, backtrace);
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
