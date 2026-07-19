# Shell

Everything under [`src/shell/`](../src/shell): `main`, the UCI loop and option
table, and the benchmark. This is the process — the only zone that reads stdin,
writes stdout, or knows the protocol exists.

Audience: shell contributors. The zone rule this page depends on is in
[00-architecture.md](00-architecture.md).

## Where this zone stands

**Wired into the binary** ([`../build.sh`](../build.sh) `SOURCES`):
[`main.c`](../src/shell/main.c), [`uci.c`](../src/shell/uci.c),
[`ucioption.c`](../src/shell/ucioption.c),
[`benchmark.c`](../src/shell/benchmark.c).

**Ported and not in `SOURCES`**, so not in the binary and not gated: the engine
object [`engine.c`](../src/shell/engine.c), the identity and process utilities
[`misc.c`](../src/shell/misc.c), the bench script data
[`bench_positions.c`](../src/shell/bench_positions.c), and the decomposition of the
loop itself — [`uci_strings.c`](../src/shell/uci_strings.c),
[`uci_input.c`](../src/shell/uci_input.c),
[`uci_parse.c`](../src/shell/uci_parse.c),
[`uci_format.c`](../src/shell/uci_format.c),
[`uci_output.c`](../src/shell/uci_output.c),
[`uci_bench.c`](../src/shell/uci_bench.c).

**`uci.c` is still the monolith.** It holds the position, the state chain and the
option table as file-scope statics, and it parses `go` and `position` inline.
Everything under *The command loop* below describes that file. The ported
replacements exist beside it and nothing calls them —
[`../src/shell/PORT_NOTES_uci.md`](../src/shell/PORT_NOTES_uci.md) is the list of
decisions the rewiring commit owes, and the last section of this page summarises
them.

**`engine.c` is written, unused, and now duplicates a live table.** It owns what
upstream's `engine.cpp` owns — the position and its state chain, the option table,
the search entry points — behind one seam, and it registers its own copy of the
option set that `uci.c` registers for real. Two registrations, one of them dead, is
a trap: edit the wrong one and the handshake does not move. When `engine.c` is
wired, `uci.c`'s registration must be deleted in the same commit, not left as a
fallback.

## main as the composition root

[`main.c`](../src/shell/main.c) initialises the static tables in dependency order,
then enters the UCI loop:

```c
int main(int argc, char **argv) {
    bitboards_init();
    attacks_init();
    threats_init();  // build RayPassBB, which reads the attack tables
    position_init();

    uci_loop(argc, argv);
    return 0;
}
```

It is the only file permitted to include across every zone, and nothing includes it.
The ordering constraint — and its silent failure mode — is spelled out in
[00-architecture.md](00-architecture.md); the short version is that a `Position`
built before `attacks_init` reads zeroed attack tables and generates no piece
moves, which presents as a search bug.

`uci_loop` performs the rest of the wiring, because it owns the state the wiring
targets:

it installs the output sink, sizes the transposition table, establishes the start
position, resolves the root directory from `argv[0]`, defaults `EvalFile` to the
name this build expects, and loads the net.

Establishing a position before any command matters more than it looks: a `go` or
`d` arriving before any `position` command must operate on the start position, not
on a zeroed `Position` whose `king_square` would `lsb` an empty bitboard — which is
undefined behaviour, not a diagnosable error. See
[01-engine-board.md](01-engine-board.md).

### The net and the root directory

The shell owns the net because it owns the `EvalFile` option; the engine zone
allocates the arenas at startup and never touches the filesystem. `uci.c` keeps two
strings for this: the option value, and the directory the binary was launched from,
derived from `argv[0]`. **That root must carry its trailing separator**, because
`network_load`'s concatenation inserts none.

`report_net()` prints `eval_nnue_status()` through `info string` at the four sites
upstream prints it. **Do not make a failed load silent.** Upstream terminates on a
net it cannot load; ccfish keeps playing on the classical fallback instead, so the
`info string` is the only thing distinguishing "running NNUE" from "running a
placeholder" — and without it a missing file reads as a strength regression. See
[03-engine-eval.md](03-engine-eval.md).

## The output sink

The engine zone never calls `printf`. `search_go` and `perft` emit through a
function pointer the shell installs:

