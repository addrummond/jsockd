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

  size_t total_read;

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
