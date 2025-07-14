#include "line_buf.h"
#include <assert.h>
#include <memory.h>
#include <stdbool.h>
#include <stdio.h>

int line_buf_read(LineBuf *b, char sep_char,
                  int (*readf)(char *buf, size_t n, void *data),
                  void *readf_data,
                  int (*line_handler)(const char *line, size_t line_len,
                                      void *data),
                  void *line_handler_data, const char *truncation_append) {
  assert(sep_char == '\0' || !strchr(truncation_append, sep_char));
  assert(strlen(truncation_append) <= b->size - 1);
  assert((long long int)b->size >= (long long int)b->start);

  size_t to_read = b->size - b->start;

  if (to_read == 0) {
    // No space left in the buffer. Truncate.
    strncpy(b->buf, truncation_append, b->size - 1);
    b->start = strlen(truncation_append);
    to_read = b->size - b->start;
  }

  int n = readf(b->buf + b->start, to_read, readf_data);
  if (n <= 0) // 0 is EOF
    return n;

  int i;
  int sep = -1;
  for (i = b->start; i < b->start + n; ++i) {
    if (b->buf[i] == sep_char) {
      b->buf[i] = '\0';
      // Note: the parens around (sep + 1) are necessary to avoid (temporarily)
      // decrementing b->buf to the position before its first byte, invoking the
      // spectre of undefined behavior (consider the case where sep == -1).
      int lh_r =
          line_handler(b->buf + (sep + 1), i - sep - 1, line_handler_data);
      sep = i;
      if (lh_r < 0)
        return lh_r;
    }
  }

  if (i == (int)b->size) {
    strcpy(b->buf, truncation_append);
    b->start = strlen(truncation_append);
  } else if (sep < i - 1) {
    memcpy(b->buf, b->buf + sep + 1, i - sep - 1);
    b->start = i - sep - 1;
  } else {
    b->start = 0;
  }

  return n;
}
