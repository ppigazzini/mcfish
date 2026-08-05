// Own the two atomic cells the search shares with whatever runs it.
//
// These live in the ENGINE zone, not the platform zone, because they are not a
// platform service: they are C11 `<stdatomic.h>` and nothing else. Putting them
// under `platform/` made every engine file that touches the stop flag depend on
// the host, which is the dependency direction the zone rule forbids. Platform is
// allowed to depend on engine, so both zones include this one header.
//
// The bodies are `static inline` for the same reason the bitboard leaves are: the
// per-node abort check reads one of these, and an out-of-line call for a single
// load is the cost the header form exists to avoid.
//
// AtomicBool is SEQUENTIALLY CONSISTENT, because upstream's `stop`, `increaseDepth`
// and `ponder` are plain `std::atomic_bool` assignments and reads (thread.h:157) and
// only two sites in the whole engine opt out -- the two in-tree abort checks,
// search.cpp:771 and search.cpp:1405, which say `memory_order_relaxed` explicitly.
// Making every access relaxed is not a free optimisation: `stop` is raised by one
// thread and must be seen by the ID loop of every other, and under relaxed ordering
// the compiler may hoist the load out of the depth loop entirely, so a `stop`
// arrives only when some unrelated barrier happens to publish it.
//
// Upstream: thread.h:157 (the pool's stop flag), misc.h:337 (RelaxedAtomic).

#ifndef MCFISH_ATOMIC_H
#define MCFISH_ATOMIC_H

#include <stdatomic.h>
#include <stdint.h>

typedef struct {
    atomic_bool value;
} AtomicBool;

// Upstream wraps the per-worker counters in RelaxedAtomic (misc.h:337), so every
// accessor below is relaxed by design rather than by omission.
typedef struct {
    atomic_uint_least64_t value;
} AtomicU64;

static inline void atomic_bool_init(AtomicBool *a, bool value) { atomic_init(&a->value, value); }

static inline void atomic_bool_store(AtomicBool *a, bool value) {
    atomic_store_explicit(&a->value, value, memory_order_seq_cst);
}

static inline bool atomic_bool_load(const AtomicBool *a) {
    return atomic_load_explicit(&a->value, memory_order_seq_cst);
}

// Read without ordering. Reserved for the two in-tree abort checks named above;
// anywhere else this silently converts "stop the search now" into "stop it
// eventually".
static inline bool atomic_bool_load_relaxed(const AtomicBool *a) {
    return atomic_load_explicit(&a->value, memory_order_relaxed);
}

static inline void atomic_u64_init(AtomicU64 *a, uint64_t value) { atomic_init(&a->value, value); }

static inline void atomic_u64_store(AtomicU64 *a, uint64_t value) {
    atomic_store_explicit(&a->value, value, memory_order_relaxed);
}

static inline uint64_t atomic_u64_load(const AtomicU64 *a) {
    return atomic_load_explicit(&a->value, memory_order_relaxed);
}

// Add DELTA and return the value held BEFORE the addition, wrapping on overflow as
// unsigned arithmetic does.
static inline uint64_t atomic_u64_fetch_add(AtomicU64 *a, uint64_t delta) {
    return atomic_fetch_add_explicit(&a->value, delta, memory_order_relaxed);
}

#endif  // MCFISH_ATOMIC_H
