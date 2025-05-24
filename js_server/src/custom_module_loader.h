#ifndef CUSTOM_MODULE_LOADER_H_
#define CUSTOM_MODULE_LOADER_H_

#include "quickjs.h"

JSValue load_binary_module(JSContext *ctx, const uint8_t *buf, size_t buf_len);
JSModuleDef *jsockd_js_module_loader(JSContext *ctx, const char *module_name,
                                     void *opaque, JSValueConst attributes);

#endif
