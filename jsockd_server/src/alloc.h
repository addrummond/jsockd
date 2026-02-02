#ifndef ALLOC_H_
#define ALLOC_H_

#include <quickjs.h>

extern const JSMallocFunctions my_malloc_funcs;

typedef enum {
  MY_MALLOC_BEHAVIOR_NORMAL,
  MY_MALLOC_BEHAVIOR_BYTECODE,
} MyMallocBehavior;

void set_my_malloc_behavior(MyMallocBehavior behavior);

#endif
