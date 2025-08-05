#ifndef LINE_BUF_H
#define LINE_BUF_H

#ifndef _REENTRANT
#define _REENTRANT
#endif

#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>

typedef struct {
  char *buf;   // buffer for the line
  size_t size; // size of the buffer
  int start;
  bool truncated;
} LineBuf;

int line_buf_read(LineBuf *b, char sep_char,
                  int (*readf)(char *buf, size_t n, void *data),
                  void *readf_data,
                  int (*line_handler)(const char *line, size_t line_len,
                                      void *data, bool truncated),
                  void *line_handler_data);

#endif
