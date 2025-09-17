#include "quickjs.h"
#include "utils.h"
#include <stdatomic.h>
#include <stdio.h>

#include <stdlib.h>

extern pthread_mutex_t g_log_mutex;
extern atomic_bool g_log_mutex_initialized;

typedef struct {
  const char *data;
  size_t pos;
} LogState;

static size_t remove_trailing_ws(const char *buf, size_t len) {
  if (len == 0)
    return 0;
  --len;
  do {
    if (buf[len] != ' ' && buf[len] != '\t' && buf[len] != '\n' &&
        buf[len] != '\r')
      break;
  } while (len-- > 0);
  return len + 1;
}

static void js_print_value_write(void *opaque, const char *buf, size_t len) {
  FILE *fo = opaque;

  len = remove_trailing_ws(buf, len);

  int line = 1;
  size_t start = 0;
  size_t i;
  for (i = 0; i < len; ++i) {
    if (buf[i] == '\n') {
      fwrite(buf + start, 1, i - start + 1, fo);
      ++line;
      start = i + 1;
      print_log_prefix(LOG_INFO, fo, line);
    }
  }
  fwrite(buf + start, 1, i - start, fo);
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
      js_print_value_write((void *)stderr, str, len);
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
