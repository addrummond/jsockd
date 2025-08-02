#ifndef CMDARGS_H_
#define CMDARGS_H_

#include "config.h"
#include <stdbool.h>
#include <stdio.h>

typedef struct {
  const char *es6_module_bytecode_file;
  const char *socket_path[MAX_THREADS];
  const char *source_map_file;
  int n_sockets;
  char socket_sep_char;
  bool socket_sep_char_set;
  bool version;
} CmdArgs;

int parse_cmd_args(int argc, char **argv, void (*errlog)(const char *fmt, ...),
                   CmdArgs *cmdargs);

#endif
