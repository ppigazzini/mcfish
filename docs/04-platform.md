# Platform

Everything under [`src/platform/`](../src/platform): the OS runtime the engine sits
on.

Audience: platform contributors. The zone's place in the dependency stack is in
[00-architecture.md](00-architecture.md).

## The zone today

Every module is in the build.

| Module | In `SOURCES`? | Driven by the search? | Owns |
| --- | --- | --- | --- |
| [`clock.c`](../src/platform/clock.c) | **yes** | **yes** | `now_ms`, the one clock the engine reads |
| [`memory.c`](../src/platform/memory.c) | **yes** | no | large-page aligned allocation and the page-allocator seam |
| [`thread_runtime.c`](../src/platform/thread_runtime.c) | **yes** | no | the mutex / condition-variable / atomic primitives |
| [`thread.c`](../src/platform/thread.c) | **yes** | no | one OS thread and its idle-loop handshake |
| [`thread_pool.c`](../src/platform/thread_pool.c) | **yes** | no | the Lazy-SMP worker pool, the shared stop flag, the NUMA binding plan |
| [`numa.c`](../src/platform/numa.c) | **yes** | no | the NUMA topology model and the replication registry |
| [`tablebase.c`](../src/platform/tablebase.c) | **yes** | **yes** | the Syzygy facade the engine and shell call |
| [`syzygy/`](../src/platform/syzygy) | **yes** | **yes** | the prober: `tables.c`, `encode.c`, `decode.c`, `registry.c`, `wdl.c`, `probe.c` |

**The two columns are different claims, and only the first one is now true of every
row.** Being in `SOURCES` means the module compiles under the full warning set, links
into `zone-check`, and can be reached by `./build.sh test` — the thread pool and the
NUMA config are covered by unit tests and by `./build.sh tsan`. Being *driven by the
search* means the engine's behaviour changes when the module does, and for the
thread/NUMA rows it still does not: nothing constructs a pool, so `Threads` above 1
is accepted and ignored.

**Why the pool is built but not driven.** Lazy-SMP is not a matter of calling
`thread_pool_set`. The live search keeps every piece of per-worker state in file-scope
globals — the `SearchCtx` in `search.c`, the `Histories` block in `history.c`, and the
accumulator stack, refresh cache and scratch dirty-piece records in `evaluate.c`.
Running two workers over those is not parallel search, it is a data race on 28 MiB of
history tables and one shared NNUE accumulator. The remaining work is to make that
state per-worker and to route the node sum, the thread vote and `best_move_changes`
through a seam that answers with thread 0's own values at `Threads 1` — which is what
keeps the anchor bit-exact. The per-worker layout that state has to move to is already
written, in [`src/engine/state/`](../src/engine/state).

The authoritative status list is `tools/upstream/port_map.tsv`, and `./build.sh
port-status` prints it live.

## What each module does

### Memory

Hands back blocks that are 2 MiB-aligned and rounded up to whole large pages. The
alignment is load-bearing: the NNUE accumulator reads it as a precondition. The
contents are **not** initialised, exactly as upstream leaves them
(`memory.cpp:129`). An allocator that zeroes looks harmless and is not — it lets a
constructor that forgets a field read 0 and look correct, so the field is right only
because the allocator hid the omission. `worker_construct_full` zeroes its own block
and then writes every field it does not otherwise clear, including the tablebase
config that upstream initialises through `Tablebases::Config`'s own member defaults
(`syzygy/tbprobe.h:41`).

`page_alloc` is the exception and says so: it is backed by an anonymous mapping,
which the kernel is required to hand over zeroed.

It degrades to a plain aligned allocation on a host without huge pages and **never
fails an allocation `malloc` could serve**. Today `tt_resize` calls `aligned_alloc`
directly; routing it through here is what puts the hottest random-access structure
in the engine onto large pages.

### Threads and the pool

`thread_runtime.c` wraps pthreads. zfish hand-rolls a Drepper futex mutex and a
sequence-counter condition variable because Zig 0.16 removed
`std.Thread.Mutex`/`Condition`; **C has no such gap**, so this module wraps the
`std::mutex` / `std::condition_variable` pair upstream actually uses rather than
re-deriving the futex protocol. Every predicate it guards is re-checked by its
caller in a loop, so a spurious wakeup is harmless and a missed wakeup is not:
signal and broadcast must be reached on every path that changes a predicate.

