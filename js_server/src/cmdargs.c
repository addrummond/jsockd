#include "cmdargs.h"
#include "hex.h"
#include <libgen.h>
#include <string.h>

#define print_usage(prog_name)                                                 \
  errlog("Usage: %s -s socket_path [-s socket_path ...] [-m "                  \
         "module_bytecode_file] [-b XX]\n",                                    \
         basename((prog_name)))

int parse_cmd_args(int argc, char **argv, void (*errlog)(const char *fmt, ...),
                   CmdArgs *cmdargs) {
  memset(cmdargs, 0, sizeof(*cmdargs));
  cmdargs->socket_sep_char = '\n';
  if (argc < 1) {
    print_usage("js_server");
    return -1;
  }
  for (int i = 1; i < argc; ++i) {
    if (0 == strcmp(argv[i], "-m")) {
      ++i;
      if (i >= argc) {
        errlog("Error: -m requires an argument (ES6 module bytecode file)\n");
        print_usage(argv[0]);
      }
      if (cmdargs->es6_module_bytecode_file) {
        errlog("Error: -m can be specified at most once\n");
        print_usage(argv[0]);
        return -1;
      }
      cmdargs->es6_module_bytecode_file = argv[i];
    } else if (0 == strcmp(argv[i], "-s")) {
      ++i;
      if (i >= argc) {
        errlog("Error: -s requires an argument (socket file)\n");
        print_usage(argv[0]);
        return -1;
      }
      cmdargs->n_sockets++;
      cmdargs->socket_path[cmdargs->n_sockets - 1] = argv[i];
    } else if (0 == strcmp(argv[i], "-b")) {
      if (cmdargs->socket_sep_char_set) {
        errlog("Error: -b can be specified at most once\n");
        print_usage(argv[0]);
        return -1;
      }
      ++i;
      if (i >= argc) {
        errlog("Error: -b requires an argument (two hex digits giving "
               "separator byte)\n");
        print_usage(argv[0]);
        return -1;
      }
      if (strlen(argv[i]) != 2 || -1 == hex_digit(argv[i][0]) ||
          -1 == hex_digit(argv[i][1])) {
        errlog("Error: -b requires an argument of exactly two hex digits "
               "(e.g. '0A')\n");
        print_usage(argv[0]);
        return -1;
      }
      hex_decode((uint8_t *)&cmdargs->socket_sep_char, sizeof(char), argv[i]);
      cmdargs->socket_sep_char_set = true;
    } else {
      errlog("Error: unknown argument '%s'\n", argv[i]);
      print_usage(argv[0]);
      return -1;
    }
  }

  if (cmdargs->n_sockets == 0) {
    errlog("No sockets specified.\n");
    return -1;
  }

  return 0;
}
