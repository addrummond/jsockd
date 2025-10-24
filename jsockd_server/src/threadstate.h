#ifndef THREADSTATE_H_
#define THREADSTATE_H_

#include <stdbool.h>
#include <pthread.h>
#include "quickjs.h"
#include <stdatomic.h>
#include <sys/socket.h>
#include <sys/un.h>
#include "config.h"

typedef struct {
  const uint8_t *bytecode;
  size_t bytecode_size;
  int refcount;
} cached_function_t;

// values for ThreadState.replacement_thread_state
enum {
  REPLACEMENT_THREAD_STATE_NONE,
  REPLACEMENT_THREAD_STATE_INIT,
  REPLACEMENT_THREAD_STATE_INIT_COMPLETE,
  REPLACEMENT_THREAD_STATE_CLEANUP,
  REPLACEMENT_THREAD_STATE_CLEANUP_COMPLETE
};

typedef struct {
  const char *unix_socket_filename;
  int sockfd;
  int streamfd;
  int stream_io_err;
  struct sockaddr_un addr;
} SocketState;

// The state for each thread which runs a QuickJS VM.
typedef struct ThreadState {
  int thread_index;
  SocketState *socket_state;
  JSRuntime *rt;
  JSContext *ctx;
  int exit_status;
  int line_n;
  JSValue compiled_module;
  JSValue compiled_query;
  JSValue backtrace_module;
  struct timespec last_js_execution_start;
  char current_uuid[MESSAGE_UUID_MAX_BYTES + 1 /*zeroterm*/];
  size_t current_uuid_len;
  int memory_check_count;
  int memory_increase_count;
  int64_t last_memory_usage;
  int last_n_cached_functions;
  bool truncated;
  JSValue sourcemap_str;
  int64_t last_command_exec_time_ns;
  struct ThreadState *my_replacement;
  atomic_int replacement_thread_state;
  pthread_t replacement_thread;
  struct timespec last_active_time;
  uint8_t *dangling_bytecode;
  cached_function_t *cached_function_in_use;
#ifdef CMAKE_BUILD_TYPE_DEBUG
  bool manually_trigger_thread_state_reset;
#endif
} ThreadState;

int init_thread_state(ThreadState *ts, SocketState *socket_state, int thread_index);

#endif
