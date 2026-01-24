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

static const char *allowed_modules[] = {"os", "std"};

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
    JS_FreeValue(ctx, obj);
    return JS_EXCEPTION;
  }

  if (js_module_set_import_meta(ctx, obj, 0, 1) < 0) {
    JS_FreeValue(ctx, obj);
    return JS_EXCEPTION;
  }

  val = JS_EvalFunction(ctx, obj);
  val = js_std_await(ctx, val);

  if (JS_IsException(val))
    return val;

  JS_FreeValue(ctx, val);

  JSModuleDef *m = JS_VALUE_GET_PTR(obj);
  return JS_GetModuleNamespace(ctx, m);
}

JSModuleDef *jsockd_js_module_loader(JSContext *ctx, const char *module_name,
                                     void *opaque, JSValueConst attributes) {
  for (size_t i = 0; i < sizeof(allowed_modules) / sizeof(allowed_modules[0]);
       ++i) {
    if (!strcmp(module_name, allowed_modules[i]))
      return js_module_loader(ctx, module_name, opaque, attributes);
  }
  JS_ThrowReferenceError(
      ctx, "JSockD doesn't allow module imports other than 'os' and 'std'");
  return NULL;
}