`thread.c` parks a thread in an idle loop from the moment it is spawned and runs one
job at a time. `searching` is the single predicate. The invariant callers depend on:
**`thread_spawn` returns only after the thread has reached the parked state**, so a
job submitted immediately afterwards cannot be lost. `worker` is deliberately opaque
here — the pool's builder attaches the search payload and this module never
dereferences it, which is what keeps the thread vehicle separable from the search
zone it will drive.

`thread_pool.c` is **inert until `thread_pool_set` is called**, and a pool of one
thread performs no binding, no distribution and no cross-thread synchronisation
beyond the single idle-loop handshake. That is the property the `signature` gate
rests on: nothing in the pool may make thread 0's behaviour depend on wall-clock
time, on scheduling, or on an address. The `Worker` is attached through an injected
`ThreadBuilder` rather than constructed here, so the pool never reaches into the
search zone.

`stop` and `increase_depth` are **sequentially consistent**, and that is deliberate.
Upstream's are plain `std::atomic_bool` assignments and reads (`thread.h:157`); only
two sites in the whole engine opt out, the in-tree abort checks at `search.cpp:770`
and `search.cpp:1403`, which spell `memory_order_relaxed` explicitly. Making every
access relaxed is not a free optimisation — `stop` is raised by one thread and must be
seen by every other worker's depth loop, and under relaxed ordering the compiler may
hoist the load out of that loop entirely. `atomic_bool_load_relaxed` exists for the two
sites upstream names and nowhere else. The per-worker counters go the other way:
upstream wraps them in `RelaxedAtomic` (`misc.h:337`), so `AtomicU64` is relaxed.

Driving the pool also fixes the shell's `stop` gap: nothing reads stdin while
`cmd_go` is inside `search_go`, so `go infinite` does not return. See
[05-shell.md](05-shell.md).

### NUMA

A `NumaConfig` is a list of nodes, each an ascending duplicate-free CPU set, plus the
reverse cpu→node index. The invariant: **a CPU belongs to at most one node**, and
`numa_config_add_cpu_to_node` refuses a re-assignment rather than silently moving the
CPU, because a topology where one CPU appears twice makes every thread-distribution
answer arbitrary.

A `NumaReplicationContext` owns one config and a registry of replicated objects — the
NNUE network is the live one. **Replacing the config notifies every registered object
to re-replicate**, and that notification is the whole point of the registry; skipping
it is how a `NumaPolicy` change becomes a silent no-op.

The topology is read from `/sys` rather than by linking libnuma, so the build keeps
its no-dependencies property. It fails soft everywhere: no `/sys`, no nodes or a
restricted affinity mask all degrade to one node holding every allowed CPU, which is a
correct single-node run. An unparseable `NumaPolicy` string is the one case that does
**not** degrade — it is refused, and the caller keeps the previous topology, because
installing a config with zero nodes makes `distribute_threads` and
`suggests_binding_threads` divide by a node count of zero.

**The partition is L3-aware first.** Upstream's default policy is
`BundledL3Policy{32}` (`engine.cpp:58`), and `from_system` tries the L3-aware config
before falling back to the raw `/sys/devices/system/node` read (`numa.h:583`). This is
not a refinement: on a chiplet CPU one system NUMA node spans several L3 domains, so
the raw partition reports **one** node where upstream reports one per bundle — a
different thread distribution, a different number of shared-history banks, and a
different bind decision for the same `Threads`. `try_get_l3_aware_config` reads each
CPU's `cache/index3/shared_cpu_list`, groups the domains by the system node they sit
on, merges them pairwise while a pair still fits in 32 CPUs, and emits one config node
per surviving domain. `auto`, `system` and `hardware` all resolve to it; only the raw
partition the L3 pass itself reads uses `SystemNumaPolicy`.

**The policy-string grammar is upstream's, including what it forgives.**
`indices_from_shortened_string` (`numa.h:1033`) never fails: an element it cannot read
contributes nothing and the walk continues, so `0-1,7-3` is the node `{0,1}` rather
than a rejected string. What *is* refused is a string naming no node at all
(`numa.h:686`) and any CPU claimed twice — `add_cpu_to_node` opens with `if
(is_cpu_assigned(c)) return false` (`numa.h:995`), which rejects a re-add even into the
node that already holds the CPU. Matching both halves matters: a policy the two engines
read differently is a topology they run differently.

