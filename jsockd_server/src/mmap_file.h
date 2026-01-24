#ifndef MMAP_FILE_H
#define MMAP_FILE_H

#include <stdint.h>
#include <stdlib.h>

const uint8_t *mmap_file(const char *filename, size_t *out_size,
                         int *out_errno);

#endif