```c
// src/shell/uci.c
static void emit_stdout(const char *line) {
    printf("%s\n", line);
    fflush(stdout);
}
```

Three reasons this indirection is worth its cost:

- **It is what makes the zone rule hold.** `engine/` links without `shell/` because
  it never names a shell symbol. `./build.sh zone-check` and `./build.sh test` both
  link the engine sources with no shell object and would fail instantly on a direct
  `printf` of an `info` line.
- **A gate can capture search output in-process.** Any future harness that wants to
  read `info` lines installs its own sink instead of spawning a subprocess and
  parsing pipes.
- **The stream choice lives in one place.** `info` lines go to stdout because a GUI
  reads stdout; the bench banners go to stderr because upstream puts them there.
  Deciding that per call site is how an engine ends up with half its handshake on
  the wrong stream.

The sink defaults to `nullptr` and `emit_line` checks before calling, so an engine
zone used without a shell is silent rather than crashing. Note the flip side: a
harness that forgets to install a sink sees a search that runs and prints nothing,
which reads as a hang rather than as a wiring mistake.

`fflush` after every line is not optional. stdout to a pipe is block-buffered, and a
GUI waiting on `uciok` or `bestmove` would wait for a full buffer.

The ported [`uci_output.c`](../src/shell/uci_output.c) is the module that generalises
this: it is intended to be **the only module in the tree that writes to a stream**,
with `uci_output_emit_line` carrying the sink signature so wiring it in preserves the
funnel exactly.

## The command loop

`uci_loop` has two modes:

- **Argv mode.** With any argument, the words are joined into one line, executed,
  and the process exits without reading stdin. `./build/ccfish bench 8` and
  `./build/ccfish "go depth 5"` both work, which is what
  [`../build.sh`](../build.sh) relies on for the `bench` and `signature` steps.
- **Interactive mode.** `fgets` into a 4096-byte buffer until `quit` or EOF.

`execute` skips leading whitespace, splits the first word off as the command, and
dispatches. Every handler gets the remainder of the line as a mutable buffer, which
is why `strtok` is usable at all.

### Every command handled

| Command | Behaviour |
| --- | --- |
| `uci` | Print `id name`, `id author`, the option lines, then `uciok`. |
| `isready` | Print `readyok`. |
| `ucinewgame` | Clear the transposition table and reset to the start position. |
| `position` | `startpos` or `fen <6 fields>`, optionally followed by `moves ...`. |
| `go` | Parse limits and run the search, which emits its own `info` and `bestmove` lines through the sink. `go perft N` short-circuits to a perft divide. |
| `setoption` | `setoption name <NAME> [value <VALUE>]`; see the table below. |
| `stop` | Set the stop flag. |
| `quit` | Set the stop flag and leave the loop; `uci_loop` frees the table. |
| `d` | Print the ASCII board, the FEN, and the Zobrist key via `pos_pretty`. |
| `bench` | Run the benchmark at the given depth, default 8. |
| `eval` | Print the evaluation trace via `evaluate_trace`. |
| `compiler` | Print the clang or gcc version and `__STDC_VERSION__` the binary was built with. |
| `ponderhit` | Accepted and ignored — pondering is not implemented. |
| anything else | Print `Unknown command: '<cmd>'. Type help for more information.` |

Two honest notes on that last row: the message names a `help` command, and **there
is no `help` handler** — typing `help` prints the unknown-command message about
itself. And an empty line is not treated as unknown, so a blank line from a GUI is
silently ignored rather than producing noise.

`stop` and `ponderhit` are accepted but cannot do their job. The loop is
single-threaded: while `cmd_go` is inside `search_go`, nothing is reading stdin, so
a `stop` sent during a search is not seen until the search has already returned.
Consequently `go infinite` has no deadline and no interruption path and does not
return. This is a gap in the shell, and the runtime that closes it is already built
and gated — the thread pool in `src/platform/`, whose shared stop flag is the
cross-thread protocol. Nothing drives it yet. See [04-platform.md](04-platform.md).

`ucinewgame` clears the TT but does **not** clear the history block; the live search
clears that per `go` instead, which is not where upstream clears it. See
[02-engine-search.md](02-engine-search.md).

### position

