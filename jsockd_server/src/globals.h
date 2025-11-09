#ifndef GLOBALS_H_
#define GLOBALS_H_

#include "cmdargs.h"
#include "threadstate.h"
#include "wait_group.h"
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>

extern char *g_thread_state_input_buffers[MAX_THREADS];
extern ThreadState g_thread_states[MAX_THREADS];

extern const uint32_t g_backtrace_module_bytecode_size;
extern const uint8_t g_backtrace_module_bytecode[];

extern const uint32_t g_shims_module_bytecode_size;
extern const uint8_t g_shims_module_bytecode[];

extern atomic_int g_sig_triggered;

extern const uint8_t *g_module_bytecode;
extern size_t g_module_bytecode_size;

extern atomic_int g_n_threads;
extern atomic_int g_n_ready_threads;

extern const uint8_t *g_source_map;
extern size_t g_source_map_size;

extern atomic_int g_source_map_load_count;

extern atomic_bool g_interrupted_or_error;

extern CmdArgs g_cmd_args;

extern WaitGroup g_thread_ready_wait_group;
extern pthread_mutex_t g_cached_functions_mutex;

#endif
