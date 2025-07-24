#ifndef CMDARGS_H_
#define CMDARGS_H_

#include "config.h"
#include <stdbool.h>

typedef struct {
  const char *es6_module_bytecode_file;
  const char *socket_path[MAX_THREADS];
  int n_sockets;
  char socket_sep_char;
  bool socket_sep_char_set;
} CmdArgs;

int parse_cmd_args(int argc, char **argv, CmdArgs *cmdargs);

#endif
