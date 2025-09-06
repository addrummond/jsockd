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

  b->n = readf(b->buf + b->start, to_read, readf_data);
  if (b->n == 0)
    return LINE_BUF_READ_EOF;
  if (b->n < 0)
    return b->n;

  return line_buf_replay(b, sep_char, line_handler, line_handler_data);
}

int line_buf_replay(LineBuf *b, char sep_char,
                    int (*line_handler)(const char *line, size_t line_len,
                                        void *data, bool truncated),
                    void *line_handler_data) {
  int i;
  for (i = b->start; i < b->start + b->n; ++i) {
    if (b->buf[i] == sep_char) {
      b->buf[i] = '\0';
      int lh_r = line_handler(b->buf + b->afsep, i - b->afsep,
                              line_handler_data, b->truncated);
      b->truncated = false;
      if (lh_r < 0) {
        b->buf[i] = sep_char;
        b->start = b->afsep;
        return lh_r;
      }
      b->afsep = i + 1;
    }
  }

  memmove(b->buf, b->buf + b->afsep, i - b->afsep);
  b->start = i - b->afsep;
  b->afsep = 0;

  return b->n;
}
