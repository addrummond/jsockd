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
    if (msg[i] == g_cmd_args.socket_sep_char)
      return i;
  }
  return len;
}

typedef enum {
  SEND_MESSAGE_ERR_TIMEOUT = -1,
  SEND_MESSAGE_ERR_EOF = -2,
  SEND_MESSAGE_ERR_BAD_JSON = -3,
  SEND_MESSAGE_ERR_TOO_BIG = -4,
  SEND_MESSAGE_ERR_INTERRUPTED = -5,
  SEND_MESSAGE_ERR_IO = -6,
  SEND_MESSAGE_ERR_TIME = -7,
  SEND_MESSAGE_ERR_BAD_MESSAGE = -8,
  SEND_MESSAGE_ERR_HANDLER_INTERNAL_ERROR = -9
} SendMessageError;

static const char *send_message_error_to_string(int err) {
  switch (err) {
  case SEND_MESSAGE_ERR_TIMEOUT:
    return "Timeout waiting for message response";
  case SEND_MESSAGE_ERR_EOF:
    return "EOF while reading message response";
  case SEND_MESSAGE_ERR_BAD_JSON:
    return "Bad JSON in message response";
  case SEND_MESSAGE_ERR_TOO_BIG:
    return "Message response too big";
  case SEND_MESSAGE_ERR_INTERRUPTED:
    return "Interrupted while waiting for message response";
  case SEND_MESSAGE_ERR_IO:
    return "I/O error while waiting for message response";
  case SEND_MESSAGE_ERR_TIME:
    return "Time error while waiting for message response";
  case SEND_MESSAGE_ERR_BAD_MESSAGE:
    return "Internal protocol error (no command id or mismatched command id)";
  case SEND_MESSAGE_ERR_HANDLER_INTERNAL_ERROR:
    return "Client indicated internal error in its message handler";
  default:
    return "<unknown error>";
  }
}

static int send_message(JSRuntime *rt, const char *message, size_t message_len,
                        JSValue *result) {
  const char term = '\n';
  ThreadState *ts = get_runtime_thread_state(rt);

  *result = JS_UNDEFINED;

  struct iovec msgvecs[] = {
      {.iov_base = (void *)ts->current_uuid, .iov_len = ts->current_uuid_len},
      {.iov_base = (void *)" message ", .iov_len = STRCONST_LEN(" message ")},
      {.iov_base = (void *)message, .iov_len = message_len},
      {.iov_base = (void *)&term, .iov_len = sizeof(char)},
  };
  if (writev_all(ts->socket_state->streamfd, msgvecs,
                 sizeof(msgvecs) / sizeof(msgvecs[0])) < 0) {
    jsockd_logf(LOG_ERROR, "Error writing message to socket: %s\n",
                strerror(errno));
    return SEND_MESSAGE_ERR_IO;
  }

  size_t total_read = 0;

  bool too_big = false;

  struct timespec polling_interval = {
      .tv_sec = g_cmd_args.max_command_runtime_us / 1000000ULL,
      .tv_nsec =
          MAX(1, (g_cmd_args.max_command_runtime_us % 1000000ULL) / 1000ULL)};

  for (;;) {
  read_loop:
    switch (ppoll_fd(ts->socket_state->streamfd, &polling_interval)) {
    case GO_AROUND:
      goto read_loop;
    case SIG_INTERRUPT_OR_ERROR:
      return SEND_MESSAGE_ERR_INTERRUPTED;
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
        jsockd_logf(
            LOG_ERROR,
            "Error reading from socket fd=%i in message handler (%i): %s\n",
            ts->socket_state->streamfd, r, strerror(errno));
        return SEND_MESSAGE_ERR_IO;
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
      return SEND_MESSAGE_ERR_TIME;
    }
    int64_t delta_ns = ns_time_diff(&now, &ts->last_js_execution_start);
    if (delta_ns > 0 &&
        (uint64_t)delta_ns > g_cmd_args.max_command_runtime_us) {
      jsockd_logf(LOG_WARN,
                  "Command runtime of %" PRIu64 "us exceeded %" PRIu64
                  "us while waiting for %.*s message response; interrupting\n",
                  delta_ns / 1000ULL, g_cmd_args.max_command_runtime_us,
                  (int)ts->current_uuid_len, ts->current_uuid);
      return SEND_MESSAGE_ERR_TIMEOUT;
    }
  }

read_done:
  if (too_big)
    return SEND_MESSAGE_ERR_TOO_BIG;
  if (total_read == 0)
    return SEND_MESSAGE_ERR_EOF;
  ts->input_buf[total_read] = '\0';

  size_t uuid_len = split_uuid(ts->input_buf, total_read);
  if (uuid_len == total_read) {
    jsockd_logf(
        LOG_DEBUG,
        "Error parsing message response, no UUID found: <<END\n%.*s\nEND\n",
        (int)total_read, ts->input_buf);
    return SEND_MESSAGE_ERR_BAD_MESSAGE;
  }

  if (0 != strncmp(ts->input_buf, ts->current_uuid, ts->current_uuid_len)) {
    jsockd_logf(
        LOG_DEBUG,
        "Error parsing message response, UUID mismatch (expected %.*s, got "
        "%.*s): <<END\n%.*s\nEND\n",
        (int)ts->current_uuid_len, ts->current_uuid, (int)uuid_len,
        ts->input_buf, (int)total_read, ts->input_buf);
    return SEND_MESSAGE_ERR_BAD_MESSAGE;
  }

  const char *json_input = ts->input_buf + uuid_len + 1;
  size_t json_input_len = total_read - uuid_len - 1 - 1; /*sep byte at end */

  if (0 == strcmp("internal_error", json_input)) {
    jsockd_log(
        LOG_DEBUG,
        "Received internal_error message response from message handler\n");
    return SEND_MESSAGE_ERR_HANDLER_INTERNAL_ERROR;
  }

  JSValue parsed =
      JS_ParseJSON(ts->ctx, json_input, json_input_len, "<message>");
  if (JS_IsException(parsed)) {
    JS_FreeValue(ts->ctx, parsed);
    dump_error(ts->ctx);
    jsockd_logf(
        LOG_DEBUG,
        "Error parsing JSON message response len %zu: <<END\n%.*s\nEND%s\n",
        json_input_len, MIN((int)json_input_len, 1024), json_input,
        json_input_len > 1024 ? "[truncated]" : "");
    return SEND_MESSAGE_ERR_BAD_JSON;
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
  if (JS_IsException(encoded_message_val)) {
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
  JS_FreeCString(ctx, message_str);
  if (r != 0) {
    JS_FreeValue(ctx, res);
    jsockd_logf(LOG_DEBUG, "Error sending message, error code=%i: %s\n", r,
                send_message_error_to_string(r));
    return JS_ThrowInternalError(ctx, "Error sending message via JSockD: %s",
                                 send_message_error_to_string(r));
  }

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
