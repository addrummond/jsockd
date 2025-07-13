#include "hex.h"

static uint8_t hex_digit(uint8_t c) {
  if (c >= '0' && c <= '9')
    return c - '0';
  if (c >= 'A' && c <= 'F')
    return c - 'A' + 10;
  if (c >= 'a' && c <= 'f')
    return c - 'a' + 10;
  return 0;
}

size_t hex_decode(uint8_t *buf, size_t buf_len, const char *input) {
  size_t i = 0;
  for (const char *cp = input; i < buf_len && *cp;) {
    buf[i] = hex_digit(*cp) << 4;
    ++cp;
    if (!*cp)
      return i + 1;
    buf[i] |= hex_digit(*cp);
    ++i;
    ++cp;
  }
  return i;
}