`numa_execute_on_node` runs its callback on a **throwaway thread** and joins it, as
upstream's `execute_on_numa_node` does (`numa.h:957`). The binding is the point of the
call and must not survive it — binding the caller instead confines whoever asked (the
UCI thread, during a pool rebuild) to one node for the rest of the process, and every
later allocation it makes first-touches that node. The same reasoning is why the
per-node shared-history banks are inserted *through* it: the bank is tens of megabytes
and its pages are first-touched by whoever writes them first, so inserting every node's
bank from the calling thread puts them all on one node, which is precisely the cost
per-node banks exist to avoid (`thread.cpp:208`).

### Syzygy

[`tablebase.h`](../src/platform/tablebase.h) is the facade — the one surface the
engine and shell call — and everything under `syzygy/` is an implementation detail
of it. `tablebase_init` scans a `SyzygyPath` and is the only way tables enter the
process; it may be called again for a new path and releases the previous set.

**Until it is called with a non-empty path, every probe reports `available == 0` and
the max cardinality is 0.** That is the normal state of an engine with no tablebases
installed and the state `bench` runs in, and it is why wiring the prober left
`./build.sh signature` unchanged: with no path set, the root ranking in
[`../src/engine/search/root_move_build.c`](../src/engine/search/root_move_build.c)
does not run, every `tb_rank` and `tb_score` stays 0, and the root list is the move
list in generator order.

The internal split is deliberate:

- [`encode.c`](../src/platform/syzygy/encode.c) is pure board geometry — the
  binomials, the 462-entry king-pair map, the square maps, the leading-pawn
  encoding. No I/O, no engine types. **Every table reads as zero until
  `encode_init_geometry` runs**, so the registry builds the geometry before it
  registers a table.
- [`tables.c`](../src/platform/syzygy/tables.c) is the on-disk data model. Every
  pointer field of a `PairsData` aims into a mapped file and is valid only while
  that mapping lives.
- [`decode.c`](../src/platform/syzygy/decode.c) is the compressed stream.
  `decode_set_sizes` is **the only place a table's shape is validated** — it
  bounds-checks every field against the mapped length and refuses an inverted or
  oversized symbol-length pair, which is what lets the hot `decode_pairs` walk the
  stream with only the guards a corrupt file makes unavoidable.
- [`registry.c`](../src/platform/syzygy/registry.c) owns the material-key→table map,
  the lazy mmap and the parse. `wdl.c` and `probe.c` import it and never the
  reverse, so neither side becomes a god-file. Nothing here is thread-safe, exactly
  as upstream.
- [`wdl.c`](../src/platform/syzygy/wdl.c) is the WDL probe, including the capture
  recursion upstream calls `search` — which is what a WDL probe actually *is*,
  because the stored value is wrong for a position whose every legal move zeroes the
  fifty-move counter. `search_wdl` does and undoes moves on the position it is given
  and **restores it exactly**, so the caller may hand it the live search position.
- [`probe.c`](../src/platform/syzygy/probe.c) is the DTZ probe and the two public
  entry points. `available == 0` is the single encoding for "no result" — no path,
  no table for this material, or a file that would not parse.

All seven files are in both `SOURCES` and `ENGINE_SOURCES`; the module links
against no library and needs only the `-D_POSIX_C_SOURCE=200809L` that
`CFLAGS_COMMON` already sets. The shell half is
[`syzygy_option.c`](../src/shell/syzygy_option.c), which holds the four option
values and binds `TbMaxCardinality` / `TbProbeFen` / `TbProbeWdlPos` and the three
`OptionSyzygy*` readers; `uci.c` only dispatches to it.

`registry.c` **deviates from upstream on a corrupt file**: upstream
(`syzygy/tbprobe.cpp:267`) prints `Corrupt tablebase file` and `exit()`s, while
ccfish prints the same diagnostic and reports the file unavailable, so one bad
file does not take a GUI's engine down mid-game. Keep the diagnostic — without it
a corrupt table is indistinguishable from an absent one.

Two gaps remain. The `d` command prints no `Tablebases WDL:` / `Tablebases DTZ:`
lines, so there is no per-position probe inspection surface. And `./build.sh tb`
runs on the 3-man set only, which leaves the cursed-win / blessed-loss branches of
`map_score_dtz` and `probe_dtz` — reachable only at DTZ > 100, so only from 5-man
tables — unexercised.