`set_position` is the single entry point for establishing a position, and every path
goes through it. It resets `StatesUsed` before parsing, which is what keeps the
`States[MAX_GAME_PLIES]` array from accumulating across games.

That array is file-scope, and deliberately so: `pos_undo_move` and the repetition
scan both follow `StateInfo::previous`, so a state allocated on `cmd_position`'s
stack would leave the chain pointing into a dead frame the moment the handler
returned. See [01-engine-board.md](01-engine-board.md). The ported owner of that
storage is [`../src/engine/state/position_storage.c`](../src/engine/state/position_storage.c),
which grows by appending fixed chunks rather than reallocating — for exactly the
same reason: **once a state's address is handed out it must never move.**

The FEN branch reassembles six space-separated fields into one buffer, stopping at
`moves` or end of line, and bounded by the buffer size. If `pos_set` rejects the
result, `set_position` falls back to the start position rather than leaving `Pos`
unspecified — the `errors` golden case in
[`../tools/cases/errors.uci`](../tools/cases/errors.uci) pins what a malformed FEN
actually produces.

Moves are applied one at a time with `move_from_uci`, which parses against the
current position's legal moves, writing each move's NNUE deltas into the position's
scratch slots. The walk stops at the first token that does not parse and at
`MAX_GAME_PLIES`. It stops silently — an illegal move in a `moves` list truncates
the game with no diagnostic.

### go

Limits parsed: `depth`, `movetime`, `wtime`, `btime`, `winc`, `binc`, `movestogo`,
`nodes`, `infinite`, `ponder`, and `perft`. How they become a deadline is in
[02-engine-search.md](02-engine-search.md).

When no limit at all is given, `cmd_go` sets `depth 8`. An unqualified `go` from a
script would otherwise start an unbounded search that nothing can stop.

**The parser eats a token after every valueless keyword, and the guard against it
does not work.** The loop pulls a value token for every keyword before it knows
whether the keyword takes one, so `infinite` and `ponder` each swallow whatever
follows them. `infinite` carries an attempted fix — it assigns the lookahead back
to `token` before `continue` — but the `for` statement's increment
(`token = strtok(nullptr, ...)`) overwrites that assignment immediately, so the
assignment is dead and the token is lost anyway. Drive it and see:

```
$ printf 'go infinite depth 2\n' | ./build/ccfish     # searches without a depth limit
$ printf 'go depth 2 infinite\n' | ./build/ccfish     # honours depth 2
```

Keywords that take values are unaffected, and no golden case covers the broken
ordering, so nothing fails. This is a defect, not a convention: do not write around
it by fixing the argument order in a script.

The ported [`uci_parse.c`](../src/shell/uci_parse.c) is the fix, and its
contract is stricter than the current behaviour: **parsing is total** — no input
crashes it and none is rejected mid-scan — and a value that will not convert is
recorded in `bad_token` naming the **keyword**, not the offending value, because
that is what upstream's post-scan `is.fail()` check reports.

`go perft N` runs `perft` with `root = true` and prints the per-move split through
the sink, then a `Nodes searched:` total on stdout. That total is what
`./build.sh perft` greps.

## The option table

There is one, [`ucioption.c`](../src/shell/ucioption.c): typed options with kind,
default, bounds, current value and an on-change callback. `uci.c`'s
`register_options` fills it with upstream's full set, `cmd_uci` renders it, and
`cmd_setoption` hands the whole command body to `options_setoption`.

**Registration order is the wire order.** A GUI parses the `uci` handshake in
emission order and [`../tools/handshake.golden`](../tools/handshake.golden) diffs
it byte for byte against the oracle, so `options_add` appends and `options_render`
walks the same sequence — never a sort, never a hash order. The order is upstream's
own registration order in `engine.cpp`; do not regroup related options. Storage is
fixed: no allocation, bounded names and values, and an add past `OPTION_MAX` is
dropped rather than silently overwriting.

### What each option actually does

The advertised spec — type, default, bounds — matches upstream for all nineteen.
What differs is whether anything reads the value.

