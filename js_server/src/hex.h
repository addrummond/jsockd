#ifndef HEX_H_
#define HEX_H_

#include <stdint.h>
#include <stdlib.h>

int8_t hex_digit(uint8_t c);
size_t hex_decode(uint8_t *buf, size_t buf_len, const char *input);

#endif
