# Platform runtime — requests the port could not make itself

Five modules landed: `memory`, `thread`, `thread_pool`, `thread_runtime`, `numa`. They
compile, link and pass an ASan+UBSan driver standalone, but nothing in the tree calls them
yet. Three changes are needed from owners outside this zone.

## 1. `build.sh` — add the sources

New `.c` files must be listed or neither the binary nor `zone-check` nor the test binary
sees them. All five belong in `SOURCES` **and** in `ENGINE_SOURCES`, alongside
`src/platform/clock.c`:

```
  src/platform/memory.c
  src/platform/numa.c
  src/platform/thread.c
  src/platform/thread_pool.c
  src/platform/thread_runtime.c
```

`ENGINE_SOURCES` also needs `-lpthread` on its link line, as `SOURCES` will.

## 2. `build.sh` — `-D_GNU_SOURCE` in `CFLAGS_COMMON`

`memory.c`, `thread.c` and `numa.c` each open with `#define _GNU_SOURCE`, which
[docs/04-platform.md](../../docs/04-platform.md) rightly calls out as per-file
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

## 3. `docs/04-platform.md` — the zone is no longer one pair of files

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

- **`thread.zig`'s `startThinking` / `clear` / `reconfigure` glue.** It reads
  `worker_layout.Worker`, `search_driver`, `root_move_build` and `state_list`, none of
  which exist in mcfish yet. The NUMA-distribution half of `reconfigure` **is** ported (it
  is pure platform); the root-move and limits half is not.
- **`thread_vote.zig`.** It is a pure read over `RootMove`/`Worker` fields, so it belongs
  with the search zone that defines them, not here.
- **`ensureNetworkReplicated`.** A no-op in zfish (weights are always resident), so there
  is nothing to port until NNUE lands. Not stubbed.

## Where this diverges from zfish, and why

zfish's `NumaConfig.fromSystem` builds **one node holding every CPU** and says so
(`numa/config.zig:8`, `numa.zig:84`): it never reads the host topology, so `hardware`
cannot differ from `system`, `suggestsBindingThreads` is false everywhere, and upstream
reports `1/16` where zfish reports `1/1`.

Stockfish wins, so `numa_config_from_system` reads
`/sys/devices/system/node/{online,node*/cpulist}` for real (upstream `numa.h:1075`),
intersects with the process affinity mask, and drops empty nodes (`numa.h:652`). No
libnuma: the parse is plain `stdio`, so the build keeps its no-dependencies property.

Still unported from upstream's `from_system`: the `BundledL3Policy` L3-domain split, which
is why `numa_context_set_hardware` still aliases `set_system`. Left explicit in the header
rather than silently aliased.

Two bounds exist here that upstream does not have, because the cpu→node map is an array
where upstream's is a hash: `NumaMaxCpus` (65536) and `NumaMaxNodes` (4096). They read back
to the caller as a malformed `NumaPolicy` string, so a hostile `0-4000000000` is refused
rather than allocating gigabytes.
