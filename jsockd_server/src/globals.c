#include "cmdargs.h"
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>

atomic_int g_sig_triggered = 0;

const uint8_t *g_module_bytecode = NULL;
size_t g_module_bytecode_size = 0;

atomic_int g_n_threads = 0;
atomic_int g_n_ready_threads = 0;

const uint8_t *g_source_map = NULL;
size_t g_source_map_size = 0;

atomic_int g_source_map_load_count = 0;

atomic_bool g_interrupted_or_error = 0;

CmdArgs g_cmd_args;
