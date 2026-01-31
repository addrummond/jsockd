#include "cmdargs.h"
#include "config.h"
#include "threadstate.h"
#include "wait_group.h"
#include <stdatomic.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

char *g_thread_state_input_buffers[MAX_THREADS];
ThreadState *g_thread_states = NULL;

atomic_int g_sig_triggered = 0;

const uint8_t *g_module_bytecode = NULL;
size_t g_module_bytecode_size = 0;

atomic_int g_n_threads = 0;

const uint8_t *g_source_map = NULL;
size_t g_source_map_size = 0;

atomic_int g_source_map_load_count = 0;

atomic_bool g_interrupted_or_error = false;

CmdArgs g_cmd_args;

WaitGroup g_thread_ready_wait_group;

const char *g_log_prefix = NULL;

bool g_interactive_logging_mode = false;

#ifdef CMAKE_BUILD_TYPE_DEBUG
int g_debug_hash_bits = CACHED_FUNCTIONS_HASH_BITS_DEBUG;
#endif
