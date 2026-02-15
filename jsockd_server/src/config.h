#ifndef CONFIG_H_
#define CONFIG_H_

#define MAX_THREADS 256
#define MESSAGE_UUID_MAX_BYTES 32
#define DEFAULT_MAX_COMMAND_RUNTIME_US 250000
#define DEFAULT_MAX_IDLE_TIME_US 30000000

// This is the interval at which threads pause IO on the UNIX socket to check
// for exceptional conditions (e.g. SIGINT).
#define SOCKET_POLL_TIMEOUT_MS 100

// check memory usage every 100 commands
#define MEMORY_CHECK_INTERVAL 100

// if memory usage increases over this * MEMORY_CHECK_INTERVAL commands, reset
// the interpreter state.
#define MEMORY_INCREASE_MAX_COUNT 3

#define CACHED_FUNCTIONS_HASH_BITS_RELEASE 10
#define CACHED_FUNCTIONS_HASH_BITS_DEBUG 6

#define ERROR_MSG_MAX_BYTES (1024 * 10)

#define INPUT_BUF_BYTES (1024 * 1024)

#define VERSION_STRING_SIZE 128

#define PUBLIC_KEY_FILE_SUFFIX ".pubkey"
#define PRIVATE_KEY_FILE_SUFFIX ".privkey"

#define ABSOLUTE_MAX_LOG_BUF_SIZE (1024 * 1024)

// Following Linux conventions for default pthread stack size stops things
// blowing up unexpectedly. (E.g. QuickJS implicitly assumes that it has
// more stack space available than a defaultly configured pthread will have
// on MacOS.)
//
// ------------------------------ IMPORTANT -----------------------------------
// This allocation is a virtual memory allocation, so reducing it will not
// reduce the actual memory usage of QuickJS interpreter threads.
// ----------------------------------------------------------------------------
#define QUICKS_THREAD_STACK_SIZE                                               \
  (sizeof(void *) == 8 ? (1024 * 1024 * 8) : (1024 * 1024 * 2))

#define MAGIC_KEY_TO_ALLOW_INVALID_SIGNATURES                                  \
  "dangerously_allow_invalid_signatures"

#define LOW_CONTENTION_SPIN_LOCK_MAX_TRIES 200

// Number of spin-loop iterations before emitting a cpu_relax_no_barrier() hint.
// Tuned per-architecture based on the cost of the hint instruction.
#if defined(__x86_64__) || defined(__i386__)
// x86 PAUSE is ~140 cycles on Skylake+. A tight spin iteration is ~3-5
// cycles, so 20 fast spins (~60-100 cycles) cost less than a single PAUSE.
#define SPIN_PAUSE_DELAY_ITERATIONS 20
#elif defined(__aarch64__) || defined(__arm__)
// ARM YIELD is very cheap (typically 1 cycle or NOP-like on Apple Silicon
// and Cortex-A), so there's little cost to starting it early.
#define SPIN_PAUSE_DELAY_ITERATIONS 3
#elif defined(__powerpc__) || defined(__powerpc64__)
// PowerPC SMT priority reduction hint (or 27,27,27) is cheap, similar to
// ARM YIELD.
#define SPIN_PAUSE_DELAY_ITERATIONS 3
#elif defined(__riscv)
// RISC-V Zihintpause pause cost varies by implementation. Use a moderate
// value to hedge between cheap and expensive implementations.
#define SPIN_PAUSE_DELAY_ITERATIONS 10
#else
// cpu_relax_no_barrier() is a no-op on unknown architectures, so the
// threshold is irrelevant. Skip the delay.
#define SPIN_PAUSE_DELAY_ITERATIONS 0
#endif

#endif
