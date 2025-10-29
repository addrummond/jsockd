#include "config.h"
#include "globals.h"
#include "log.h"
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
        return -2;
      }
      int r = read(ts->socket_state->streamfd, ts->input_buf + total_read,
                   INPUT_BUF_BYTES - 1 - total_read);
      if (r < 0 && errno == EINTR)
        continue;
      if (r <= 0) {
        jsockd_logf(LOG_ERROR,
                    "Error reading from socket in message handler (%i): %s\n",
                    r, strerror(errno));
        // TODO ERROR HANDLING
        return -3;
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
    return -4;
  }
  ts->input_buf[total_read] = '\0';
  JSValue parsed =
      JS_ParseJSON(ts->ctx, ts->input_buf, total_read, "<message>");
  if (JS_IsException(parsed)) {
    JS_FreeValue(ts->ctx, parsed);
    jsockd_logf(LOG_DEBUG,
                "Error parsing JSON message response: <<END\n%.*s\nEND%s\n",
                MIN((int)total_read, 1024), ts->input_buf,
                total_read > 1024 ? "[truncated]" : "");
    // TODO error logging
    return -5;
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

static JSValue jsockd_send_message(JSContext *ctx, JSValueConst this_val,
                                   int argc, JSValueConst *argv) {

  if (argc != 1) {
    return JS_ThrowInternalError(
        ctx,
        "JSockD.sendMessage requires exactly 1 argument (the message to send)");
  }
  JSValue message_val = argv[0];
  if (!JS_IsString(message_val)) {
    return JS_ThrowTypeError(ctx,
                             "JSockD.sendMessage argument must be a string");
  }

  JSValue res;
  size_t message_len;
  const char *message_str = JS_ToCStringLen(ctx, &message_len, message_val);
  int r = send_message(JS_GetRuntime(ctx), message_str, message_len, &res);
  if (r != 0) {
    JS_FreeCString(ctx, message_str);
    JS_FreeValue(ctx, res);
    jsockd_logf(LOG_DEBUG, "Error sending message, error code=%i\n", r);
    return JS_ThrowInternalError(ctx, "Error sending message via JSockD");
  }

  return res;
}

static const JSCFunctionListEntry jsockd_function_list[] = {
    JS_CFUNC_DEF("sendMessage", 1, jsockd_send_message),
};

static JSValue jsockd_ctor(JSContext *ctx, JSValueConst this_val, int argc,
                           JSValueConst *argv) {
  JSValue obj;

  JS_NewClassID(&jsockd_class_id);

  obj = JS_NewObjectClass(ctx, jsockd_class_id);
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
