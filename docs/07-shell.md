# Shell

Everything under [`src/shell/`](../src/shell): `main`, the UCI loop and option
table, and the benchmark. This is the process — the only zone that reads stdin,
writes stdout, or knows the protocol exists.

Audience: shell contributors. The zone rule this page depends on is in
[00-architecture.md](00-architecture.md).

## The split

The zone is two files with a seam between them, plus the pieces they drive:

- [`engine.c`](../src/shell/engine.c) — **the session** ([`engine.h`](../src/shell/engine.h)).
  It owns what upstream's `engine.cpp` owns: the position and its unbounded state
  chain, the option table with its wired on-change callbacks, the resident net, and
  the search wiring. It holds no stdio and knows no command grammar.
- [`uci.c`](../src/shell/uci.c) — **the transport and the dispatch**
  ([`uci.h`](../src/shell/uci.h)). It reads a line, routes it to an `engine_*` call,
  and prints every byte the engine puts on the wire. It holds no engine state.
- [`main.c`](../src/shell/main.c) — the composition root.
- [`ucioption.c`](../src/shell/ucioption.c) — the typed option table.
- [`benchmark.c`](../src/shell/benchmark.c) and its data
  [`bench_positions.c`](../src/shell/bench_positions.c) — the fixed bench.
- [`syzygy_option.c`](../src/shell/syzygy_option.c) — the Syzygy option delegate.

The seam is the point: `uci.c` parses text and prints text, `engine.c` holds the
state, and neither reaches into the other's job. That is why the option table is
registered once (in `engine.c`) and the stdio funnel lives in one place (`uci.c`).

## main as the composition root

[`main.c`](../src/shell/main.c) initialises the static tables in dependency order,
then enters the UCI loop:

```c
int main(int argc, char **argv) {
    bitboards_init();
    attacks_init();
    threats_init();  // build RayPassBB, which reads the attack tables
    position_init();
    eval_nnue_init();

    uci_loop(argc, argv);
    search_shutdown();
    eval_nnue_shutdown();
    return 0;
}
```

It is the only file permitted to include across every zone, and nothing includes it.
The ordering constraint — and its silent failure mode — is spelled out in
[00-architecture.md](00-architecture.md); the short version is that a `Position`
built before `attacks_init` reads zeroed attack tables and generates no piece
moves, which presents as a search bug.

`uci_loop` announces the engine, installs the transport sinks with
`engine_set_output`, and hands the rest of the wiring to `engine_init`, which owns
the state the wiring targets: it builds the state chain, registers the option table,
clears the search state, points the search at that table, sizes the transposition
table, establishes the start position, resolves the root directory from `argv[0]`,
and loads the net.

Establishing a position before any command matters more than it looks: a `go` or
`d` arriving before any `position` command must operate on the start position, not
on a zeroed `Position` whose `king_square` would `lsb` an empty bitboard — which is
undefined behaviour, not a diagnosable error. See
[01-engine-board.md](01-engine-board.md).

### The net and the root directory

The shell owns the net because it owns the `EvalFile` option; the engine zone
allocates the arenas at startup and never touches the filesystem. `engine.c` keeps
two strings for this: the option value, and the directory the binary was launched
from, derived from `argv[0]`. **That root must carry its trailing separator**,
because `network_load`'s concatenation inserts none.

`engine_report_net()` prints `eval_nnue_status()` through `info string` before every
`go`, `perft` and `eval`, and `engine_verify_network()` **terminates** the process
right after when no usable net is loaded — upstream's five error lines verbatim, from
the same three sites (nnue/network.cpp:165-187). Refusing to run is the honest
answer: a placeholder eval that plays legal moves reads as a strength regression, not
as a missing file. See [03-engine-eval.md](03-engine-eval.md).

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

`uci.c` is the only module in the tree that writes to a stream. `emit_stdout` (the
search/status sink) and `emit_info_string` (the option-message sink) both funnel
through `uci_write`, which is also where the `Debug Log File` tee lives — so there is
one point at which every outgoing byte can be copied to the session log.

## The command loop

`uci_loop` has two modes:

- **Argv mode.** With any argument, the words are joined into one line, executed,
  and the process exits without reading stdin. `./build/mcfish bench 8` and
  `./build/mcfish "go depth 5"` both work, which is what
  [`../build.sh`](../build.sh) relies on for the `bench` and `signature` steps.
- **Interactive mode.** `getline` reads a whole line however long, until `quit` or
  EOF — upstream's unbounded `std::getline` (uci.cpp:106), so a `position ... moves`
  line past any fixed bound is not split across reads and run as fragments.

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
return. This is a gap in the shell: `search_go` blocks the UCI thread, so nothing
reads stdin while a search runs. The thread pool in `src/platform/` is built, gated
and driven by the search, and its shared stop flag is the cross-thread protocol a
listener would use — but the shell still calls the search synchronously rather than
handing it off. See [06-platform.md](06-platform.md).

`ucinewgame` clears the TT but does **not** clear the history block; the live search
clears that per `go` instead, which is not where upstream clears it. See
[02-engine-search.md](02-engine-search.md).

### position

`engine_set_position` (and `engine_set_startpos`) is the single entry point for
establishing a position, and every path in `uci.c`'s `cmd_position` goes through it.
It calls `state_list_reset` before parsing, which returns the chain to its single
root and is what keeps it from accumulating across games.

The chain is an unbounded `StateList`, and deliberately so: `pos_undo_move` and the
repetition scan both follow `StateInfo::previous`, so a state allocated on a
handler's stack would leave the chain pointing into a dead frame the moment the
handler returned. See [01-engine-board.md](01-engine-board.md). Each record is its
own allocation that never moves once handed out, and the list has no bound —
upstream's chain is a deque (engine.cpp:210), so a long analysis line of coordinate
moves does not run into a cap.

