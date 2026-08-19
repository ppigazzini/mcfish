# Shell

Everything under [`src/shell/`](../src/shell): `main`, the UCI loop and option
table, and the benchmark. This is the process — the only zone that reads stdin,
writes stdout, or knows the protocol exists.

Audience: shell contributors. The zone rule this page depends on is in
[00-architecture.md](00-architecture.md).

### `stop` and `ponderhit`: what is adjudicated, and what cannot be

Both have transcript cases (`tools/cases/transcript/stop.uci`, `ponderhit.uci`), and
what those adjudicate is the **piped** form: the gate sends the whole script at once,
so the command overtakes the search and both engines return the same cut-short
answer. That is a real comparison against upstream and it is byte-exact.

It is not the interesting path. A `stop` that lands inside a **running** search ends
it wherever the clock got to, and the node count on the final `info` line moves run to
run — measured at 443388 and 460932 on two consecutive runs of the same binary. No
golden can hold that, and eliding the count would delete the only part that carries
information.

`./build.sh async-check` covers it on invariants instead: exactly one bestmove, that
move legal in the position, the engine still answering `isready`, and `quit` during a
search actually exiting. Those hold whatever the clock did, and they need no reference
— which is why they are the right shape here rather than an mcfish-authored
expectation of upstream's output.

Three of the ten are a different claim: the engine still **answers at all**. An
`export_net` arriving during a live search, a `go movetime 0` — unbounded here,
because zero means absent everywhere below the parse — and a critical error raised
while workers sit inside a tablebase probe. None of them changes an answer; each stops
there being one, and only a deadline the harness owns can tell that from a slow
search.

## The split

The zone is decomposed into single-responsibility modules, each owning one thing and
reaching the others only through a header. No file holds both the state and the
stream, which is the entanglement the split exists to remove.

**The dispatch and the transport:**

- [`uci.c`](../src/shell/uci.c) — **the command dispatch** ([`uci.h`](../src/shell/uci.h)).
  Reads a line, routes it to an `engine_*` call. Holds no engine state and no stream.
- [`uci_output.c`](../src/shell/uci_output.c) — **the stdio funnel and the debug-log
  tee** ([`uci_output.h`](../src/shell/uci_output.h)). The only module in the tree
  that writes to a stream.

**The session, behind the [`engine.h`](../src/shell/engine.h) facade:**

- [`engine.c`](../src/shell/engine.c) — **the session**: the position and its
  unbounded state chain, the search entry points, and the startup sequence that wires
  the rest together.
- [`engine_options.c`](../src/shell/engine_options.c) — **the option table**: the
  wire-order registration and every on-change callback.
- [`engine_nnue.c`](../src/shell/engine_nnue.c) — **the resident net**: the load, the
  status line, and the refuse-to-run check.

**The pieces they drive:**

- [`main.c`](../src/shell/main.c) — the composition root.
- [`ucioption.c`](../src/shell/ucioption.c) — the typed option-table container.
- [`benchmark.c`](../src/shell/benchmark.c) and its data
  [`bench_positions.c`](../src/shell/bench_positions.c) — the fixed bench.
- [`syzygy_option.c`](../src/shell/syzygy_option.c) — the Syzygy option delegate.

