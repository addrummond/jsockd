#include "config.h"
#include "globals.h"
#include "log.h"
#include "quickjs.h"
#include "threadstate.h"
#include "utils.h"
#include <errno.h>
#include <inttypes.h>
#include <unistd.h>

static size_t split_uuid(const char *msg, size_t len) {
  for (size_t i = 0; i < len; i++) {
    if (msg[i] == ' ')
      return i;
  }
  return len;
}

typedef enum {
  RET_TIMEOUT = -1,
  RET_EOF = -2,
  RET_BAD_JSON = -3,
  RET_TOO_BIG = -4,
  RET_INTERRUPTED = -5,
  RET_IO = -6,
  RET_TIME = -7,
  RET_BAD_MESSAGE = -8,
} SendMessageError;

static const char *send_message_error_to_string(int err) {
  switch (err) {
  case RET_TIMEOUT:
    return "Timeout waiting for message response";
  case RET_EOF:
    return "EOF while reading message response";
  case RET_BAD_JSON:
    return "Bad JSON in message response";
  case RET_TOO_BIG:
    return "Message response too big";
  case RET_INTERRUPTED:
    return "Interrupted while waiting for message response";
  case RET_IO:
    return "I/O error while waiting for message response";
  case RET_TIME:
    return "Time error while waiting for message response";
  case RET_BAD_MESSAGE:
    return "Internal protocol error (no command id or mismatched command id)";
  default:
    return "<unknown error>";
  }
}

static int send_message(JSRuntime *rt, const char *message, size_t message_len,
                        JSValue *result) {
  const char term = '\n';
  ThreadState *ts = get_runtime_thread_state(rt);

  *result = JS_UNDEFINED;

  // TODO: Might be faster to concat all of these into a buffer to avoid
  // multiple syscalls.
  write_all(ts->socket_state->streamfd, ts->current_uuid, ts->current_uuid_len);
  write_all(ts->socket_state->streamfd, " message ",
            sizeof("message ") / sizeof(char));
  write_all(ts->socket_state->streamfd, message, message_len);
  write_all(ts->socket_state->streamfd, &term, sizeof(char));

  size_t total_read = 0;

  bool too_big = false;

  for (;;) {
  read_loop:
    switch (poll_fd(ts->socket_state->streamfd, 1)) {
    case GO_AROUND:
      goto read_loop;
    case SIG_INTERRUPT_OR_ERROR:
      return RET_INTERRUPTED;
    case READY: {
      if (total_read == INPUT_BUF_BYTES - 1) {
        too_big = true;
        total_read = 1; // make sure we don't confuse this condition with EOF
      }
      int r = read(ts->socket_state->streamfd, ts->input_buf + total_read,
                   INPUT_BUF_BYTES - 1 - total_read);
      if (r < 0 && errno == EINTR)
        continue;
      if (r <= 0) {
        jsockd_logf(LOG_ERROR,
                    "Error reading from socket in message handler (%i): %s\n",
                    r, strerror(errno));
        return RET_IO;
      }
      total_read += (size_t)r;
      if (total_read > 0 &&
          ts->input_buf[total_read - 1] == g_cmd_args.socket_sep_char)
        goto read_done;
    } break;
    }

    // check for timeout condition
    struct timespec now;
    if (0 != clock_gettime(MONOTONIC_CLOCK, &now)) {
      jsockd_log(LOG_ERROR,
                 "Error getting time in handle_line_3_parameter [2]\n");
      return RET_TIME;
    }
    int64_t delta_ns = ns_time_diff(&now, &ts->last_js_execution_start);
    if (delta_ns > 0 &&
        (uint64_t)delta_ns > g_cmd_args.max_command_runtime_us) {
      jsockd_logf(LOG_WARN,
                  "Command runtime of %" PRIu64 "us exceeded %" PRIu64
                  "us while waiting for %.*s message response; interrupting\n",
                  delta_ns / 1000ULL, g_cmd_args.max_command_runtime_us,
                  (int)ts->current_uuid_len, ts->current_uuid);
      return RET_TIMEOUT;
    }
  }

read_done:
  if (too_big)
    return RET_TOO_BIG;
  if (total_read == 0)
    return RET_EOF;
  ts->input_buf[total_read] = '\0';

  size_t uuid_len = split_uuid(ts->input_buf, total_read);
  if (uuid_len == total_read) {
    jsockd_logf(
        LOG_DEBUG,
        "Error parsing message response, no UUID found: <<END\n%.*s\nEND\n",
        (int)total_read, ts->input_buf);
    return RET_BAD_MESSAGE;
  }

  if (0 != strncmp(ts->input_buf, ts->current_uuid, ts->current_uuid_len)) {
    jsockd_logf(
        LOG_DEBUG,
        "Error parsing message response, UUID mismatch (expected %.*s, got "
        "%.*s): <<END\n%.*s\nEND\n",
        (int)ts->current_uuid_len, ts->current_uuid, (int)uuid_len,
        ts->input_buf, (int)total_read, ts->input_buf);
    return RET_BAD_MESSAGE;
  }

  JSValue parsed = JS_ParseJSON(ts->ctx, ts->input_buf + uuid_len + 1,
                                total_read - uuid_len - 1, "<message>");
  if (JS_IsException(parsed)) {
    JS_FreeValue(ts->ctx, parsed);
    jsockd_logf(LOG_DEBUG,
                "Error parsing JSON message response: <<END\n%.*s\nEND%s\n",
                MIN((int)total_read, 1024), ts->input_buf,
                total_read > 1024 ? "[truncated]" : "");
    return RET_BAD_JSON;
  }
  *result = parsed;
  return 0;
}

