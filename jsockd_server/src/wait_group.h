#ifndef WAIT_GROUP_H_
#define WAIT_GROUP_H_

#include <pthread.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>

typedef struct {
  pthread_cond_t cond;
  pthread_mutex_t mutex;
  atomic_int n_remaining;
  // We lock the mutex before calling pthread_cond_signal (optional, explicitly
  // allowed by POSIX) so that we can use the following boolean flag to handle
  // the case where the wait group is zeroed before wait_group_timed_wait is
  // called. 'wait_called' should be accessed only while holding the mutex.
  bool wait_called;
} WaitGroup;

int wait_group_init(WaitGroup *sem, int n_waiting_for);
int wait_group_inc(WaitGroup *sem, int n);
int wait_group_n_remaining(WaitGroup *sem);
int wait_group_timed_wait(WaitGroup *sem, uint64_t timeout_ns);
int wait_group_destroy(WaitGroup *sem);

#endif
