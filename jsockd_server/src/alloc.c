#include "alloc.h"
#include "utils.h"
#include <assert.h>
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

static THREAD_LOCAL MyMallocBehavior alloc_behavior = MY_MALLOC_BEHAVIOR_NORMAL;

#if defined(__APPLE__)
#define MALLOC_OVERHEAD 0
#else
#define MALLOC_OVERHEAD 8
#endif

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

  /* Do not allocate zero bytes: behavior is platform dependent */
  assert(size != 0);

  if (unlikely(s->malloc_size + size > s->malloc_limit))
    return NULL;

  ptr = malloc(size);
  if (!ptr)
    return NULL;

  if (alloc_behavior != MY_MALLOC_BEHAVIOR_BYTECODE) {
    s->malloc_count++;
    s->malloc_size += my_malloc_usable_size(ptr) + MALLOC_OVERHEAD;
  }

  return ptr;
}

static void my_free(JSMallocState *s, void *ptr) {
  if (!ptr)
    return;

  if (alloc_behavior != MY_MALLOC_BEHAVIOR_BYTECODE) {
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

  old_size = my_malloc_usable_size(ptr);
  if (size == 0) {
    if (alloc_behavior != MY_MALLOC_BEHAVIOR_BYTECODE) {
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

void set_my_malloc_behavior(MyMallocBehavior behavior) {
  alloc_behavior = behavior;
}
