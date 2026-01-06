#include "cmdargs.h"
#include "config.h"
#include "hex.h"
#include <errno.h>
#include <libgen.h>
#include <stdbool.h>
#include <string.h>

const char EVAL_INPUT_STDIN_SENTINEL[1] = {'\0'};

static int n_flags_set(const CmdArgs *cmdargs) {
  return (cmdargs->es6_module_bytecode_file != NULL) +
         (cmdargs->source_map_file != NULL) + (cmdargs->n_sockets != 0) +
         (cmdargs->socket_sep_char_set == true) + (cmdargs->version == true) +
         (cmdargs->max_command_runtime_us != 0) +
         (int)(cmdargs->max_idle_time_set) +
         (cmdargs->key_file_prefix != NULL) +
         (cmdargs->mod_to_compile != NULL) +
         (cmdargs->compile_opts != COMPILE_OPTS_NONE) + (cmdargs->eval == true);
}

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
      char *endptr = NULL;
      long long int v = strtoll(argv[i], &endptr, 10);
      if (errno != 0 || !endptr || *endptr != '\0' || v <= 0) {
        errlog("Error: -t requires a valid integer argument > 0\n");
        return -1;
      }
      cmdargs->max_command_runtime_us = (uint64_t)v;
    } else if (0 == strcmp(argv[i], "-i")) {
      ++i;
      if (i >= argc) {
        errlog("Error: -i requires an argument (max thread idle time in "
               "microseconds)\n");
        return -1;
      }
      if (cmdargs->max_idle_time_set) {
        errlog("Error: -i can be specified at most once\n");
        return -1;
      }
      errno = 0;
      char *endptr = NULL;
      long long int v = strtoll(argv[i], &endptr, 10);
      if (errno != 0 || !endptr || *endptr != '\0' || v < 0) {
        errlog("Error: -i requires a valid integer argument >= 0\n");
        return -1;
      }
      cmdargs->max_idle_time_us = (uint64_t)v;
      cmdargs->max_idle_time_set = true;
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
        // If they specify more sockets than MAX_THREADS that's fine, as the
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
    } else if (0 == strcmp(argv[i], "-k")) {
      if (cmdargs->key_file_prefix != NULL) {
        errlog("Error: -k can be specified at most once\n");
        return -1;
      }
      ++i;
      if (i >= argc) {
        errlog("Error: -k requires an argument (key file)\n");
        return -1;
      }
      cmdargs->key_file_prefix = argv[i];
    } else if (0 == strcmp(argv[i], "-c")) {
      if (cmdargs->mod_to_compile != NULL) {
        errlog("Error: -c can be specified at most once\n");
        return -1;
      }
      ++i;
      if (i + 1 >= argc) {
        errlog("Error: -c requires two arguments (ES6 module to compile and "
               "output file)\n");
        return -1;
      }
      cmdargs->mod_to_compile = argv[i];
      ++i;
      cmdargs->mod_output_file = argv[i];
    } else if (0 == strcmp(argv[i], "-ss") || 0 == strcmp(argv[i], "-sd")) {
      if (cmdargs->compile_opts != COMPILE_OPTS_NONE) {
        errlog("Error: -ss and -sd are mutually exclusive and can be specified "
               "at most once\n");
        return -1;
      }
      cmdargs->compile_opts = (0 == strcmp(argv[i], "-ss"))
                                  ? COMPILE_OPTS_STRIP_SOURCE
                                  : COMPILE_OPTS_STRIP_DEBUG;
    } else if (0 == strcmp(argv[i], "-e")) {
      if (cmdargs->eval) {
        errlog("Error: -e can be specified at most once\n");
        return -1;
      }
      ++i;
      if (i >= argc) {
        errlog("Error: -e requires an argument (JavaScript code to evaluate, "
               "or '-' for stdin)\n");
        return -1;
      }
      cmdargs->eval = true;
      const char *arg = argv[i];
      if (0 == strcmp(arg, "-"))
        cmdargs->eval_input = EVAL_INPUT_STDIN_SENTINEL;
      else
        cmdargs->eval_input = argv[i];
    } else if (argv[i][0] == '-') {
      errlog("Error: unrecognized option: %s\n", argv[i]);
      return -1;
    } else {
      errlog("Error: unknown argument '%s'\n", argv[i]);
      return -1;
    }
  }

  int n_flags = n_flags_set(cmdargs);
  if (cmdargs->version && n_flags > 1) {
    errlog("Error: -v (version) cannot be used with other flags.\n");
    return -1;
  }
  if (cmdargs->eval) {
    int expected_count = 1 + (cmdargs->source_map_file != NULL) +
                         (cmdargs->es6_module_bytecode_file != NULL);
    if (n_flags != expected_count) {
      errlog("Error: -e (eval) can only be used with -m and -sm options\n");
      return -1;
    }
    if (cmdargs->source_map_file && !cmdargs->es6_module_bytecode_file) {
      errlog("Error: -e (eval) can only be used with -m and -sm options\n");
      return -1;
    }
  }

  if (cmdargs->key_file_prefix && !(n_flags == 1 || cmdargs->mod_to_compile)) {
    errlog("Error: -k (key file) option must be used either alone (to "
           "generate a key pair) or with the -c option.\n");
    return -1;
  }

  if (cmdargs->compile_opts != COMPILE_OPTS_NONE && !cmdargs->mod_to_compile) {
    errlog("Error: -ss or -sd flags must be used only with the -c option.\n");
    return -1;
  }

  if (cmdargs->mod_to_compile) {
    if (1 < n_flags - (cmdargs->key_file_prefix != 0) -
                (cmdargs->compile_opts != COMPILE_OPTS_NONE)) {
      errlog(
          "Error: -c (compile module) must be used only with -k (private key "
          "file) option and -ss or -sd flags.\n");
      return -1;
    }
  }

  if (cmdargs->version || cmdargs->key_file_prefix || cmdargs->mod_to_compile ||
      cmdargs->eval)
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
    const char *cmdname = argc > 0 ? basename(argv[0]) : "jsockd";
    errlog("Usage: %s [-m <module_bytecode_file>] [-sm <source_map_file>] [-b "
           "XX] [-t <max_command_runtime_us>] [-i <max_idle_time_us>] [-e <JS "
           "expression>] -s <socket1_path> [<socket2_path> ...]\n       %s -c "
           "<module_to_compile> <output_file> [-k <private_key_file>]\n       "
           "%s -k <key_file_prefix>\n",
           cmdname, cmdname, cmdname);
    return -1;
  }
  return 0;
}
