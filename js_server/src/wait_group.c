
#include "wait_group.h"
#include <assert.h>
#include <time.h>

int wait_group_init(WaitGroup *wg, int n_waiting_for) {
  // Don't need a monotonic clock on Mac because we can use
  // pthread_cond_timedwait_relative_np to set a relative timeout.
#ifdef LINUX
  pthread_condattr_t attr;
  pthread_condattr_init(&attr);
  pthread_condattr_setclock(&attr, CLOCK_MONOTONIC);
  pthread_cond_init(&wg->cond, &attr);
#else
  pthread_cond_init(&wg->cond, NULL);
#endif

  atomic_init(&wg->n_remaining, n_waiting_for);
  wg->wait_called = false;
  return pthread_mutex_init(&wg->mutex, NULL);
}

int wait_group_inc(WaitGroup *wg, int n) {
  int r;
  int previous_value = atomic_fetch_add(&wg->n_remaining, -n);
  if (previous_value > 0 && previous_value <= n) {
    // We have just decremented the wait group to zero. If
    // wait_group_timed_wait has been called already then we need to call
    // pthread_cond_signal. Otherwise, wait_group_timed_wait will notice
    // that the wait group has been zeroed and will return without calling
    // pthread_cond_signal.

    if (0 != (r = pthread_mutex_lock(&wg->mutex)))
      return r;
    if (wg->wait_called && (0 != (r = pthread_cond_signal(&wg->cond)))) {
      pthread_mutex_unlock(&wg->mutex);
      return r;
    }
    return pthread_mutex_unlock(&wg->mutex);
  }
  return 0;
}

int wait_group_n_remaining(WaitGroup *wg) {
  return atomic_load(&wg->n_remaining);
}

int wait_group_timed_wait(WaitGroup *wg, uint64_t timeout_ns) {
  int r;

  r = pthread_mutex_lock(&wg->mutex);
  if (r != 0)
    return r;

  if (atomic_load(&wg->n_remaining) == 0)
    return pthread_mutex_unlock(&wg->mutex);

  wg->wait_called = true;

#if defined LINUX
  struct timespec abstime;
  if (0 != clock_gettime(CLOCK_MONOTONIC, &abstime))
    return -1;
  abstime.tv_nsec += timeout_ns;
  abstime.tv_sec += abstime.tv_nsec / 1000000000;
  abstime.tv_nsec %= 1000000000;
  r = pthread_cond_timedwait(&wg->cond, &wg->mutex, &abstime);
#elif defined MACOS
  struct timespec relative_time = {.tv_nsec = timeout_ns % 1000000000,
                                   .tv_sec = timeout_ns / 1000000000};
  r = pthread_cond_timedwait_relative_np(&wg->cond, &wg->mutex, &relative_time);
#else
#error "Unsupported platform for wait_group_timed_wait"
#endif

  if (r != 0)
    return r;

  r = pthread_mutex_unlock(&wg->mutex);

  if (r != 0)
    return r;
  assert(atomic_load(&wg->n_remaining) <= 0);
  return 0;
}

int wait_group_destroy(WaitGroup *wg) {
  int r1 = pthread_cond_destroy(&wg->cond);
  int r2 = pthread_mutex_destroy(&wg->mutex);
  return r1 == 0 && r2 == 0 ? 0 : -1;
}
