#ifndef LINE_BUF_H
#define LINE_BUF_H

#include <stdbool.h>
#include <stdlib.h>

typedef struct {
  char *buf;   // buffer for the line
  size_t size; // size of the buffer
  // fields below can be zero initialized
  int start;
  int afsep;
  bool truncated;
  int n;
} LineBuf;

#define LINE_BUF_READ_EOF -99999

int line_buf_read(LineBuf *b, char sep_char,
                  int (*readf)(char *buf, size_t n, void *data),
                  void *readf_data,
                  int (*line_handler)(const char *line, size_t line_len,
                                      void *data, bool truncated),
                  void *line_handler_data);

int line_buf_replay(LineBuf *b, char sep_char,
                    int (*line_handler)(const char *line, size_t line_len,
                                        void *data, bool truncated),
                    void *line_handler_data);

#endif
