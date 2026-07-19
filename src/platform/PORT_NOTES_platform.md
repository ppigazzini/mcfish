# Platform runtime — requests the port could not make itself

Five modules landed: `memory`, `thread`, `thread_pool`, `thread_runtime`, `numa`.

**Status: all five are now in `SOURCES` and `ENGINE_SOURCES`, and are covered by
`./build.sh test` and `./build.sh tsan` — but nothing in the tree still calls them.** The
build request below is satisfied; the runtime wiring is not, and that is the milestone
tracked in [docs/04-multithreading.md](../../docs/04-multithreading.md), which also lists
what the wiring commit has to decide.

## 1. `build.sh` — add the sources — **DONE, except the link flag**

All five are listed in `SOURCES` and in `ENGINE_SOURCES`, alongside
`src/platform/clock.c`.

**Still open:** neither link line carries `-lpthread`. That works only because glibc ≥ 2.34
folds the pthread symbols into libc, so the omission is invisible on this host and is not
portable. Add it with the wiring.

## 2. `build.sh` — `-D_GNU_SOURCE` in `CFLAGS_COMMON`

`memory.c`, `thread.c` and `numa.c` each open with `#define _GNU_SOURCE`, which
[docs/06-platform.md](../../docs/06-platform.md) rightly calls out as per-file
feature-macro drift. It is not avoidable from inside a `.c` file's own control:

| symbol | needed by | glibc guard |
| --- | --- | --- |
| `madvise`, `MADV_HUGEPAGE` | `memory.c` | `__USE_MISC` / `__USE_GNU` |
| `MAP_ANONYMOUS` | `memory.c` | `__USE_MISC` |
| `cpu_set_t`, `CPU_SET`, `sched_setaffinity` | `thread.c` | `__USE_GNU` |
| `sched_getaffinity`, `CPU_ISSET` | `numa.c` | `__USE_GNU` |
| `_SC_NPROCESSORS_ONLN` | `thread.c` | `__USE_MISC` |

`-D_POSIX_C_SOURCE=200809L` alone hides every one of them. The `MADV_HUGEPAGE` case is the
dangerous one: it sits behind `#if defined(MADV_HUGEPAGE)`, so without the macro the
huge-page advisory compiles silently away and the fallback *looks* clean while doing
nothing.

The minimal request: add `-D_GNU_SOURCE` to `CFLAGS_COMMON` (it is a superset of
`_POSIX_C_SOURCE=200809L`, so the two coexist), then delete the three `#define` lines.

## 3. `docs/06-platform.md` — the zone is no longer one pair of files

The page states the zone holds `clock.h`/`clock.c` and that there is "no thread runtime, no
allocator, no NUMA handling". That is now stale in the Memory / Threads / Thread pool /
NUMA rows of its own table. The Syzygy row is another agent's.

## Seams the search will need, not built here

`thread_pool.h` deliberately stops at the vehicle. Wiring Lazy-SMP needs three things this
zone must not name:

- **`ThreadBuilder`** — allocate and construct a `Worker` per thread. `thread_pool_set`
  calls it on the thread itself, already bound to its NUMA node, so the Worker's pages are
  first-touched where they will be read. It must leave `worker` null when it fails.
- **`SharedHistoryHooks`** — rebuild the per-node shared history banks.
  `thread_pool_reconfigure` clears them and inserts one bank per occupied node, sized
  `next_power_of_two(threads_on_that_node)`.
- **A search job.** `thread_pool_start_jobs(pool, fn, 1)` starts the siblings with each
  thread's own `worker` as context; the caller drives thread 0.

## Not ported, and why

- **The `start_thinking` / `clear` / `set` glue on the pool.** It reads the `Worker`
  layout, the search driver, root-move building and the state list, none of which exist in
  mcfish yet. The NUMA-distribution half of the reconfigure path **is** ported (it is pure
  platform); the root-move and limits half is not.
- **The thread vote.** It is a pure read over `RootMove`/`Worker` fields, so it belongs
  with the search zone that defines them, not here.
- **`ensure_network_replicated`.** There is nothing to replicate until NNUE lands. Not
  stubbed.

## How the NUMA topology is read

`numa_config_from_system` reads `/sys/devices/system/node/{online,node*/cpulist}` for real
(upstream `numa.h:1075`), intersects with the process affinity mask, and drops empty nodes
(`numa.h:652`). No libnuma: the parse is plain `stdio`, so the build keeps its
no-dependencies property.

The `BundledL3Policy` L3-domain split IS ported: `numa_config_from_system` tries the
L3-aware partition first and falls back to the raw node read, and
`numa_context_set_hardware` no longer aliases `set_system` — it reads the topology without
the process affinity mask, as upstream's `hardware` does (`engine.cpp:227`).

Still open on the policy side: nothing maps the `NumaPolicy` option STRING to
`numa_context_set_system` / `_hardware` / `_none` / `_from_string`, so the option is
neither dispatched nor validated.

Two bounds exist here that upstream does not have, because the cpu→node map is an array
where upstream's is a hash: `NumaMaxCpus` (65536) and `NumaMaxNodes` (4096). They read back
to the caller as a malformed `NumaPolicy` string, so a hostile `0-4000000000` is refused
rather than allocating gigabytes.
