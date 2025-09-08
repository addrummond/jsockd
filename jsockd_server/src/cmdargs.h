#ifndef CMDARGS_H_
#define CMDARGS_H_

#ifndef _REENTRANT
#define _REENTRANT
#endif

#include "config.h"
#include <stdbool.h>
#include <stdint.h>

typedef struct {
  const char *es6_module_bytecode_file;
  const char *socket_path[MAX_THREADS];
  const char *source_map_file;
  int n_sockets;
  char socket_sep_char;
  bool socket_sep_char_set;
  bool version;
  uint64_t max_command_runtime_us;
  uint64_t max_idle_time_us;
  bool max_idle_time_set;
  const char *key_file_prefix;
  const char *mod_to_compile;
  const char *mod_output_file;
} CmdArgs;

int parse_cmd_args(int argc, char **argv, void (*errlog)(const char *fmt, ...),
                   CmdArgs *cmdargs);

#endif
