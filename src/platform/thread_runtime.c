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

void atomic_bool_init(AtomicBool *a, bool value) { atomic_init(&a->value, value); }

void atomic_bool_store(AtomicBool *a, bool value) {
    atomic_store_explicit(&a->value, value, memory_order_relaxed);
}

// Cast the const away for the load. C11's atomic_load_explicit takes a non-const pointer,
// but a relaxed load mutates nothing the caller can observe, so the const in the signature
// is the honest description of what this does.
bool atomic_bool_load(const AtomicBool *a) {
    AtomicBool *mut = (AtomicBool *) (uintptr_t) (const void *) a;
    return atomic_load_explicit(&mut->value, memory_order_relaxed);
}

void atomic_u64_init(AtomicU64 *a, uint64_t value) { atomic_init(&a->value, value); }

void atomic_u64_store(AtomicU64 *a, uint64_t value) {
    atomic_store_explicit(&a->value, value, memory_order_relaxed);
}

uint64_t atomic_u64_load(const AtomicU64 *a) {
    AtomicU64 *mut = (AtomicU64 *) (uintptr_t) (const void *) a;
    return atomic_load_explicit(&mut->value, memory_order_relaxed);
}

uint64_t atomic_u64_fetch_add(AtomicU64 *a, uint64_t delta) {
    return atomic_fetch_add_explicit(&a->value, delta, memory_order_relaxed);
}
