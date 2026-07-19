# Multithreaded search (Lazy-SMP)

Upstream searches with N workers sharing a transposition table and almost nothing
else. mcfish has the machinery and does not yet run it.

Audience: engine and platform contributors. The zone layout is in
[00-architecture.md](00-architecture.md); the OS primitives themselves are in
[06-platform.md](06-platform.md).

## Read this first: the search is single-threaded

**Every module on this page is compiled, linked and unit-tested. None of it is
driven.** `Threads` above 1 is accepted and ignored, and the search runs on the
calling thread against file-scope statics.

That is two different claims and only the first is true of every row below. Being
in `SOURCES` and `ENGINE_SOURCES` means a module compiles under the full warning
set, links into `zone-check`, and is reachable by `./build.sh test` and
`./build.sh tsan`. Being *driven* means the engine's behaviour changes when the
module does — and for all of it, it still does not.

**This is a gap, not a design.** A module verified in isolation and called by
nothing is a module no gate defends when the wiring lands. What follows names
what that wiring commit has to decide.

## Modules

| Module | Owns |
| --- | --- |
| [`thread_runtime.c`](../src/platform/thread_runtime.c) | the mutex / condition-variable / atomic primitives |
| [`thread.c`](../src/platform/thread.c) | one OS thread and its idle-loop handshake |
| [`thread_pool.c`](../src/platform/thread_pool.c) | the worker pool, the shared stop flag, the NUMA binding plan |
| [`numa.c`](../src/platform/numa.c) | the topology model, thread distribution, the replication registry |
| [`memory.c`](../src/platform/memory.c) | large-page aligned allocation and the page-allocator seam |
| [`../src/engine/state/`](../src/engine/state) | the per-worker layout the search's globals must move into |

Goldens: upstream `thread.cpp` / `thread.h` for the pool and the idle loop,
`numa.h` for the topology, `memory.cpp` for the allocator, `search.h:311`
(`Worker`) for the layout.

## The model

Lazy-SMP: every worker runs the *same* iterative deepening on the *same* root
position, at slightly staggered depths, sharing only the transposition table and
the per-node history banks. There is no work splitting and no explicit
communication — the shared TT is the entire coordination mechanism.

Two consequences shape everything here:

- **`Threads 1` must be bit-identical to no pool at all.** The pool is inert
  until `thread_pool_set` is called, and a pool of one performs no binding, no
  distribution and no cross-thread synchronisation beyond a single idle-loop
  handshake. That is the property the `signature` gate rests on.
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

This is the whole of the remaining work, and it is not a matter of calling
`thread_pool_set`. The live search keeps every piece of per-worker state in
file-scope globals:

| Global | Where | What it is upstream |
| --- | --- | --- |
| `Ctx` (a whole `SearchCtx`) | [`search.c`](../src/engine/search/search.c) | the `Worker`'s hot per-node context |
| `Tables` (a whole `Histories`) | [`history.c`](../src/engine/search/history.c) | per-worker tables plus two shared, key-indexed ones |
| `AccStack`, `RefreshCache`, `AccDepth` | [`evaluate.c`](../src/engine/eval/evaluate.c) | the per-worker NNUE arenas |
| `CallsCnt`, `StopOnPonderhit` | [`search.c`](../src/engine/search/search.c) | per-`Worker` / per-manager scalars, and neither is atomic |

Running two workers over those is not parallel search; it is a data race on tens
of megabytes of history tables and one shared NNUE accumulator. `SearchCtx`'s own
header says so — it holds by value what upstream reaches through a `Worker`
pointer *because* the search is single-threaded.

**`Histories` is one flat block and upstream's is not.** It mixes the per-worker
tables (main, low-ply, capture, continuation, tt-move) with the two shared,
key-indexed ones (correction, pawn), which upstream keeps as one bank
per NUMA node sized to that node's thread count. Splitting it into worker-owned
and node-shared halves, and clearing only the former per worker, is part of the
same commit.

The destination already exists: [`src/engine/state/`](../src/engine/state)
defines the per-worker layout, in which **a Worker is one allocation** — the
fixed struct at offset 0 with the accumulator stack and refresh cache following
it, each 64-byte aligned. Keeping them in one block is not a convenience: the
block is first-touched on the node its thread will run on, and splitting the
arenas out would put the accumulator on whichever node happened to allocate it.

Clearing shared tables is **striped**: each worker takes
`[i * n / total, (i + 1) * n / total)`. With one worker the slice is the whole
table, which is why the single-threaded clear matches upstream whether or not the
stripe is honoured — and why it must still be honoured, or two workers race and a
third range is never cleared at all.

