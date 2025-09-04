#ifndef CONFIG_H_
#define CONFIG_H_

#define MAX_THREADS 8
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

#define LINE_BUF_BYTES (1024 * 1024 * 1024)

#define VERSION_STRING_SIZE 128

#define PUBLIC_KEY_FILE_SUFFIX ".pubkey"
#define PRIVATE_KEY_FILE_SUFFIX ".privkey"

#endif