static void jsockd_finalizer(JSRuntime *rt, JSValue val) {
  jsockd_log(LOG_DEBUG, "Finalizing global JSockD object...\n");
}

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
  JSValue encoded_message_val =
      JS_JSONStringify(ctx, message_val, JS_UNDEFINED, JS_UNDEFINED);
  JS_FreeValue(ctx, message_val);
  if (JS_IsException(message_val)) {
    JS_FreeValue(ctx, encoded_message_val);
    return JS_ThrowTypeError(
        ctx, "JSockD.sendMessage argument must be JSON serializable");
  }

  JSValue res;
  size_t message_len;
  const char *message_str =
      JS_ToCStringLen(ctx, &message_len, encoded_message_val);
  JS_FreeValue(ctx, encoded_message_val);
  int r = send_message(JS_GetRuntime(ctx), message_str, message_len, &res);
  if (r != 0) {
    JS_FreeCString(ctx, message_str);
    JS_FreeValue(ctx, res);
    jsockd_logf(LOG_DEBUG, "Error sending message, error code=%i: %s\n", r,
                send_message_error_to_string(r));
    return JS_ThrowInternalError(ctx, "Error sending message via JSockD: %s",
                                 send_message_error_to_string(r));
  }

  JS_FreeCString(ctx, message_str);
  return res;
}

static const JSCFunctionListEntry jsockd_function_list[] = {
    JS_CFUNC_DEF("sendMessage", 1, jsockd_send_message),
};

static JSValue jsockd_ctor(JSContext *ctx, JSValueConst this_val, int argc,
                           JSValueConst *argv) {
  JS_NewClassID(&jsockd_class_id);

  return JS_NewObjectClass(ctx, jsockd_class_id);
}

int add_intrinsic_jsockd(JSContext *ctx, JSValueConst global) {
  JSValue ctor, proto;

  JS_NewClassID(&jsockd_class_id);

  if (JS_NewClass(JS_GetRuntime(ctx), jsockd_class_id, &jsockd_class) < 0) {
    return -1;
  }

  proto = JS_NewObject(ctx);
  if (JS_IsException(proto)) {
    JS_FreeValue(ctx, proto);
    return -1;
  }

  JS_SetClassProto(ctx, jsockd_class_id, proto);

  ctor = JS_NewCFunction2(ctx, jsockd_ctor, "TextDecoder", 2,
                          JS_CFUNC_constructor, 0);
  if (JS_IsException(ctor)) {
    JS_FreeValue(ctx, proto);
    JS_FreeValue(ctx, ctor);
    return -1;
  }

  JS_SetPropertyFunctionList(ctx, ctor, jsockd_function_list,
                             sizeof(jsockd_function_list) /
                                 sizeof(jsockd_function_list[0]));

  JS_SetConstructor(ctx, ctor, proto);

  int r = JS_SetPropertyStr(ctx, global, "JSockD", ctor);
  return r;
}
