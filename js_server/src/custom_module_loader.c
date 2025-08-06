#ifndef _REENTRANT
#define _REENTRANT
#endif

#include "custom_module_loader.h"
#include "quickjs-libc.h"
#include "quickjs.h"
#include "utils.h"
#include <assert.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// Adapted from 'js_module_loader' and 'js_std_eval_binary' in quickjs-libc.c
// See also
// https://github.com/chqrlie/quickjs-ng/blob/78910046ada3f3b58d9b690ee233ac8280965246/qjs.c#L75
JSValue load_binary_module(JSContext *ctx, const uint8_t *buf, size_t buf_len) {
  JSValue obj, val;

  obj = JS_ReadObject(ctx, buf, buf_len, JS_READ_OBJ_BYTECODE);
  if (JS_IsException(obj))
    return obj;
  if (JS_VALUE_GET_TAG(obj) != JS_TAG_MODULE) {
    JS_FreeValue(ctx, obj);
    return JS_EXCEPTION;
  }
  assert(JS_VALUE_GET_TAG(obj) == JS_TAG_MODULE);

  if (JS_ResolveModule(ctx, obj) < 0) {
    if (CMAKE_BUILD_TYPE_IS_DEBUG)
      js_std_dump_error(ctx);
    JS_FreeValue(ctx, obj);
    return JS_EXCEPTION;
  }

  if (js_module_set_import_meta(ctx, obj, 0, 1) < 0) {
    if (CMAKE_BUILD_TYPE_IS_DEBUG)
      js_std_dump_error(ctx);
    JS_FreeValue(ctx, obj);
    return JS_EXCEPTION;
  }

  val = JS_EvalFunction(ctx, obj);
  val = js_std_await(ctx, val);

  if (JS_IsException(val)) {
    if (CMAKE_BUILD_TYPE_IS_DEBUG)
      js_std_dump_error(ctx);
    return val;
  }

  JS_FreeValue(ctx, val);

  JSModuleDef *m = JS_VALUE_GET_PTR(obj);
  JSValue n = JS_GetModuleNamespace(ctx, m);
  JS_FreeValue(ctx, obj);
  return n;
}

JSModuleDef *jsockd_js_module_loader(JSContext *ctx, const char *module_name,
                                     void *opaque, JSValueConst attributes) {
  JS_ThrowReferenceError(ctx, "JSockD doesn't allow module imports");
  return NULL;
}
