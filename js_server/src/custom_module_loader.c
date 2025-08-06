#ifndef _REENTRANT
#define _REENTRANT
#endif

#include "custom_module_loader.h"
#include "quickjs-libc.h"
#include "quickjs.h"
#include <assert.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// Adapted from 'js_module_loader' and 'js_std_eval_binary' in quickjs-libc.c
JSValue load_binary_module(JSContext *ctx, const uint8_t *buf, size_t buf_len) {
  JSValue obj, val;

  obj = JS_ReadObject(ctx, buf, buf_len, JS_READ_OBJ_BYTECODE);
  if (JS_IsException(obj))
    return obj;
  assert(JS_VALUE_GET_TAG(obj) == JS_TAG_MODULE);

  if (JS_ResolveModule(ctx, obj) < 0) {
    JS_FreeValue(ctx, obj);
    return JS_EXCEPTION;
  }

  js_module_set_import_meta(ctx, obj, 0, 0);

  val = JS_EvalFunction(ctx, obj);

  if (JS_IsException(val))
    return val;

  val = js_std_await(ctx, val);
  if (JS_IsException(val))
    return val;

  JS_FreeValue(ctx, val);
  return JS_GetModuleNamespace(ctx, JS_VALUE_GET_PTR(obj));
}

JSModuleDef *jsockd_js_module_loader(JSContext *ctx, const char *module_name,
                                     void *opaque, JSValueConst attributes) {
  JS_ThrowReferenceError(ctx, "JSockD doesn't allow module imports");
  return NULL;
}
