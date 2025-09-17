#ifndef CONSOLE_H_
#define CONSOLE_H_

#include "quickjs.h"

JSValue my_js_console_log(JSContext *ctx, JSValueConst this_val, int argc,
                          JSValueConst *argv);

#endif