## The clock

[`clock.h`](../src/platform/clock.h) declares exactly one function:

```c
uint64_t now_ms(void);
```

[`clock.c`](../src/platform/clock.c) implements it with `clock_gettime`:

```c
clock_gettime(CLOCK_MONOTONIC, &ts);
return (uint64_t) ts.tv_sec * 1000 + (uint64_t) ts.tv_nsec / 1000000;
```

### Why CLOCK_MONOTONIC and not CLOCK_REALTIME

`CLOCK_REALTIME` tracks wall-clock time and is **adjustable**: NTP steps it, a user
or a container runtime can set it, and daylight-saving handling can move it. Any of
those during a search would make an elapsed computation jump forward — ending the
search early — or go **negative**, which in the search's `(int)` cast comparison
means the deadline is never reached and the engine keeps thinking past its time
control. A lost game on time is the visible symptom; nothing in the engine would log
anything.

`CLOCK_MONOTONIC` counts from an unspecified origin and never goes backwards. The
header states the contract this creates for callers: **only differences are
meaningful.** The origin is not the epoch and does not survive a reboot, so `now_ms`
must never be used to timestamp anything, print a time of day, or seed anything.

The engine reads no other clock. That single source is what makes the determinism
argument in [02-engine-search.md](02-engine-search.md) tractable: there is exactly
one place nondeterminism enters, and the search's node-count checkpoint is the only
thing that consults it inside the recursion.

### The feature-test macro

`clock_gettime` and `CLOCK_MONOTONIC` are POSIX, not ISO C. Under `-std=c23` clang
compiles in a strict-conformance mode where `<time.h>` does not declare them, so the
build defines the feature-test macro for every translation unit:

```bash
# build.sh, CFLAGS_COMMON
-D_POSIX_C_SOURCE=200809L
```

That is POSIX.1-2008, the level at which `clock_gettime`, `CLOCK_MONOTONIC`, and
`strtok` are all visible. The macro is set once, in
[`../build.sh`](../build.sh), and not in any source file — a `#define` before an
`#include` in one `.c` file is exactly the kind of per-file drift that produces one
translation unit compiled against a different set of declarations than its
neighbours.

**This is the portability boundary of the whole repo.** ccfish builds on POSIX
hosts. A Windows build would need a `now_ms` backed by `QueryPerformanceCounter`, a
replacement for the shell zone's `strtok` usage, and Windows equivalents for the
`pthread`, `mmap` and `/sys` dependencies in the unwired modules above. Adding that
backend means a second implementation file behind the same header, selected in
`build.sh`'s source list — not `#ifdef _WIN32` scattered through `clock.c`.

## The engine→platform edge

[`../src/engine/search/search.c`](../src/engine/search/search.c) includes
`../../platform/clock.h` directly. That inverts the stated dependency direction:
`engine/` is supposed to include nothing outside `engine/`.

The consequence for this zone specifically: `src/platform/clock.c` is a member of
`ENGINE_SOURCES` in [`../build.sh`](../build.sh), so it is linked into the zone
check and into the test binary. `./build.sh zone-check` therefore cannot detect the
edge — it proves engine+platform links without shell, and clock is on the inside of
that boundary.

The fix is written and unwired: the decomposed search takes its clock through
[`../src/engine/search/time_source.h`](../src/engine/search/time_source.h), the same
function-pointer seam shape `search_set_output` already uses for output. Until that
lands, `engine/` cannot be built or tested without a POSIX clock.

## Do not land a stub whose functions return constants

None of the modules above is stubbed, hidden behind a flag, or half-wired. Each one
is either real code or not there, and that is the property to preserve.

zfish's own docs record the cost of the alternative: a placeholder NUMA handle that
nothing ever dereferenced forced every function in the surface to return a fixed
value, and the resulting shape read as an architectural choice for long enough that
nobody fixed it. An absent module is honest; a stub that answers is a lie the type
system endorses.

The current gap has the same failure mode one level up, and it is worth naming
plainly: **a ported module outside `SOURCES` reads as done and is not defended.**
It compiles against a tree that has since moved, and the first thing the wiring
commit discovers is how far. Wire each module in the commit that finishes it, or
expect to re-port it.

One module per commit, naming the zfish source in the body — see
[PORTING.md](PORTING.md).