`engine.h` is a facade: `uci.c` drives one seam (`engine_*`), and `engine.c` forwards
option and net calls to `engine_options.c` and `engine_nnue.c` behind it, so the
dispatch never learns there are three modules there.

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
    search_shutdown();
    eval_nnue_shutdown();
    return 0;
}
```

`eval_nnue_init()` is deliberately **not** called here. It allocates the eval
arena, and the host's arena source is not registered until `engine_init` runs,
inside `uci_loop` — calling it any earlier hands the arena to the plain-malloc
fallback and then frees it through `page_free` once `engine_init` rewires
`ArenaFree`, corrupting the free. `engine_init` calls it itself, right after the
arena source goes in.

It is the only file permitted to include across every zone, and nothing includes it.
The ordering constraint — and its silent failure mode — is spelled out in
[00-architecture.md](00-architecture.md); the short version is that a `Position`
built before `attacks_init` reads zeroed attack tables and generates no piece
moves, which presents as a search bug.

`uci_loop` announces the engine, installs the transport sinks with
`engine_set_output`, and hands the rest of the wiring to `engine_init`. Its own
first act is installing the host's seams — `search_set_arena_source`,
`search_set_time_source` — **before anything that allocates or reads a clock**;
only then does it build the NNUE feature tables and eval arena
(`eval_nnue_init`), hand the engine a real OS-thread worker set
(`worker_pool_install`), build the state chain, bind the tablebase seams,
register the option table, clear the search state, point the search at that
table, size the transposition table, establish the start position, resolve the
root directory from `argv[0]`, and load the net. Getting the seam order wrong is
the exact class of bug `worker_pool_install` and `eval_nnue_init`'s ordering
comment call out: a block obtained from one allocator and released through
another. See [00-architecture.md](00-architecture.md) and
[06-platform.md](06-platform.md) for what each seam supplies.

Establishing a position before any command matters more than it looks: a `go` or
`d` arriving before any `position` command must operate on the start position, not
on a zeroed `Position` whose `king_square` would `lsb` an empty bitboard — which is
undefined behaviour, not a diagnosable error. See
[01-engine-board.md](01-engine-board.md).

### The net and the root directory

The shell owns the net because it owns the `EvalFile` option; the engine zone
allocates the arenas at startup and never touches the filesystem.
[`engine_nnue.c`](../src/shell/engine_nnue.c) keeps two strings for this: the file it
last tried to load, and the directory the binary was launched from, derived from
`argv[0]`. **That root must carry its trailing separator**, because `network_load`'s
concatenation inserts none.

`engine_nnue_report()` prints `eval_nnue_status()` through `info string` before every
`go`, `perft` and `eval`, **followed by one `Network replica N:` line per replica**,
which upstream emits right after the same summary. The count and the shape are
upstream's; the STATUS deliberately is not. Upstream maps the net system-wide and
answers `Shared memory.`, while mcfish holds one network in ordinary process memory
and answers `Local memory.` — one of upstream's own four strings, and the true one
here until the network registers itself for NUMA replication
([04-multithreading.md](04-multithreading.md)). Matching the byte would assert
something false about the allocation. `engine_nnue_verify()` **terminates** the
process
right after when no usable net is loaded — upstream's five error lines verbatim, from
the same three sites (nnue/network.cpp:165-187). Those five lines go through the same
`info`-prefixing sink as every option-message callback (installed by
`engine_nnue_set_info`, wired from `engine_set_output`), landing on stdout, not raw
stderr. Refusing to run is the honest answer: a placeholder eval that plays legal
moves reads as a strength regression, not as a missing file. See
[03-engine-eval.md](03-engine-eval.md).

## The output sink

The engine zone never calls `printf`. `search_go` and `perft` emit through a
function pointer the shell installs — `uci_output_emit_line`, which writes the line
and a newline through the one funnel and tees it to the log:

```c
// src/shell/uci_output.c
void uci_output_emit_line(const char *line) {
    uci_output_write(line);
    uci_output_write("\n");
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

[`uci_output.c`](../src/shell/uci_output.c) is the only module in the tree that
writes to a stream. `uci_output_emit_line` (the search/status sink) and
`uci_output_emit_info` (the option-message sink) both funnel through
`uci_output_write`, which is also where the `Debug Log File` tee lives — so there is
one point at which every outgoing byte can be copied to the session log. `uci.c`
installs those two sinks on the session with `engine_set_output`.

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
| `ucinewgame` | Clear the transposition table, the history block and the tablebase mapping. It does **not** touch the position: upstream's `Engine::search_clear` clears search state only, so a GUI that sends `ucinewgame` between `position` and `go` keeps the board it set. |
| `position` | `startpos` or `fen <fields>`, then any remaining tokens as moves. Anything else — including a bare `position` — returns without touching the board. |
| `go` | Emit the processor/thread `info string` lines and the net-status line, parse limits, hand the search to worker 0's thread, and return; the search emits its own `info` and `bestmove` lines through the sink. A perft is chosen after the whole line is parsed and on the VALUE of `perft`, so `go perft 0` is an ordinary search. |
| `setoption` | `setoption name <NAME> [value <VALUE>]`; see the table below. |
| `stop` | Raise the stop flag and return; the search thread ends and emits its `bestmove`. |
| `quit` | End the search (stop it if unbounded, else wait it out), leave the loop; `uci_loop` frees the table. |
| `flip` | Mirror the position color-flipped, via `engine_flip`. |
| `d` | Print the ASCII board, the FEN, and the Zobrist key via `pos_pretty`, then the `Tablebases WDL:`/`DTZ:` lines when the position is small enough and has no castling rights. The key printed is the **rule50-adjusted** one (`pos_adjust_key50_of`), which is what upstream's `Position::key()` returns; below a halfmove clock of 14 the adjustment is the identity, which is why every bench position and every golden case saw the raw key agree. See [05-tablebases.md](05-tablebases.md). |
| `bench` | `bench <tt> <threads> <limit> <fen file> <limit type>`, each field defaulting when the line runs dry. The limit type is upstream's own set — `depth`, `nodes`, `movetime`, `perft`, `eval` — and becomes the command run per position, so `perft` and `eval` are not searches at all. |
| `speedtest [threads] [ttSize] [seconds]` | Replay five real games — 258 positions, [`../src/shell/speedtest_positions.c`](../src/shell/speedtest_positions.c) — at a per-ply movetime scaled so the whole run lasts the requested time (default: the host's core count, 128 MiB per thread, 150 s), then report throughput. Not `bench`: that one fixes a **depth** and its node total is the anchor; this one fixes a **time** and reports nodes per second, which no golden can pin. Every line goes to **stderr**, and the search is silenced for the duration (option messages are not, as upstream leaves them). Gated for shape by `./build.sh speedtest-check`. **Each of the three arguments is clamped to the range the thing it feeds accepts, and the clamp is reported**: `desiredTimeS * 1000` and `TT_SIZE_PER_THREAD * threads` are both `int` multiplications on a number a user typed, and both overflowed — the first making the scale factor negative so every `go movetime` got a negative argument, the second emitting `setoption name Hash value -84901888`, which the option table refused while the run measured whatever `Hash` was already set. The `User invocation` echo still shows what was typed; `Filled invocation` shows what ran. |
| `eval` | Print the evaluation trace via `evaluate_trace`. |
| `compiler` | Print the clang or gcc version and `__STDC_VERSION__` the binary was built with. |
| `ponderhit` | Clear the ponder flag, so a `go ponder` search begins enforcing its time limits. |
| `export_net [file]` | Write the resident net back out as a `.nnue`, through `engine_export_net`. One token of argument, as upstream reads it, so a path with a space in it is cut at the space. With no argument the net goes to the build's default name, which is refused unless the resident net **is** that one — otherwise a net loaded under another name would overwrite the default file. Refused with `Failed to export a net. No network file is currently loaded.` when none is resident, which mcfish reaches more often than upstream: upstream embeds a net and this port embeds none. Gated by `./build.sh net-roundtrip`. |
| `help`, `--help`, `license`, `--license` | Print upstream's blurb, byte for byte — including its blank first and last lines, and its own name. The text is a citation of the project this is a clone of and points at that project's README; the identity line `uci_loop` prints is the one place the port names itself. |
| a blank line, or one whose first word starts with `#` | Nothing at all — not even the drain. Upstream hangs every arm off `!token.empty() && token[0] != '#'`, so a commented script can be piped at the engine; the shell returns before `engine_end_search`, so a comment between two `go` lines cannot end a search either. |
| anything else | Print `Unknown command: '<line>'. Type help for more information.`, quoting the **whole line** as typed — indent included — not the first word. |

The `help` that last row names is a command that answers, which it was not until
`tools/cases/shell.uci` went in: the message advertised a handler that did not exist,
so typing `help` printed the unknown-command message about itself.

`go` runs the search **off the UCI thread**. `go_line` hands the search to worker 0's
OS thread through `search_go_start` and returns, so the loop keeps reading stdin
while the search runs. That is what lets the four during-search commands — `stop`,
`quit`, `isready`, `ponderhit` — be seen and answered mid-search; `stop` raises the
pool's shared stop flag (the cross-thread protocol described in
[06-platform.md](06-platform.md)) and the search thread ends and emits its
`bestmove`. So `go infinite` now has an interruption path and terminates on `stop`.

Every **other** command drains the running search first (`engine_wait` at the top of
`execute`, before the dispatch). A command either mutates state the search reads
(`position`, `setoption`, `ucinewgame`) or prints output that must land after the
search's lines (`d`, `eval`, the net banner `go` itself prints), so it waits the
search out rather than racing it — which also keeps a batch of back-to-back `go`
lines each running to completion, in order. Only the four during-search commands skip
that drain.

`quit` (and the EOF that substitutes it when a GUI vanishes) must not hang, so it does
not blindly wait: an **unbounded** search — `go infinite`, or a `go ponder` still
pondering — is *stopped*, while a bounded one (`go depth N`, `go movetime T`, …) is
*waited out* so its output stays complete and deterministic. `search_running_unbounded`
draws that line. The bench and the tests reach the search through the synchronous
`search_go`, which is `search_go_start` immediately followed by `search_wait`.

`ucinewgame` reaches `search_clear`, which is where the history block is cleared —
upstream clears it in `ThreadPool::clear`, from `ucinewgame` and nowhere else. This
page recorded the opposite for as long as the live search cleared it per `go`; a
search that starts from a blank history searches a different tree than upstream's.
See [02-engine-search.md](02-engine-search.md).

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

**`cmd_position` reads the line the way upstream reads it: token by token, with one
branch per shape and no keyword search.** `startpos` sets the start position and then
consumes exactly ONE more token *whatever it is* — the `moves` keyword if there is
one — and everything after that is a move. `fen` accumulates words until it meets
`moves`. Anything else, including no argument at all, returns without touching the
board. Two shapes follow from the first rule and are not obvious:

- `position startpos fen <FEN>` is an **error**, not a position: `fen` is eaten as
  the keyword and the FEN's own words are read as moves, so it terminates on
  `Illegal move: <first field>`.
- `position moves e2e4` is ignored entirely, because `moves` matches neither branch.

The FEN branch reassembles the fields into one buffer, stopping at `moves` or end of
line, and bounded by the buffer size. **A separator follows every field, including
the last**, which is upstream's `fen += token + " "` and is load-bearing rather than
cosmetic: `pos_set_reason` reads the buffer as a character stream, so a record cut
short after the side-to-move field ends in whitespace there and is accepted, where
the same text without the trailing separator fails "Expected whitespace after side to
move". The two parsers must be handed the same bytes to be comparable at all. See
[01-engine-board.md](01-engine-board.md).

`engine_set_position` returns
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

Before parsing limits, `go_line` calls `engine_report_threads()`, emitting
`Available processors: <topology>` (from `worker_pool_numa_config_string`) and
`Using N thread(s)`, the latter optionally suffixed ` with NUMA node thread
binding: <split>` when `worker_pool_thread_binding_string` reports binding in
effect. `engine_report_net()` follows later, after the parse. **`go perft N` gets
the first two as well**: upstream prints them on the `go` line, before it looks at
an argument and before it chooses between a search and a perft, so the choice
cannot affect them.

This page previously recorded the opposite — that `go perft` "prints none of this"
— because mcfish emitted them after the argument loop, which the perft arm returns
from. That was a divergence documented as a design, and it survived because
`chess960.golden` and `perft.golden` were photographs of mcfish rather than of
upstream. `./build.sh golden-audit` is what settled it.

`go_line` takes an `announce` flag for that first call, and **bench passes false**.
Upstream's bench does not go through its command loop at all — it parses the limits
itself and calls `perft`/`engine.go` directly — so a bench position emits neither
line. `uci_bench_go` is the entry point that reproduces that split.

Limits parsed — **upstream's whole grammar, with nothing left out**: `searchmoves`,
`wtime`, `btime`, `winc`, `binc`, `movestogo`, `depth`, `nodes`, `movetime`, `mate`,
`perft`, `infinite`, `ponder`. How they become a deadline is in
[02-engine-search.md](02-engine-search.md). `searchmoves` consumes the rest of the
line and is matched first for that reason; each token is lower-cased and resolved
against the position the `go` is about, and anything `move_from_uci` cannot resolve
is dropped, which is why no legality filter is needed downstream.

**There is no default depth.** Upstream's limit fields are plain integers and every
test of them is a truthiness check, so an absent limit and a zero limit are the same
thing and neither bounds the depth loop: a bare `go`, `go nodes 0`, `go depth 0` and
`go movetime 0` all run until `stop`. mcfish capped these at depth 8 — defensible
only while `go` ran on the UCI thread, where nothing could interrupt an unbounded
search, and a divergence from the moment the search moved onto worker 0.
`search_running_unbounded` covers the no-limit case for exactly that reason, or
`quit` would wait forever on a search that never ends.

**Each argument parses at the width of the field it goes into**, through
`go_value_int` and `go_value_u64`. `is >> field` fails when the text does not fit the
FIELD's type and upstream turns that failbit into a critical error on the next line,
so the width is observable from a GUI: `int` for `depth`/`perft`, and for
`mate`/`movestogo` a width check followed by a BOUND (below),
`TimePoint` (int64) for the four clocks, `u64` for `nodes` — read the way a C++
stream reads one, where a leading minus is accepted and the magnitude wraps, so `go
nodes -1` is a budget of `UINT64_MAX`. `go depth 3000000000` is `Invalid argument for
'depth'` and terminates; `go nodes 18446744073709551615` searches.

**`movestogo` and `mate` are bounded where they enter too**, by the same shape as the
clocks and for the same reason: both are read into an `int` and both reach arithmetic
with no room for that type's extremes. `2 * limits_mate` is a signed overflow at
`INT_MAX` and, worse, wraps to `-2` so the stop condition can never hold — `go mate
2147483647` ran past a mate it already had, to depth 245, where `go mate 1` stops at
depth 1. `movestogo` needs a clock to reach: timeman widens it to `int64_t`, so
upstream's `mtg - 1` is safe here and only the product with an increment at the 1e12
bound overflows. The bounds are each field's own domain — `movestogo` counts moves, so
a negative is not a shorter time control and `0` is what timeman already reads as
absent; `mate` is halved because the condition doubles it. Outside the bound is
reported on an `info string`, never silently taken.

**A clock is bounded where it enters**, after that width check and without moving
it. The width is the accept/reject boundary a GUI can observe, so it stays what
upstream's failbit defines; the VALUE is then clamped to `[0, 1e12]` ms — about 31
years — and a clamp is reported, never silently taken. Without it `go wtime
4000000000000000000 winc 4000000000000000000 btime 1000` is signed overflow inside
`timeman`, which is not a defect in that arithmetic: it is asked to hold a number
the protocol never had a right to send, and the parser is the only place that knows
the number came from outside. The divergence is a clock between 1e12 ms and the
overflow threshold, where upstream is still defined — a time control no game has.

The valueless keywords (`infinite`, `ponder`) are matched **first** and `continue`
without reading a lookahead token, so they do not swallow the keyword that follows
them (`go infinite depth 2` honours the depth). Every other keyword reads its single
argument only once matched, upstream's shape (uci.cpp:192-225).

`go perft N` runs `engine_perft` with `root = true` and prints the per-move split
through
the sink, then a `Nodes searched:` total on stdout. That total is what
`./build.sh perft` greps.

**The perft is a dispatch, not a keyword arm.** Upstream parses every argument first
and only then tests `if (limits.perft)`, so a ZERO is not a perft at all but an
ordinary search with no limit, and a bad argument later on the line is still an
error: `go perft 2 depth zzz` terminates. Running the divide from inside the argument
loop, on the keyword's mere presence, made `go perft 0` print a depth-zero divide of
every root move — the same shape a GUI generates when a configured perft depth sits
at its default.

## The option table

There is one, [`ucioption.c`](../src/shell/ucioption.c): typed options with kind,
default, bounds, current value and an on-change callback.
[`engine_options.c`](../src/shell/engine_options.c)'s `engine_options_register` fills
it with upstream's full set, `uci.c`'s `cmd_uci` renders it via
`engine_render_options`, and `cmd_setoption` hands the whole command body to
`engine_setoption`.

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
| `Debug Log File` | `uci_output_start_logger` | Tees the session to a file, input lines prefixed `>> ` and output `<< `, as upstream's `Tie` streambuf does. Exits when the path cannot be opened. |
| `NumaPolicy` | the thread pool | **Live.** Chooses the NUMA topology the worker set binds under. See below. |
| `Threads` | the thread pool | **Live.** Rebuilds the worker set. See below. |
| `Hash` | `tt_resize` | A failed allocation is handled in two different places on purpose. Via `setoption` it is **recoverable**: `on_hash` reports through the info listener and the session continues on whatever table it had. At startup `engine_init` treats it as **fatal**, as upstream does (`tt.cpp:181`) — `tt_resize` leaves the one-cluster fallback installed, so continuing would mean answering UCI with every probe colliding in a single cluster: a silently much weaker engine rather than a broken one. |
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

Both callbacks report through the same two strings `go` itself prints (see
*go* above): `worker_pool_numa_config_string()` renders `Available processors:
<topology>`, and `worker_pool_thread_binding_string()` renders the conditional
` with NUMA node thread binding: <split>` suffix on `Using N thread(s)`
(`thread_allocation_string` in `engine_options.c`). An invalid `NumaPolicy`
value is refused with upstream's wording verbatim: `NumaPolicy: invalid value
'<v>', keeping previous config.` — the previous topology stays installed rather
than the engine degrading to no topology.

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
`on_debug_log_file` and `on_eval_file` (all in `engine_options.c`) are the seams through
which a subsystem becomes reachable from a `setoption`. Each returns bare text or
`nullptr`; the transport adds the `info string ` prefix, one line per line, as
upstream's `print_info_string` does. `on_debug_log_file` reaches back into `uci.c`'s
transport through `uci_output_start_logger`, because the log tees the stream the transport
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
Stockfish's bench positions, kept verbatim, at a fixed depth, and returns the node
total. The transposition table, the history block and the per-game manager scalars
are cleared **once**, by a single `ucinewgame` before the first position, and then
CARRY across every position in the run — clearing per position would be a
different search and a different number. Golden: `Stockfish/src/benchmark.cpp:430`
(`setup_bench`) and `Stockfish/src/uci.cpp:243` (`UCIEngine::bench`).

That single clear is still what makes the total a property of the engine rather
than of the run: it is the same script every time, so the same TT/history state
enters position 1 on every run, and the anchor moves only when a search change
would move it — never because of run-to-run carry-over noise.

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
distinction are in [10-tooling-ci.md](10-tooling-ci.md).

The bench position set is kept **verbatim** from upstream because it is the position
set the eventual differential comparison runs on. The ported
[`bench_positions.c`](../src/shell/bench_positions.c) holds the same script as pure
data with no search, no stdio and no dependency, and its invariant is that table
identity: changing an entry is a behaviour change that cannot be compared against
upstream afterwards.

`BenchDefaults` and `BenchDefaultsCount` are exported from `bench_positions.h` so other
harnesses can walk the same script. `benchmark.c` drives the run **through the engine's
own UCI surface** (`uci_execute`), handing every script line to the same dispatcher a
GUI's input reaches, so the signature measures the shipped command path rather than a
private one that could drift from it.