### A duplicate that has to collapse

`search.c` keeps its own `Stop`, `Ponder` and `IncreaseDepth` atomics while
`ThreadPool` owns `stop` and `increase_depth`. That is precisely the mirrored
copy [`shared_state.h`](../src/engine/state/shared_state.h) warns against: one
flag with one writer and many relaxed readers is the whole cross-thread protocol,
and a second copy is how the siblings come to disagree about whether a search is
still running. The wiring commit has to make the pool's flag the only one.

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

`numa_execute_on_node` runs its callback on a **throwaway thread** and joins it.
The binding is the point of the call and must not survive it — binding the caller
instead confines whoever asked (the UCI thread, during a pool rebuild) to one
node for the rest of the process, and every later allocation it makes
first-touches that node. The same reasoning is why the per-node history banks are
inserted *through* it: a bank is tens of megabytes, its pages are first-touched
by whoever writes them first, and inserting every node's bank from the calling
thread puts them all on one node — precisely the cost per-node banks exist to
avoid.

Thread distribution keeps its fill ratio in **`float`**, as the port source does:
a wider accumulator would move the ties and hand a different node to a thread.

## What is missing, precisely

Beyond the state split, these are absent rather than merely undriven:

- **No `NumaPolicy` string dispatcher.** `numa_config_from_string` parses only
  the explicit topology grammar; `auto`, `none`, `system` and `hardware` are
  separate C functions. Nothing maps the option's string to them, so the option
  is not even validated.
- **The replication registry is never instantiated.** `NumaReplicatedBase` and
  the attach/notify path exist, but the NNUE network does not register itself and
  no `NumaReplicationContext` is constructed outside `numa.c`. Replacing the
  config is supposed to notify every registered object to re-replicate; with an
  empty registry a policy change is a silent no-op.
- **`numa_config_string` has no caller.** It is the function that would produce
  upstream's `info string Available processors: …`, which the golden harness
  currently filters out as a declared gap.
- **`memory.c` has no unit test**, including its `page_alloc_set` seam.
- **No `-lpthread` on any link line.** This works only because glibc ≥ 2.34 folds
  the pthread symbols into libc; it is not portable and should be added with the
  wiring.

## Testing

`./build.sh tsan` rebuilds the whole engine zone plus the test suite under
ThreadSanitizer and runs it. It is the gate this zone actually needs: the pool
spawns real OS threads, hands them jobs, waits on a condition variable and joins
them, and a missing broadcast or a `worker` slot written after the join is
invisible to every other gate — the single-threaded search never reaches that
code, and a race does not have to fire to be real. TSan instruments the
happens-before edges instead of hoping the schedule lands badly.

It is kept **out of `parity`**: it needs its own build of the whole engine and
roughly triples the suite's runtime. Run it whenever `src/platform/thread*.c`
changes.

The suite covers the policy-string grammar and its refusals, the config-shape
invariants, the single-thread no-bind rule, thread distribution, and a
spawn/job/clear churn loop — construct/destroy is where a teardown race shows up,
and a dropped join leaves a thread reading a freed `Thread`. Its counters are
`atomic_int` deliberately: the pool runs those jobs on four threads at once, so a
plain `int` would itself be a race, and one that hides whether the pool is
running them concurrently at all.

**What it does not cover:** no test constructs a `Worker`, and none exercises
`thread_pool_reconfigure`, `numa_execute_on_node`, the replication context or the
shared-history hooks. Because the only concurrent code the suite reaches is the
pool test, TSan today gates `thread.c`, `thread_pool.c` and `thread_runtime.c`
and nothing else — no engine code runs on more than one thread.

Also note the TSan build uses a reduced flag set and does not inherit
`CFLAGS_COMMON`, so it is not a second warning gate.

## The wiring commit

What it has to do, in the order the dependencies force:

1. Split `Histories` into worker-owned and node-shared halves.
2. Move `SearchCtx`, the NNUE arenas and the per-search scalars into the
   `src/engine/state/` Worker block.
3. Make the pool's `stop` / `increase_depth` the only copies.
4. Route the node sum, the thread vote and `best_move_changes` through a seam
   that answers with **thread 0's own values at `Threads 1`** — which is what
   keeps `./build.sh signature` bit-exact across the change.
5. Dispatch `NumaPolicy`, register the network for replication, and add
   `-lpthread`.

Step 4 is the one that decides whether the anchor survives. Everything else is
mechanical by comparison.
