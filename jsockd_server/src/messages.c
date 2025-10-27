#include "config.h"
#include "globals.h"
#include "quickjs.h"
#include "threadstate.h"
#include "utils.h"
#include <errno.h>
#include <unistd.h>

static int send_message(JSRuntime *rt, const char *message, size_t message_len,
                        JSValue *result) {
  const char term = '\n';
  ThreadState *ts = (ThreadState *)JS_GetRuntimeOpaque(rt);

  *result = JS_UNDEFINED;

  write_all(ts->socket_state->streamfd, message, message_len);
  write_all(ts->socket_state->streamfd, &term, sizeof(char));

  size_t total_read = 0;

  for (;;) {
  read_loop:
    switch (poll_fd(ts->socket_state->streamfd, 1)) {
    case GO_AROUND:
      goto read_loop;
    case SIG_INTERRUPT_OR_ERROR:
      return -1;
    case READY: {
      if (total_read == INPUT_BUF_BYTES - 1) {
        // Message too big for input buffer
        // TODO ERROR HANDLING
        return -1;
      }
      int r = read(ts->socket_state->streamfd, ts->input_buf + total_read,
                   INPUT_BUF_BYTES - 1 - total_read);
      if (r < 0 && errno == EINTR)
        continue;
      if (r <= 0) {
        // TODO ERROR HANDLING
        return -1;
      }
      total_read += (size_t)r;
      if (total_read > 0 &&
          ts->input_buf[total_read - 1] == g_cmd_args.socket_sep_char)
        goto read_done;
    } break;
    }

    // TODO check for timeout condition
  }

read_done:
  if (total_read == 0) {
    // TODO ERROR HANDLING
    return -1;
  }
  ts->input_buf[total_read] = '\0';
  JSValue parsed =
      JS_ParseJSON(ts->ctx, ts->input_buf, total_read, "<message>");
  if (JS_IsException(parsed)) {
    JS_FreeValue(ts->ctx, parsed);
    // TODO error logging
    return -1;
  }
  *result = parsed;
  return 0;
}

static void jsockd_finalizer(JSRuntime *rt, JSValue val) {}

static JSClassDef jsockd_class = {
    "TextDecoder",
    .finalizer = jsockd_finalizer,
};

static JSClassID jsockd_class_id;

static JSValue jsockd_send_message(JSContext *cx, JSValueConst this_val,
                                   int argc, JSValueConst *argv) {

  if (argc != 1) {
    return JS_ThrowInternalError(
        cx,
        "JSockD.sendMessage requires exactly 1 argument (the message to send)");
  }

  // TODO
  return JS_UNDEFINED;
}

static const JSCFunctionListEntry jsockd_function_list[] = {
    JS_CFUNC_DEF("sendMessage", 1, jsockd_send_message),
};

static JSValue jsockd_ctor(JSContext *cx, JSValueConst this_val, int argc,
                           JSValueConst *argv) {
  JSValue obj;

  JS_NewClassID(&jsockd_class_id);

  obj = JS_NewObjectClass(cx, jsockd_class_id);
  if (JS_IsException(obj)) {
    return JS_EXCEPTION;
  }

  return obj;
}

int add_intrinsic_jsockd(JSContext *ctx, JSValueConst global) {
  JSValue ctor, proto;

  JS_NewClassID(&jsockd_class_id);

  if (JS_NewClass(JS_GetRuntime(ctx), jsockd_class_id, &jsockd_class) < 0) {
    return -1;
  }

  proto = JS_NewObject(ctx);
  if (JS_IsException(proto)) {
    return -1;
  }

  JS_SetClassProto(ctx, jsockd_class_id, proto);

  ctor = JS_NewCFunction2(ctx, jsockd_ctor, "TextDecoder", 2,
                          JS_CFUNC_constructor, 0);
  if (JS_IsException(ctor)) {
    return -1;
  }

  JS_SetPropertyFunctionList(ctx, ctor, jsockd_function_list,
                             sizeof(jsockd_function_list) /
                                 sizeof(jsockd_function_list[0]));

  JS_SetConstructor(ctx, ctor, proto);

  int r = JS_SetPropertyStr(ctx, global, "JSockD", ctor);
  return r;
}
