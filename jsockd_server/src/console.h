#ifndef CONSOLE_H_
#define CONSOLE_H_

#include "quickjs.h"

JSValue my_js_console_log(JSContext *ctx, JSValueConst this_val, int argc,
                          JSValueConst *argv);
JSValue my_js_console_warn(JSContext *ctx, JSValueConst this_val, int argc,
                           JSValueConst *argv);
JSValue my_js_console_info(JSContext *ctx, JSValueConst this_val, int argc,
                           JSValueConst *argv);
JSValue my_js_console_error(JSContext *ctx, JSValueConst this_val, int argc,
                            JSValueConst *argv);
JSValue my_js_console_debug(JSContext *ctx, JSValueConst this_val, int argc,
                            JSValueConst *argv);
JSValue my_js_console_trace(JSContext *ctx, JSValueConst this_val, int argc,
                            JSValueConst *argv);

#endif
