#include "thread_runtime.h"

// Ignore the pthread return codes deliberately. The only documented failures for these
// calls on a default-attribute mutex are EINVAL (an uninitialised object) and EDEADLK /
// EPERM (a lock discipline error) -- programmer errors, not runtime conditions a chess
// engine can recover from. Checking them would add a branch on the hottest handshake in
// the pool and leave nothing sensible to do in the failure arm.

void mutex_init(Mutex *m) { (void) pthread_mutex_init(&m->handle, nullptr); }

void mutex_destroy(Mutex *m) { (void) pthread_mutex_destroy(&m->handle); }

void mutex_lock(Mutex *m) { (void) pthread_mutex_lock(&m->handle); }

void mutex_unlock(Mutex *m) { (void) pthread_mutex_unlock(&m->handle); }

void condition_init(Condition *cv) { (void) pthread_cond_init(&cv->handle, nullptr); }

void condition_destroy(Condition *cv) { (void) pthread_cond_destroy(&cv->handle); }

void condition_wait(Condition *cv, Mutex *m) { (void) pthread_cond_wait(&cv->handle, &m->handle); }

void condition_signal(Condition *cv) { (void) pthread_cond_signal(&cv->handle); }

void condition_broadcast(Condition *cv) { (void) pthread_cond_broadcast(&cv->handle); }
