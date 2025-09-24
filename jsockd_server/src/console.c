#include "log.h"
#include "quickjs.h"
#include "utils.h"
#include <stdio.h>

#include <stdlib.h>

typedef struct {
  const char *data;
  size_t pos;
} LogState;

static void js_print_value_write(void *opaque, const char *buf, size_t len) {
  log_with_prefix_for_subsequent_lines(LOG_INFO, (FILE *)opaque, buf, len);
}

static JSValue js_print(JSContext *ctx, JSValueConst this_val, int argc,
                        JSValueConst *argv) {
  int i;
  JSValueConst v;

  print_log_prefix(LOG_INFO, stderr, 1);
  fputs("<console.*>: ", stderr);
  for (i = 0; i < argc; i++) {
    if (i != 0)
      fputc(' ', stderr);
    v = argv[i];
    if (JS_IsString(v)) {
      const char *str;
      size_t len;
      str = JS_ToCStringLen(ctx, &len, v);
      if (!str)
        return JS_EXCEPTION;
      log_with_prefix_for_subsequent_lines(LOG_INFO, stderr, str, len);
      JS_FreeCString(ctx, str);
    } else {
      JS_PrintValue(ctx, js_print_value_write, stderr, v, NULL);
    }
  }
  fputc('\n', stderr);
  return JS_UNDEFINED;
}

JSValue my_js_console_log(JSContext *ctx, JSValueConst this_val, int argc,
                          JSValueConst *argv) {
  if (g_log_mutex_initialized)
    mutex_lock(&g_log_mutex);
  JSValue ret;
  ret = js_print(ctx, this_val, argc, argv);
  fflush(stderr);
  if (g_log_mutex_initialized)
    mutex_unlock(&g_log_mutex);
  return ret;
}
