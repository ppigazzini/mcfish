# Multithreaded search (Lazy-SMP)

Upstream searches with N workers sharing a transposition table and almost nothing
else. mcfish runs it.

Audience: engine and platform contributors. The zone layout is in
[00-architecture.md](00-architecture.md); the OS primitives themselves are in
[06-platform.md](06-platform.md).

## Read this first: the pool is driven

`Threads` builds a worker set, `NumaPolicy` chooses the topology it binds under,
and a `go` runs N workers over one root. Node counts scale with the thread count
and the bestmove is the pool's vote.

**What that scaling actually measures, on this host** (16 logical CPUs, `bench 256
<threads> 2000000 default nodes`, so every position gets the same node budget):

| threads | nodes | time | nps | speedup |
|---|---|---|---|---|
| 1 | 98,016,854 | 109,333 ms | 896,498 | 1.00x |
| 2 | 98,027,473 | 57,164 ms | 1,714,846 | 1.91x |
| 4 | 98,060,707 | 33,358 ms | 2,939,645 | 3.28x |

**A threaded search is not reproducible at a fixed DEPTH, and that is the fact to
carry away.** Three runs of `bench 128 8 10` read 3,773,312 / 6,144,045 / 4,460,748
nodes — a **62.8% spread**, because which thread finishes a depth first decides what
the others skip. The same three runs under a node BUDGET read 9,849,966 / 9,854,910 /
9,852,636, a 0.05% spread. So every gate in this tree that asserts a node count runs
`Threads 1`, correctly, and the only axis that can compare two binaries above one
thread is [`tools/nps_threads.sh`](../tools/nps_threads.sh), which makes the node
count its input for exactly this reason. Lazy-SMP threads still overshoot a budget by
a little and by a different amount each run, which is why its node check is a band.

[`worker_pool.c`](../src/platform/worker_pool.c) is the driver: it owns
the pool, one `SearchWorker` per thread, one shared history bank per occupied NUMA
node, the summed counters and the thread vote.
[`search.c`](../src/engine/search/search.c) sets every worker up on the root,
starts the siblings, searches thread 0 on the calling thread, joins, and votes.

**Thread 0 runs on the calling thread.** Its OS thread is spawned and left parked.
`search_go` blocks, so the caller has a thread to spare; upstream hands thread 0 a
job only because its `go` returns immediately. The tree is the same either way.

## Modules

| Module | Owns |
| --- | --- |
| [`thread_runtime.c`](../src/platform/thread_runtime.c) | the mutex / condition-variable / atomic primitives |
| [`thread.c`](../src/platform/thread.c) | one OS thread and its idle-loop handshake |
| [`thread_pool.c`](../src/platform/thread_pool.c) | the worker pool, the shared stop flag, the NUMA binding plan |
| [`numa.c`](../src/platform/numa.c) | the topology model, thread distribution, the replication registry |
| [`memory.c`](../src/platform/memory.c) | large-page aligned allocation and the page-allocator seam |
| [`../src/engine/state/`](../src/engine/state) | the per-worker `SearchWorker` block and its construction |
| [`worker_pool.c`](../src/platform/worker_pool.c) | the worker set, the shared banks, the counter sums, the vote |
| [`pool_source.h`](../src/engine/search/pool_source.h) | the seam through which the search reads the pool's totals |

Goldens: upstream `thread.cpp` / `thread.h` for the pool and the idle loop,
`numa.h` for the topology, `memory.cpp` for the allocator, `search.h:311`
(`Worker`) for the layout.

## The model

Lazy-SMP: every worker runs the *same* iterative deepening on the *same* root
position, at slightly staggered depths, sharing only the transposition table and
the per-node history banks. There is no work splitting and no explicit
communication — the shared TT is the entire coordination mechanism.

Two consequences shape everything here:

- **`Threads 1` must be bit-identical to no pool at all.** A pool of one performs
  no binding, no distribution and no cross-thread synchronisation beyond a single
  idle-loop handshake, its summed counters are thread 0's own counters, and its
  vote has one candidate. That is the property the `signature` gate rests on.
- **Nothing may make thread 0's behaviour depend on an address, a clock or
  scheduling.** A worker that reads its own node count is deterministic; one that
  reads the *pool's* sum is not, because the sum depends on when the other
  threads got scheduled.

## The pool and worker lifecycle

