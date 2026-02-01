#include "alloc.h"
#include "utils.h"
#include <assert.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdlib.h>
#if defined __APPLE__
#include <malloc/malloc.h>
#elif defined(__linux__) || defined(__GLIBC__) || defined(_WIN32)
#include <malloc.h>
#endif

// The code in this file is copied from the default QuickJS allocator (which
// unfortunately is not exposed publicly) but with some modifications to allow
// query bytecode to be allocated 'off the books' so that we can share it
// between runtime contexts without unnecessary copying.

// If anything to do with POSIX thread local storage fails in any thread, we
// just set this flag to false and fall back on normal behavior. This could
// lead to spurious memory leak warnings in debug builds, in the worst case.
static atomic_bool pthread_key_init_succeeded = true;

static pthread_key_t alloc_behavior_key;
static pthread_once_t alloc_behavior_key_once = PTHREAD_ONCE_INIT;
static void make_alloc_behavior_key() {
  if (0 != pthread_key_create(&alloc_behavior_key, NULL)) {
    atomic_store_explicit(&pthread_key_init_succeeded, false,
                          memory_order_release);
    return;
  }
  if (0 != pthread_setspecific(alloc_behavior_key, (void *)MY_MALLOC_NORMAL)) {
    atomic_store_explicit(&pthread_key_init_succeeded, false,
                          memory_order_release);
  }
}

#if defined(__APPLE__)
#define MALLOC_OVERHEAD 0
#else
#define MALLOC_OVERHEAD 8
#endif

// Set the opaque to one of these values to affect behavior.
char MY_MALLOC_NORMAL[1] = {0};
char MY_MALLOC_BYTECODE[1] = {0};

static size_t my_malloc_usable_size(const void *ptr) {
#if defined(__APPLE__)
  return malloc_size(ptr);
#elif defined(_WIN32)
  return _msize((void *)ptr);
#elif defined(EMSCRIPTEN)
  return 0;
#elif defined(__linux__) || defined(__GLIBC__)
  return malloc_usable_size((void *)ptr);
#else
  /* change this to `return 0;` if compilation fails */
  return malloc_usable_size((void *)ptr);
#endif
}

static void *my_malloc(JSMallocState *s, size_t size) {
  void *ptr;

  pthread_once(&alloc_behavior_key_once, make_alloc_behavior_key);

  /* Do not allocate zero bytes: behavior is platform dependent */
  assert(size != 0);

  if (unlikely(s->malloc_size + size > s->malloc_limit))
    return NULL;

  ptr = malloc(size);
  if (!ptr)
    return NULL;

  // Default behaviour on failure of pthread_getspecific or init failure
  if (atomic_load_explicit(&pthread_key_init_succeeded, memory_order_acquire) &&
      pthread_getspecific(alloc_behavior_key) != (void *)MY_MALLOC_BYTECODE) {
    s->malloc_count++;
    s->malloc_size += my_malloc_usable_size(ptr) + MALLOC_OVERHEAD;
  }

  return ptr;
}

static void my_free(JSMallocState *s, void *ptr) {
  if (!ptr)
    return;

  pthread_once(&alloc_behavior_key_once, make_alloc_behavior_key);

  // Default behaviour on failure of pthread_getspecific or init failure
  if (atomic_load_explicit(&pthread_key_init_succeeded, memory_order_acquire) &&
      pthread_getspecific(alloc_behavior_key) != (void *)MY_MALLOC_BYTECODE) {
    s->malloc_count--;
    s->malloc_size -= my_malloc_usable_size(ptr) + MALLOC_OVERHEAD;
  }
  free(ptr);
}

static void *my_realloc(JSMallocState *s, void *ptr, size_t size) {
  size_t old_size;

  if (!ptr) {
    if (size == 0)
      return NULL;
    return my_malloc(s, size);
  }

  pthread_once(&alloc_behavior_key_once, make_alloc_behavior_key);

  old_size = my_malloc_usable_size(ptr);
  if (size == 0) {
    // Default behaviour on failure of pthread_getspecific
    if (atomic_load_explicit(&pthread_key_init_succeeded,
                             memory_order_acquire) &&
        pthread_getspecific(alloc_behavior_key) != (void *)MY_MALLOC_BYTECODE) {
      s->malloc_count--;
      s->malloc_size -= old_size + MALLOC_OVERHEAD;
    }
    free(ptr);
    return NULL;
  }
  if (s->malloc_size + size - old_size > s->malloc_limit)
    return NULL;

  ptr = realloc(ptr, size);
  if (!ptr)
    return NULL;

  s->malloc_size += my_malloc_usable_size(ptr) - old_size;
  return ptr;
}

const JSMallocFunctions my_malloc_funcs = {
    my_malloc,
    my_free,
    my_realloc,
    my_malloc_usable_size,
};

void set_my_malloc_behavior(char *behavior) {
  assert(behavior == MY_MALLOC_NORMAL || behavior == MY_MALLOC_BYTECODE);
  pthread_once(&alloc_behavior_key_once, make_alloc_behavior_key);
  pthread_setspecific(alloc_behavior_key, (void *)behavior);
}