| Option | Reaches | Notes |
| --- | --- | --- |
| `Debug Log File` | `start_logger` in `uci.c` | Tees the session to a file, input lines prefixed `>> ` and output `<< `, as upstream's `Tie` streambuf does. Exits when the path cannot be opened. |
| `NumaPolicy` | nothing | **Inert.** No NUMA topology, no thread binding. Says so on every set. |
| `Threads` | nothing | **Inert above 1.** See below. |
| `Hash` | `tt_resize` | |
| `Clear Hash` | `tt_clear` + `search_clear` | The same pair `ucinewgame` runs, which is what upstream's button does. |
| `Ponder` | the time manager | Read through the option seam by `search_tm_init`. |
| `MultiPV` | the search | `search_id_state_init` and `search_emit_pv`. |
| `Skill Level` | the search | `search_skill_level`; below 20 it enables the handicapped move pick. |
| `Move Overhead` | the time manager | |
| `nodestime` | the time manager | Converts the clock to a node budget, which makes a clocked search reproducible. |
| `UCI_Chess960` | `pos_set` | Selects the Chess960 castling parse and move rendering. |
| `UCI_LimitStrength` / `UCI_Elo` | the search | Together they override `Skill Level` through upstream's Elo→level polynomial. |
| `UCI_ShowWDL` | the info line | Adds the `wdl` triple. |
| `SyzygyPath`, `SyzygyProbeDepth`, `Syzygy50MoveRule`, `SyzygyProbeLimit` | nothing | **Inert.** See below. |
| `EvalFile` | `eval_nnue_load` | Re-loads the net and reports the outcome. |

**`Threads` advertises a range it cannot honour, deliberately.** The maximum is
upstream's `max(1024, 4 * hardware_concurrency)` because a narrower one is a
different handshake, and the handshake is what a GUI configures against. The search
is single-threaded, so any value above 1 is accepted and ignored — and the
on-change callback says exactly that on the wire, because a GUI that sets
`Threads 8` and sees silence has no way to learn otherwise. Owner: zfish
`platform/thread_pool.zig`, upstream `thread.cpp`.

**The four Syzygy options are stored, rendered, and not fed to the search.**
Nothing registers the `TbProbeFen` / `TbMaxCardinality` seams in
[`../src/engine/search/tb_source.h`](../src/engine/search/tb_source.h), so the root
ranker reads a zero cardinality and never probes. They are deliberately *not*
routed through the option seam: handing a prober-less search a probe budget only
moves the no-op somewhere harder to find. Owner: zfish `engine/syzygy/`, upstream
`syzygy/tbprobe.cpp`.

### The seam the search reads options through

The search zone never includes a shell header. It reads options through the
function pointers in
[`../src/engine/search/option_source.h`](../src/engine/search/option_source.h),
and `uci_loop` installs the table behind them with `search_set_option_source`.

Without that install the search answers itself from
`facade_option_int` in [`../src/engine/search/search.c`](../src/engine/search/search.c),
which returns upstream's defaults so that a caller with no table — the bench
harness, the unit tests — still searches the right tree. **That fallback is not
neutral and must not be treated as one**: the zone's own default answers 0 to
everything, which reads as MultiPV 0 and Skill Level 0. A partial install is a
wrong search, not an absent one.

`install_seams` runs before every `go`, so the installed source is held separately
and re-applied there. Assigning `OptionIntByName` directly from the shell would be
overwritten on the first search, and every `setoption` a GUI sent would silently
stop taking effect.

### On-change callbacks

`on_hash`, `on_threads`, `on_numa_policy`, `on_syzygy_path`, `on_debug_log_file`
and `on_eval_file` are the seams through which a subsystem becomes reachable from a
`setoption`. Each returns bare text or `nullptr`; the transport adds the
`info string ` prefix, one line per line, as upstream's `print_info_string` does.

**A callback whose subsystem is unported must say so.** Advertising a control that
does nothing, in silence, is the one outcome worse than not advertising it: a GUI
cannot tell a no-op from a working feature. That is why `NumaPolicy`, `Threads`
above 1, and a non-empty `SyzygyPath` each answer.

`on_syzygy_path` is the one already satisfied. The live `uci.c` advertises and
handles all four Syzygy options today by delegating to
[`syzygy_option.c`](../src/shell/syzygy_option.c), which holds the values and binds
the engine's `tb_source.h` and `option_source.h` seams. When `ucioption.c` lands,
move the four declarations into the table and point `on_syzygy_path` at
`syzygy_option_set`; do not re-implement them, and keep them emitted **before**
`EvalFile`, which is upstream's order (`Stockfish/src/engine.cpp:125-138`) and what
`tools/handshake.golden` now pins.

