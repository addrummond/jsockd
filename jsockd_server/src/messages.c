#include "config.h"
#include "globals.h"
#include "quickjs.h"
#include "threadstate.h"
#include "utils.h"
#include <errno.h>
#include <unistd.h>

static int send_message(JSRuntime *rt, const char *message,
                        size_t message_len) {
  const char term = '\n';
  ThreadState *ts = (ThreadState *)JS_GetRuntimeOpaque(rt);
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
      if (total_read == INPUT_BUF_BYTES) {
        // Message too big for input buffer
        return -1;
      }
      int r = read(ts->socket_state->streamfd, ts->input_buf + total_read,
                   INPUT_BUF_BYTES - total_read);
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
  return (int)total_read;
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

static const JSCFunctionListEntry jsockd_proto[] = {
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

int add_intrinsic_jsockd(JSContext *cx, JSValueConst global) {
  JSValue ctor, proto;

  JS_NewClassID(&jsockd_class_id);

  if (JS_NewClass(JS_GetRuntime(cx), jsockd_class_id, &jsockd_class) < 0) {
    return -1;
  }

  proto = JS_NewObject(cx);
  if (JS_IsException(proto)) {
    return -1;
  }

  JS_SetPropertyFunctionList(cx, proto, jsockd_proto,
                             sizeof(jsockd_proto) / sizeof(jsockd_proto[0]));

  JS_SetClassProto(cx, jsockd_class_id, proto);

  ctor = JS_NewCFunction2(cx, jsockd_ctor, "TextDecoder", 2,
                          JS_CFUNC_constructor, 0);
  if (JS_IsException(ctor)) {
    return -1;
  }

  JS_SetConstructor(cx, ctor, proto);

  return JS_SetPropertyStr(cx, global, "TextDecoder", ctor);
}
