// Define _GNU_SOURCE before any libc header: cpu_set_t, CPU_SET, sched_setaffinity and
// _SC_NPROCESSORS_ONLN all sit behind glibc's __USE_GNU/__USE_MISC guards, which
// -D_POSIX_C_SOURCE=200809L alone does not open. See PORT_NOTES_platform.md.
#define _GNU_SOURCE

#include "thread.h"

#include <sched.h>
#include <string.h>
#include <unistd.h>

// Park the thread, run whatever job arrives, repeat until told to exit. Mirror upstream's
// Thread::idle_loop (thread.cpp:82).
static void *idle_loop(void *arg) {
    Thread *t = (Thread *) arg;

    for (;;) {
        mutex_lock(&t->mutex);
        t->searching = false;
        condition_broadcast(&t->cond);  // wake anyone waiting for search-finished

        // Include `exit` in the predicate: thread_join may set exit+searching and
        // broadcast while this loop is between iterations (just past a job). If we then
        // re-enter here, set searching=false, and waited on `searching` alone, we would
        // re-park and never observe the exit -- nothing sets searching=true again after
        // join's single broadcast. Waiting on `!searching && !exit` makes the exit signal
        // impossible to miss.
        while (!t->searching && !t->exit)
            condition_wait(&t->cond, &t->mutex);

        if (t->exit) {
            mutex_unlock(&t->mutex);
            return nullptr;
        }

        ThreadJobFn job_fn = t->job_fn;
        void *job_ctx = t->job_ctx;
        t->job_fn = nullptr;
        mutex_unlock(&t->mutex);

        if (job_fn != nullptr)
            job_fn(job_ctx);
    }
}

bool thread_spawn(Thread *t, size_t idx) {
    memset(t, 0, sizeof *t);
    t->idx = idx;

    // Start `searching` true; the idle loop drives it to false once the thread parks, so
    // the wait below cannot return before the thread exists.
    t->searching = true;
    t->exit = false;

    mutex_init(&t->mutex);
    condition_init(&t->cond);

    if (pthread_create(&t->handle, nullptr, idle_loop, t) != 0) {
        condition_destroy(&t->cond);
        mutex_destroy(&t->mutex);
        t->searching = false;
        return false;
    }

    t->started = true;
    thread_wait_for_search_finished(t);
    return true;
}

void thread_start_job(Thread *t, ThreadJobFn job_fn, void *job_ctx) {
    if (!t->started)
        return;

    mutex_lock(&t->mutex);
    while (t->searching)
        condition_wait(&t->cond, &t->mutex);
    t->job_fn = job_fn;
    t->job_ctx = job_ctx;
    t->searching = true;
    mutex_unlock(&t->mutex);
    condition_broadcast(&t->cond);
}

void thread_wait_for_search_finished(Thread *t) {
    if (!t->started)
        return;

    mutex_lock(&t->mutex);
    while (t->searching)
        condition_wait(&t->cond, &t->mutex);
    mutex_unlock(&t->mutex);
}

void thread_join(Thread *t) {
    if (!t->started)
        return;

    // Drain any queued or in-flight job BEFORE raising the exit flag. The teardown path
    // runs with the pool's stop flag already set, so an in-flight search bails immediately
    // and emits its bestmove here. Without this drain the exit flag races the idle loop
    // and can drop a just-queued-but-not-yet-started job -- a lost bestmove, deterministic
    // for `go ...; quit` back-to-back.
    thread_wait_for_search_finished(t);

    mutex_lock(&t->mutex);
    t->exit = true;
    t->searching = true;
    mutex_unlock(&t->mutex);
    condition_broadcast(&t->cond);

    (void) pthread_join(t->handle, nullptr);
    t->started = false;

    condition_destroy(&t->cond);
    mutex_destroy(&t->mutex);
}

void *thread_worker(const Thread *t) { return t->worker; }

void thread_set_worker(Thread *t, void *worker) { t->worker = worker; }

size_t thread_index(const Thread *t) { return t->idx; }

bool thread_set_affinity(const size_t *cpus, size_t count) {
    if (cpus == nullptr || count == 0)
        return false;

    cpu_set_t set;
    CPU_ZERO(&set);

    size_t added = 0;
    for (size_t i = 0; i < count; ++i) {
        if (cpus[i] >= (size_t) CPU_SETSIZE)
            continue;  // skip an index the fixed mask cannot express rather than failing
        // Silence -Wsign-conversion inside glibc's CPU_SET, which divides its own `int`
        // parameter by a size_t. The warning is in the macro's expansion, not in the
        // argument: the index is already range-checked above.
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wsign-conversion"
        CPU_SET((int) cpus[i], &set);
#pragma GCC diagnostic pop
        ++added;
    }

    if (added == 0)
        return false;

    return sched_setaffinity(0, sizeof set, &set) == 0;
}

size_t thread_hardware_concurrency(void) {
    // Prefer the affinity mask over the online-CPU count: inside a cpuset-restricted
    // container the two differ, and the mask is the set this process may actually use.
    cpu_set_t set;
    CPU_ZERO(&set);
    if (sched_getaffinity(0, sizeof set, &set) == 0) {
        const int n = CPU_COUNT(&set);
        if (n > 0)
            return (size_t) n;
    }

    const long online = sysconf(_SC_NPROCESSORS_ONLN);
    return online > 0 ? (size_t) online : (size_t) 1;
}
