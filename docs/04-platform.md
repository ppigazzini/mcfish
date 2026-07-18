# Platform

Everything under [`src/platform/`](../src/platform): the OS runtime the engine sits
on.

Audience: platform contributors. The zone's place in the dependency stack is in
[00-architecture.md](00-architecture.md).

## The zone today

The modules are written. **One of them is in the build.**

| Module | In `SOURCES`? | Owns |
| --- | --- | --- |
| [`clock.c`](../src/platform/clock.c) | **yes** | `now_ms`, the one clock the engine reads |
| [`memory.c`](../src/platform/memory.c) | no | large-page aligned allocation and the page-allocator seam |
| [`thread_runtime.c`](../src/platform/thread_runtime.c) | no | the mutex / condition-variable / atomic primitives |
| [`thread.c`](../src/platform/thread.c) | no | one OS thread and its idle-loop handshake |
| [`thread_pool.c`](../src/platform/thread_pool.c) | no | the Lazy-SMP worker pool, the shared stop flag, the NUMA binding plan |
| [`numa.c`](../src/platform/numa.c) | no | the NUMA topology model and the replication registry |
| [`tablebase.c`](../src/platform/tablebase.c) | no | the Syzygy facade the engine and shell call |
| [`syzygy/`](../src/platform/syzygy) | no | the prober: `tables.c`, `encode.c`, `decode.c`, `registry.c`, `wdl.c`, `probe.c` |

**A module that is not in `SOURCES` is compiled by nothing.** It is not in the
binary, not linked by `zone-check`, not reached by `./build.sh test`, and covered by
no gate. So the engine still runs on one thread, still allocates its transposition
table with a plain `aligned_alloc`, and still has no endgame knowledge — not because
those modules are missing, but because nothing calls them.

**That is a gap, not a design.** Each row below says what its own wiring commit has
to decide.

## What each unwired module needs to enter the build

Every one is **required** for the goal stated in [PORTING.md](PORTING.md). The
authoritative status list is `tools/upstream/port_map.tsv`, and `./build.sh
port-status` prints it live.

### Memory (M4)

Hands back blocks that are 2 MiB-aligned, rounded up to whole large pages, and
**zeroed**. All three properties are load-bearing, and the third is the one that
surprises: `Worker` construction relies on the zero-fill for a field that neither
its constructor nor its `clear()` initialises, so a reused non-zeroed block would
leave that field heap-layout-dependent — exactly the kind of address dependence the
signature gate exists to catch.

It degrades to a plain aligned allocation on a host without huge pages and **never
fails an allocation `malloc` could serve**. Today `tt_resize` calls `aligned_alloc`
directly; routing it through here is what puts the hottest random-access structure
in the engine onto large pages.

### Threads and the pool (M4)

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

Wiring the pool also fixes the shell's `stop` gap: nothing reads stdin while
`cmd_go` is inside `search_go`, so `go infinite` does not return. See
[05-shell.md](05-shell.md).

### NUMA (M4)

A `NumaConfig` is a list of nodes, each an ascending duplicate-free CPU set, plus the
reverse cpu→node index. The invariant: **a CPU belongs to at most one node**, and
`numa_config_add_cpu_to_node` refuses a re-assignment rather than silently moving the
CPU, because a topology where one CPU appears twice makes every thread-distribution
answer arbitrary.

A `NumaReplicationContext` owns one config and a registry of replicated objects — the
NNUE network is the live one. **Replacing the config notifies every registered object
to re-replicate**, and that notification is the whole point of the registry; skipping
it is how a `NumaPolicy` change becomes a silent no-op.

The topology is read from `/sys/devices/system/node` rather than by linking libnuma,
so the build keeps its no-dependencies property. It fails soft everywhere: no
`/sys`, no nodes, a restricted affinity mask or an unparseable policy string all
degrade to one node holding every allowed CPU, which is a correct single-node run.

### Syzygy (M5)

[`tablebase.h`](../src/platform/tablebase.h) is the facade — the one surface the
engine and shell call — and everything under `syzygy/` is an implementation detail
of it. `tablebase_init` scans a `SyzygyPath` and is the only way tables enter the
process; it may be called again for a new path and releases the previous set.

**Until it is called with a non-empty path, every probe reports `available == 0` and
the max cardinality is 0.** That is the normal state of an engine with no tablebases
installed and the state `bench` runs in, which is what lets the prober be wired in
without moving the anchor: with no path set, the root ranking in
[`../src/engine/search/root_move_build.c`](../src/engine/search/root_move_build.c)
does not run, every `tb_rank` and `tb_score` stays 0, and the root list is the move
list in generator order.

The internal split is deliberate and should survive the wiring:

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

[`../src/platform/syzygy/PORT_NOTES_syzygy.md`](../src/platform/syzygy/PORT_NOTES_syzygy.md)
lists exactly what the wiring commit must add outside the module: the seven source
files in both `SOURCES` and `ENGINE_SOURCES`, and the four UCI options. The module
links against no library and needs only the `-D_POSIX_C_SOURCE=200809L` that
`CFLAGS_COMMON` already sets.

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
