#ifndef BACKTRACE_H
#define BACKTRACE_H

#include "threadstate.h"

typedef enum { BACKTRACE_JSON, BACKTRACE_PRETTY } BacktraceFormat;

const char *get_backtrace(ThreadState *ts, const char *backtrace,
                          size_t backtrace_length,
                          size_t *out_json_backtrace_length,
                          BacktraceFormat backtrace_format);

#endif