`thread.c` parks a thread in an idle loop from the moment it is spawned and runs
one job at a time. `searching` is the single predicate, and the invariant callers
depend on is that **`thread_spawn` returns only after the thread has reached the
parked state**, so a job submitted immediately afterwards cannot be lost. The
exit path is folded into the same predicate — waiting on `!searching && !exit`
is what makes the exit signal impossible to miss when `thread_join` broadcasts
while the loop sits between iterations.

`worker` is deliberately an opaque `void *` in `thread.c`. The pool's injected
`ThreadBuilder` attaches the search payload and this module never dereferences
it, which is what keeps the thread vehicle separable from the search zone it will
drive — and what lets `zone-check` keep `platform/` free of `engine/`.

Teardown order is a contract: **join the idle loop first**, so no thread can
touch `worker` afterwards, and only then hand the attached block back to whoever
built it.

Binding happens *on the thread itself*, before its worker is built, so the
worker's one allocation is first-touched on the node that will own it.

## Shared vs per-worker state

Every piece of per-worker state now lives in one `SearchWorker` block, built by
[`worker_construct.c`](../src/engine/state/worker_construct.c) and allocated from
the page allocator so it arrives zeroed and first-touched on the node its thread
will run on.

| What | Where it was | Where it is |
| --- | --- | --- |
| the whole `SearchCtx` | a static in `search.c` | `w->ctx` |
| the history tables | a static `Histories` in `history.c` | `w->hist` |
| the NNUE accumulator, refresh cache and ply counter | three statics in `evaluate.c` | `w->eval_arena`, an opaque `EvalArena` |
| the time manager and the carried-over scores | statics in `search.c` | `w->manager`, thread 0's only |
| `calls_cnt`, `stop_on_ponderhit`, `ponder` | statics in `search.c` | `w->manager` |

**A sibling has no `SearchManager` and that is the whole of the difference.**
Upstream gives it a `NullSearchManager` whose one virtual does nothing; here
`w->manager` is null and the time-management block is behind
`!main_thread -> continue`, so nothing reads it.

`Histories` is split the way upstream splits it — see
[02-engine-search.md](02-engine-search.md). The worker owns main, low-ply,
capture, continuation-correction and the tt-move counter; its node's
`SharedHistories` bank owns the correction and pawn tables (sized by that node's
thread count) and the continuation block.

Clearing the shared tables is **striped**: each worker takes
`[i * n / total, (i + 1) * n / total)`. With one worker the slice is the whole
table, which is why the single-threaded clear matches upstream whether or not the
stripe is honoured — and why it must still be honoured, or two workers race and a
third range is never cleared at all. The continuation block is deliberately *not*
striped: upstream has every worker fill all of it.

### Reading the pool's totals

Upstream reads `threads.nodes_searched()` / `tb_hits()` at exactly five places —
`check_time`, `Worker::elapsed`, `output_pv`, the `nodes as time` settle, and the
best-move-change collection — and the worker's own counter everywhere else. The
distinction is not cosmetic: a value drawn from the pool sum depends on when the
siblings were scheduled, so **no search decision may be taken on one**.

[`pool_source.h`](../src/engine/search/pool_source.h) is that seam. Each hook is
null until the driver installs it, and a null hook reads as this worker's own
value. **At `Threads 1` the two are the same number** — the sum is over one
worker, and that worker is thread 0 — which is what keeps `./build.sh signature`
bit-exact across the wiring.

`ctx->nodes`, `ctx->tb_hits` and `ctx->best_move_changes` are `_Atomic uint64_t`
read and written **relaxed**, through the `ctx_*` accessors. Each has exactly one
writer, so the read-modify-write needs no atomicity — only the store's visibility,
which is why the accessors are a load/store pair and compile to the instructions
the plain integers did.

### The thread vote

`search_worker_best` is upstream's `ThreadPool::get_best_thread`, including the
shortest-mate rule and the PV-length tie-break. Upstream keeps the votes in a hash
map; a scan over the same worker set is the same arithmetic in the same order, and
the map's iteration order never reaches the result.

**A depth-limited or skill-limited search skips the vote entirely** and plays
thread 0's move, which is upstream's own condition — and why `bench`, and every
`go depth N`, is unaffected by the thread count in its choice of move.

## Memory ordering

`stop` and `increase_depth` are **sequentially consistent**, deliberately.
Upstream's are plain `std::atomic_bool` reads and writes; only two sites in the
whole engine opt out, the in-tree abort checks that spell `memory_order_relaxed`
explicitly. Making every access relaxed is not a free optimisation — `stop` is
raised by one thread and must be seen by every other worker's depth loop, and
under relaxed ordering the compiler may hoist the load out of that loop
entirely. `atomic_bool_load_relaxed` exists for the two sites upstream names and
nowhere else.

