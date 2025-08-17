#include "line_buf.h"
#include <memory.h>

int line_buf_read(LineBuf *b, char sep_char,
                  int (*readf)(char *buf, size_t n, void *data),
                  void *readf_data,
                  int (*line_handler)(const char *line, size_t line_len,
                                      void *data, bool truncated),
                  void *line_handler_data) {
  size_t to_read = b->size - b->start;

  if (to_read == 0) {
    b->start = 0;
    to_read = b->size;
    b->truncated = true;
  }

  int n = readf(b->buf + b->start, to_read, readf_data);
  if (n == 0)
    return LINE_BUF_READ_EOF;
  if (n < 0)
    return n;

  int i;
  int sep = -1;
  for (i = b->start; i < b->start + n; ++i) {
    if (b->buf[i] == sep_char) {
      b->buf[i] = '\0';
      // Note: the parens around (sep + 1) are necessary to avoid (temporarily)
      // decrementing b->buf to the position before its first byte, invoking the
      // spectre of undefined behavior (consider the case where sep == -1).
      int lh_r = line_handler(b->buf + (sep + 1), i - sep - 1,
                              line_handler_data, b->truncated);
      sep = i;
      b->truncated = false;
      if (lh_r < 0)
        return lh_r;
    }
  }

  memcpy(b->buf, b->buf + sep + 1, i - sep - 1);
  b->start = i - sep - 1;

  return n;
}