The FEN branch reassembles six space-separated fields into one buffer, stopping at
`moves` or end of line, and bounded by the buffer size. `engine_set_position` returns
the parse reason on a malformed record, and `cmd_position` **terminates** on it
(`terminate_on_critical_error`) rather than answering for some other board — the
`errors` golden case in [`../tools/cases/errors.uci`](../tools/cases/errors.uci) pins
what a malformed FEN produces.

Moves are applied one at a time by `engine_play_move`, which parses each token with
`move_from_uci` against the current position's legal moves and writes the move's NNUE
deltas into the position's scratch slots. An illegal move or an exhausted chain is a
**failed command** — `cmd_position` terminates on it, quoting the move, not a silent
truncation of the game.

### go

Limits parsed: `depth`, `movetime`, `wtime`, `btime`, `winc`, `binc`, `movestogo`,
`nodes`, `infinite`, `ponder`, and `perft`. How they become a deadline is in
[02-engine-search.md](02-engine-search.md).

When no limit at all is given, `cmd_go` sets `depth 8`. An unqualified `go` from a
script would otherwise start an unbounded search that nothing can stop.

The valueless keywords (`infinite`, `ponder`) are matched **first** and `continue`
without reading a lookahead token, so they do not swallow the keyword that follows
them (`go infinite depth 2` honours the depth). Every other keyword reads its single
argument only once matched, upstream's shape (uci.cpp:192-225).

`go perft N` runs `engine_perft` with `root = true` and prints the per-move split
through
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
| `NumaPolicy` | the thread pool | **Live.** Chooses the NUMA topology the worker set binds under. See below. |
| `Threads` | the thread pool | **Live.** Rebuilds the worker set. See below. |
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
| `SyzygyPath`, `SyzygyProbeDepth`, `Syzygy50MoveRule`, `SyzygyProbeLimit` | the prober and the root ranker | Live. `SyzygyPath` loads the tables; the other three reach the search through `option_source.h`. |
| `EvalFile` | `eval_nnue_load` | Re-loads the net and reports the outcome. |

**`Threads` is live.** The maximum advertised is upstream's
`max(1024, 4 * hardware_concurrency)` because a narrower one is a different
handshake, and the handshake is what a GUI configures against. The callback
rebuilds the worker set rather than resizing it — a thread must be created on the
NUMA node it will run on — which drops every history table, exactly as upstream's
`ThreadPool::set` does. `NumaPolicy` chooses the topology that rebuild binds
under, and re-applies the current thread count so the change takes effect at once.
Golden: upstream `thread.cpp`, `numa.h`.

**The four Syzygy options are live.** `syzygy_option_install` binds the
`TbMaxCardinality` / `TbProbeFen` / `TbProbeWdlPos` seams in
[`../src/engine/search/tb_source.h`](../src/engine/search/tb_source.h) and the three
option readers in `option_source.h`, and it runs before the options are registered,
so a value set here reaches a prober that is already bound.

What still gates the whole path is the PATH: with no `SyzygyPath` the cardinality
stays 0, the root ranker never probes and the in-search Step 6 never fires. That is
the state `bench` runs in, which is why the anchor is blind to this block. See
[05-tablebases.md](05-tablebases.md).

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

`on_hash`, `on_clear_hash`, `on_threads`, `on_numa_policy`, `on_syzygy`,
`on_debug_log_file` and `on_eval_file` (all in `engine.c`) are the seams through
which a subsystem becomes reachable from a `setoption`. Each returns bare text or
`nullptr`; the transport adds the `info string ` prefix, one line per line, as
upstream's `print_info_string` does. `on_debug_log_file` reaches back into `uci.c`'s
transport through `uci_start_logger`, because the log tees the stream the transport
owns.

**A callback whose subsystem is unported must say so.** Advertising a control that
does nothing, in silence, is the one outcome worse than not advertising it: a GUI
cannot tell a no-op from a working feature. The live callbacks answer for the
opposite reason — their subsystems work, so each reports what it did. `SyzygyPath`
reports the tables it found, `Threads` the worker set it rebuilt, and `NumaPolicy`
the topology it bound under, each of which is upstream's own line.

The live `uci.c` advertises and
handles all four Syzygy options today by delegating to
[`syzygy_option.c`](../src/shell/syzygy_option.c), which holds the values and binds
the engine's `tb_source.h` and `option_source.h` seams. They are registered in the
option table [`ucioption.c`](../src/shell/ucioption.c) owns and emitted **before**
`EvalFile`, which is upstream's order (`Stockfish/src/engine.cpp:125-138`) and what
`tools/handshake.golden` pins.

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

That anchor is **mcfish's current count, not the target.** The target is
upstream's own `Bench:` for the SHA pinned in
[`../tools/upstream/UPSTREAM_BASE`](../tools/upstream/UPSTREAM_BASE), and the
whole port exists to reach it. The anchor's job is narrower: stop a refactor
changing behaviour silently today. The gate, the trap in regenerating it, and the
distinction are in [09-tooling-ci.md](09-tooling-ci.md).

The bench position set is kept **verbatim** from upstream because it is the position
set the eventual differential comparison runs on. The ported
[`bench_positions.c`](../src/shell/bench_positions.c) holds the same script as pure
data with no search, no stdio and no dependency, and its invariant is that table
identity: changing an entry is a behaviour change that cannot be compared against
upstream afterwards.

`BenchFens` and `BenchFenCount` are exported from `benchmark.h` so other harnesses
can walk the same position set. `benchmark.c` drives the run **through the engine's
own UCI surface** (`uci_execute`), handing every script line to the same dispatcher a
GUI's input reaches, so the signature measures the shipped command path rather than a
private one that could drift from it.