## bench and the signature

[`benchmark.c`](../src/shell/benchmark.c) runs a **fixed** set of upstream
Stockfish's bench positions, kept verbatim, at a fixed depth, with the
transposition table **cleared between positions**, and returns the node total.

Clearing between positions is what makes the total a property of the engine rather
than of the run: carried-over entries would make the result depend on the position
order and on the `Hash` setting, and the anchor would move whenever anyone changed
either.

Output goes to **stderr**: the per-position banner, and the summary block with
`Total time (ms)`, `Nodes searched`, and `Nodes/second`. That matches upstream's
stream choice, and it is why `do_signature` in [`../build.sh`](../build.sh) reads
stderr and discards stdout.

The node total is the repo's anchor. `./build.sh signature` compares it against
[`../tools/signature.golden`](../tools/signature.golden); every change to move
generation, ordering, pruning, or evaluation moves it. **Read the expected value
from that file, never from memory and never from a doc** — which is why no number
for it appears anywhere in this documentation set, and why `./build.sh docs-lint`
fails a page that quotes it.

That anchor is **ccfish's current count, not the target.** The target is
upstream's own `Bench:` for the SHA pinned in
[`../tools/upstream/UPSTREAM_BASE`](../tools/upstream/UPSTREAM_BASE), and the
whole port exists to reach it. The anchor's job is narrower: stop a refactor
changing behaviour silently today. The gate, the trap in regenerating it, and the
distinction are in [07-tooling-ci.md](07-tooling-ci.md).

The bench position set is kept **verbatim** from upstream because it is the position
set the eventual differential comparison runs on. The ported
[`bench_positions.c`](../src/shell/bench_positions.c) holds the same script as pure
data with no search, no stdio and no dependency, and its invariant is that table
identity: changing an entry is a behaviour change that cannot be compared against
upstream afterwards.

`BenchFens` and `BenchFenCount` are exported from `benchmark.h` so other harnesses
can walk the same position set.

The ported [`uci_bench.c`](../src/shell/uci_bench.c) changes one thing that matters:
it runs bench **through the engine's own UCI surface**, handing every script line to
the injected dispatcher a GUI's input also reaches, so the signature measures the
shipped command path rather than a private one that could drift from it.

## What the rewiring commit owes

[`../src/shell/PORT_NOTES_uci.md`](../src/shell/PORT_NOTES_uci.md) is the full list.
The four decisions that are not mechanical:

- **The node count has no publisher.** `uci_bench_run` reads each `go`'s node total
  from `uci_output_last_nodes_searched()`, and **nothing sets it**. The search
  driver must publish the count on completion, exactly as upstream's
  `set_on_update_full` capture does. Until it does, bench through the dispatcher
  totals zero — a silently wrong number, not an error.
- **`engine.h` needs two functions it does not have**: an FEN accessor (so
  `uci_bench.c` stops reaching through `engine_get_position()` and can drop its
  `position.h` include entirely), and `engine_wait_for_search_finished()`, which is
  a no-op while the search is synchronous and becomes required at M4.
- **`START_FEN` is spelled three times** — in `uci.c`, as `UCI_START_FEN` in
  `uci_strings.h`, and as `ENGINE_START_FEN` in `engine.h`. `uci_parse` cannot
  include `engine.h` without dragging `position.h` into a leaf, so collapsing them
  means moving the literal into a header both can reach.
- **`uci_output.c` has no mutex, deliberately.** zfish serialises `printLine` so a
  search thread and the listener cannot tear a line; `build.sh` links no pthread
  into the current binary and the search is single-threaded, so this module has
  none. The funnel is preserved, so adding the lock at M4 is a one-function change
  — but it is a change that must actually happen at M4.

One further deliberate divergence, already decided: `UciInput` bounds its line at
64 KiB and sets `truncated` on overflow, consuming the tail so the next read starts
on a line boundary. Upstream's `std::getline` and zfish's reader are unbounded, and
zfish's returns null on overflow, which makes its caller dispatch `quit`. The
bounded form is strictly better behaved and 64 KiB is far past any reachable
`position ... moves` line.