The per-worker counters go the other way: `nodes`, `tb_hits` and
`best_move_changes` are relaxed, because upstream wraps them in `RelaxedAtomic`
and other threads only ever *read* them for reporting. **No search decision may
be taken on another worker's counter.**

## NUMA

A `NumaConfig` is a list of nodes, each an ascending duplicate-free CPU set, plus
the reverse cpu→node index. The invariant: **a CPU belongs to at most one node**,
and adding it twice is refused rather than silently moving it, because a topology
where one CPU appears twice makes every distribution answer arbitrary.
`node_insert_sorted` takes an ascending-append fast path when the CPU is greater
than the node's current tail, and falls back to a binary search otherwise, so
insertion is `O(log n)` rather than a linear scan from zero — closing a
quadratic-DoS surface reachable through a crafted `NumaPolicy` string.

The topology is read from `/sys` rather than by linking libnuma, so the build
keeps its no-dependencies property. It fails soft everywhere: no `/sys`, no
nodes, or a restricted affinity mask all degrade to one node holding every
allowed CPU, which is a correct single-node run. The one case that does **not**
degrade is a policy string naming no node at all — it is refused, and the caller
keeps the previous topology, because a config with zero nodes makes the
distribution and binding decisions divide by zero.

**The partition is L3-aware first.** Upstream's default policy is
`BundledL3Policy{32}`, and the system read tries the L3-aware config before
falling back to the raw `/sys/devices/system/node` read. This is not a
refinement: on a chiplet CPU one system NUMA node spans several L3 domains, so
the raw partition reports *one* node where upstream reports one per bundle — a
different thread distribution, a different number of shared-history banks, and a
different bind decision for the same `Threads`.

`hardware` differs from `system` in exactly one way: it reads the topology
*without* the process affinity mask, so a run pinned to half the box reports the
whole box.

**`Available processors: …` and the per-node thread-binding line are wired.**
`worker_pool_numa_config_string` (`worker_pool.c`, backed by
`numa_config_to_string` in `numa.c`) renders the installed topology, and
`worker_pool_thread_binding_string` renders the `"placed/cpus"`-per-node split
joined by `:`, returning `nullptr` when nothing is bound. `src/shell/engine.c`'s
`engine_report_threads` prints both before every `go` (skipped for `go perft`),
and `engine_options.c`'s `on_numa_policy`/`thread_allocation_string` print the
same pair from the `NumaPolicy`/`Threads` `setoption` callbacks — see
[07-shell.md](07-shell.md). An invalid `NumaPolicy` value is refused with
upstream's wording verbatim (`"NumaPolicy: invalid value '%s', keeping previous
config."`), and the previous topology stays installed.

`numa_execute_on_node` runs its callback on a **throwaway thread** and joins it.
The binding is the point of the call and must not survive it — binding the caller
instead confines whoever asked (the UCI thread, during a pool rebuild) to one
node for the rest of the process, and every later allocation it makes
first-touches that node. The same reasoning is why the per-node history banks are
inserted *through* it: a bank is tens of megabytes, its pages are first-touched
by whoever writes them first, and inserting every node's bank from the calling
thread puts them all on one node — precisely the cost per-node banks exist to
avoid.

Thread distribution keeps its fill ratio in **`float`**, as upstream does:
a wider accumulator would move the ties and hand a different node to a thread.

## What is missing, precisely

- **The NNUE network is not registered for NUMA replication.** A
  `NumaReplicationContext` is now constructed and `NumaPolicy` reaches it, but
  nothing calls `numa_context_attach`, so the registry is empty and a policy
  change re-partitions the threads without re-replicating any weights. On a
  single-node host that is the whole of the difference; on a multi-node one every
  worker reads the net from the node that loaded it.
- **`numa_config_string` has no caller.** It renders the process affinity mask
  rather than the bound topology — the shape `Available processors: …` used to
  report before the fix that wired `numa_config_to_string`/
  `worker_pool_numa_config_string` in above. It is dead code now, superseded
  rather than merely unwired; nothing should come to call it.
