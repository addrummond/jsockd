#ifndef ALLOC_H_
#define ALLOC_H_

#include <quickjs.h>

extern char MY_MALLOC_NORMAL[1];
extern char MY_MALLOC_BYTECODE[1];

extern const JSMallocFunctions my_malloc_funcs;

void set_my_malloc_behavior(char *behavior);

#endif
