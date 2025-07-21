#include "writejsonstring.h"

#include <stdio.h>
#include <unistd.h>

int write_json_string(int fd_, const char *raw, size_t size) {
  // We need to call fclose to clean up the FILE* properly, but we don't want to
  // close the underlying UNIX file descriptor. We can work around this problem
  // by first duplicating the file descriptor and then using fdopen to create a
  // FILE* that we can fclose without closing the original file descriptor.
  int fd = dup(fd_);
  if (fd < 0)
    return fd;

  FILE *fp = fdopen(fd, "w");
  if (!fp)
    return -1;

  int r;

  r = fprintf(fp, "\"");
  if (r < 0) {
    fclose(fp);
    return r;
  }

  for (size_t i = 0; i < size; ++i) {
    char c = raw[i];

    switch (c) {
    case '"':
      r = fprintf(fp, "\\\"");
      break;
    case '\n':
      r = fprintf(fp, "\\n");
      break;
    case '\r':
      r = fprintf(fp, "\\r");
      break;
    case '\t':
      r = fprintf(fp, "\\t");
      break;
    case '\b':
      r = fprintf(fp, "\\b");
      break;
    case '\f':
      r = fprintf(fp, "\\f");
      break;
    default:
      r = fprintf(fp, "%c", c);
      break;
    }

    if (r < 0) {
      fclose(fp);
      return r;
    }
  }

  r = fprintf(fp, "\"");
  if (r < 0) {
    fclose(fp);
    return r;
  }

  return fclose(fp);
}