- **`memory.c` has no unit test**, including its `page_alloc_set` seam.
- **No test constructs a `SearchWorker`.** The pool churn test covers
  `thread.c` / `thread_pool.c` / `thread_runtime.c`; the worker set, the shared
  banks and the vote are covered only by `tsan-search` and by the multi-thread
  runs, not by the unit suite.

## What the wiring commit decided

For the record, in the order the dependencies forced:

1. `Histories` split into worker-owned and node-shared halves, with the shared
   bank sized by its node's thread count and cleared in stripes.
2. `SearchCtx`, the NNUE arenas and the per-search scalars moved into the
   `SearchWorker` block, which was itself rebuilt on the live search types after
   the parallel record set in `src/engine/state/` was deleted.
3. The pool's `stop` / `increase_depth` made the only copies.
4. The node sum, the thread vote and `best_move_changes` routed through
   `pool_source.h`, which answers with thread 0's own values at `Threads 1`.
5. `NumaPolicy` dispatched to `numa_context_set_system` / `_hardware` / `_none` /
   `_from_string`, and refused rather than degraded when it names no node.

`-lpthread` was already on every link line.

## The gates

**`bench` is single-threaded, so every gate that reads a node count stays green
while a data race is live and while contention is getting worse.** These are the
ones that do not — and two of the four report on a thread count rather than on a
value, because that is the only thing a one-threaded gate cannot fake.

| step | what it proves here | owned by |
|---|---|---|
| `tsan` | the pool: that spawning, dispatching, waiting on the condition variable and joining carry the happens-before edges they claim | this page |
| `tsan-search` | races in the SEARCH, which `tsan` structurally cannot reach, at a real thread count it measures rather than assumes | this page |
| `async-check` | that a `setoption` arriving during `go infinite` does not wedge on the thread that would release it | [07-shell.md](07-shell.md) |
| `engine-standalone` | that the engine zone links with no platform object behind it, which is what makes the thread seam a seam | [00-architecture.md](00-architecture.md) |

`tools/nps_threads.sh` is the only instrument in the tree that measures more than
one thread, and it is not a gate — see [11-performance.md](11-performance.md). Read
`r(T)/r(1)` from it and never the A/B column, which carries the single-thread speed
difference inside it.

`./build.sh tsan` rebuilds the whole engine zone plus the test suite under
ThreadSanitizer and runs it. It is the gate this zone actually needs: the pool
spawns real OS threads, hands them jobs, waits on a condition variable and joins
them, and a missing broadcast or a `worker` slot written after the join is
invisible to every other gate — a data race does not have to fire to be real. Since
`go` runs even a one-thread search on worker 0's OS thread (see
[07-shell.md](07-shell.md)), every search crosses this dispatch/join, so
`tsan-search` exercises it at `Threads 1` as well. TSan instruments the
happens-before edges instead of hoping the schedule lands badly.

It is kept **out of `parity`**: it needs its own build of the whole engine and
roughly triples the suite's runtime. Run it whenever `src/platform/thread*.c`
changes.

### `./build.sh tsan-search` — the search under real threads

`tsan` links the test binary, so the only concurrent code it reaches is the pool
test. `tsan-search` builds the **whole engine**, shell included, and drives one
`go` through the UCI front end — the only way a race in the *search* can be
observed at all. It takes an optional depth and thread count
(`./build.sh tsan-search 14 8`).

This is now the gate that means something. Before the pool was driven it reported
0 races **because the process never left one thread**, and that number was
recorded as a baseline rather than a result: a race needs two threads to fire, and
zero on one thread is a measurement of the thread count, not of the state. Read
any run of this step against that history — the tempting reading of a small number
is the wrong one.

That reading is not left to the reader. The step samples `/proc/<pid>/task` while
the search runs and reports the **peak OS thread count**, then acts on it: fewer
threads than requested is a failure, and a process that never held two threads
exits 127 as skipped, because a race needs two. Note what it does not do — count
threads out of the sanitizer's own output. Any such string appears only inside a
race report, so on a clean run it is zero by construction and would answer the
caveat with the one number that cannot mean anything.

`Threads 1` passes on its own terms: `go` dispatches even a one-thread search onto
worker 0's OS thread, so the dispatch-and-join handshake is real concurrency.

zfish ran the same experiment once its pool was live and found
**10,664 races** on an 8-thread depth-14 search, on state it had until then been
describing as latent. Latent and unmeasured are not the same claim, and only one
of them is falsifiable.

Also note the TSan build uses a reduced flag set and does not inherit
`CFLAGS_COMMON`, so it is not a second warning gate.
