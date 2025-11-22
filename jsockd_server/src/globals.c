#include "cmdargs.h"
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
atomic_int g_n_ready_threads = 0;

const uint8_t *g_source_map = NULL;
size_t g_source_map_size = 0;

atomic_int g_source_map_load_count = 0;

atomic_bool g_interrupted_or_error = 0;

CmdArgs g_cmd_args;

// Global vars that need destruction before exit.
WaitGroup g_thread_ready_wait_group;
pthread_mutex_t g_cached_functions_mutex;

const char *g_log_prefix = NULL;
