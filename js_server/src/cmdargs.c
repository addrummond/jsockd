#include "cmdargs.h"
#include "config.h"
#include "hex.h"
#include <errno.h>
#include <libgen.h>
#include <string.h>

static int parse_cmd_args_helper(int argc, char **argv,
                                 void (*errlog)(const char *fmt, ...),
                                 CmdArgs *cmdargs) {
  memset(cmdargs, 0, sizeof(*cmdargs));
  cmdargs->socket_sep_char = '\n';
  if (argc < 1)
    return -1;
  for (int i = 1; i < argc; ++i) {
    if (0 == strcmp(argv[i], "-v")) {
      cmdargs->version = true;
    } else if (0 == strcmp(argv[i], "-m")) {
      ++i;
      if (i >= argc) {
        errlog("Error: -m requires an argument (ES6 module bytecode file)\n");
        return -1;
      }
      if (cmdargs->es6_module_bytecode_file) {
        errlog("Error: -m can be specified at most once\n");
        return -1;
      }
      cmdargs->es6_module_bytecode_file = argv[i];
    } else if (0 == strcmp(argv[i], "-t")) {
      ++i;
      if (i >= argc) {
        errlog("Error: -t requires an argument (max command runtime in "
               "microseconds)\n");
        return -1;
      }
      if (cmdargs->max_command_runtime_us != 0) {
        errlog("Error: -t can be specified at most once\n");
        return -1;
      }
      errno = 0;
      long long int v = strtoll(argv[i], NULL, 10);
      if (errno != 0 || v <= 0) {
        errlog("Error: -t requires a valid integer argument > 0\n");
        return -1;
      }
      cmdargs->max_command_runtime_us = (uint64_t)v;
    } else if (0 == strcmp(argv[i], "-s")) {
      ++i;
      bool after_double_dash = false;
      int n_sockets_added = 0;
      for (; i < argc; ++i) {
        if (0 == strcmp(argv[i], "--")) {
          after_double_dash = true;
          continue;
        }
        if (!after_double_dash && argv[i][0] == '-') {
          break;
        }
        // If they specificy more sockets than MAX_THREADS that's fine, as the
        // `READ n` output of the server will inform the client how many of the
        // sockets are actually in use.
        if (cmdargs->n_sockets < (int)(sizeof(cmdargs->socket_path) /
                                       sizeof(cmdargs->socket_path[0]))) {
          cmdargs->socket_path[cmdargs->n_sockets] = argv[i];
          cmdargs->n_sockets++;
        }
        ++n_sockets_added;
      }
      --i;
      if (n_sockets_added == 0) {
        errlog("Error: -s requires at least one argument (socket file)\n");
        return -1;
      }
    } else if (0 == strcmp(argv[i], "-sm")) {
      ++i;
      if (i >= argc) {
        errlog("Error: -sm requires an argument (source map file, e.g. "
               "'foo.js.map')\n");
        return -1;
      }
      if (cmdargs->source_map_file) {
        errlog("Error: -sm can be specified at most once\n");
        return -1;
      }
      cmdargs->source_map_file = argv[i];
    } else if (0 == strcmp(argv[i], "-b")) {
      if (cmdargs->socket_sep_char_set) {
        errlog("Error: -b can be specified at most once\n");
        return -1;
      }
      ++i;
      if (i >= argc) {
        errlog("Error: -b requires an argument (two hex digits giving "
               "separator byte)\n");
        return -1;
      }
      if (strlen(argv[i]) != 2 || -1 == hex_digit(argv[i][0]) ||
          -1 == hex_digit(argv[i][1])) {
        errlog("Error: -b requires an argument of exactly two hex digits "
               "(e.g. '0A')\n");
        return -1;
      }
      hex_decode((uint8_t *)&cmdargs->socket_sep_char,
                 sizeof(cmdargs->socket_sep_char), argv[i]);
      cmdargs->socket_sep_char_set = true;
    } else if (argv[i][0] == '-') {
      errlog("Error: unrecognized option: %s\n", argv[i]);
      return -1;
    } else {
      errlog("Error: unknown argument '%s'\n", argv[i]);
      return -1;
    }
  }

  if (cmdargs->version &&
      (cmdargs->n_sockets > 0 || cmdargs->socket_sep_char_set ||
       cmdargs->es6_module_bytecode_file || cmdargs->source_map_file ||
       cmdargs->max_command_runtime_us != 0)) {
    errlog("Error: -v (version) cannot be used with other flags.\n");
    return -1;
  }

  if (cmdargs->version)
    return 0;

  if (cmdargs->n_sockets == 0) {
    errlog("No sockets specified.\n");
    return -1;
  }

  if (cmdargs->source_map_file && !cmdargs->es6_module_bytecode_file) {
    errlog("Error: -sm (source map file) can only be used with -m (ES6 module "
           "bytecode file)\n");
    return -1;
  }

  if (cmdargs->max_command_runtime_us == 0)
    cmdargs->max_command_runtime_us = DEFAULT_MAX_COMMAND_RUNTIME_US;

  return 0;
}

int parse_cmd_args(int argc, char **argv, void (*errlog)(const char *fmt, ...),
                   CmdArgs *cmdargs) {
  if (parse_cmd_args_helper(argc, argv, errlog, cmdargs) < 0) {
    errlog("Usage: %s [-m <module_bytecode_file>] [-sm <source_map_file>] [-b "
           "XX] [-t <max_command_runtime_us>] -s <socket1_path> "
           "[<socket2_path> ...] \n",
           argc > 0 ? basename(argv[0]) : "js_server");
    return -1;
  }
  return 0;
}
