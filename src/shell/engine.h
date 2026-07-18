// Own the engine session: the position and its state chain, the option table,
// and the entry points the UCI loop drives.
//
// This is the seam. Everything a `uci` command needs to touch lives behind one
// of the calls below, so `uci.c` parses text and prints text and holds no engine
// state of its own. The state chain is the invariant that forces the ownership
// here: `pos_undo_move` and the repetition scan follow `StateInfo::previous`, so
// every StateInfo a game reaches must outlive the command that created it —
// which rules out a caller-owned position on a handler's stack.
//
// There is exactly one session. Upstream's Engine is an object a caller can
// instantiate; ccfish runs one engine per process, so the state is file-scope in
// engine.c and every call below is implicitly against it.
//
// Port source: zfish `shell/engine.zig`, `shell/engine/object.zig`,
// `shell/engine/options.zig`, `shell/engine/session.zig`, `shell/engine/control.zig`.
// Golden: upstream `engine.cpp:60` (the constructor and its option registration),
// `engine.h:47` (the public surface).

#ifndef CCFISH_ENGINE_H
#define CCFISH_ENGINE_H

#include "../engine/board/position.h"
#include "../engine/search/search.h"
#include "ucioption.h"

#include <stddef.h>
#include <stdint.h>

#define ENGINE_START_FEN "rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1"

// Build the session: register the option table in wire order, size the
// transposition table from the `Hash` default, and set the start position, so a
// `go` or `d` arriving before any `position` command operates on a legal board.
// Call once, after bitboards_init and position_init.
void engine_init(void);

// Release what engine_init acquired. Safe to call without a prior engine_init.
void engine_shutdown(void);

// Install the line sink. Everything this module and the search emit goes through
// it, including the option table's on-change messages, so the shell decides the
// stream and the flushing. Forwarded to search_set_output. Lines arrive without
// a trailing newline and already carry their `info string ` prefix where they
// need one.
void engine_set_output(void (*emit)(const char *line));

// Reach the option table — for `options_render` on the handshake and for
// `options_setoption` on a `setoption` line.
OptionsMap *engine_get_options(void);

// Reach the live position, for the callers that read it directly to render a
// move or a board.
Position *engine_get_position(void);

// Set the position from FEN and reset the state chain. Return false and fall
// back to the start position when the record is malformed, so the position is
// never left unspecified. Reads `UCI_Chess960` to select the castling parse.
bool engine_set_position(const char *fen);

// Apply one move in UCI notation to the live position, extending the state
// chain. Return false when the move does not parse against the current position
// or when the chain is full.
bool engine_play_move(const char *uci_move);

// Search the live position under LIMITS and return the outcome.
SearchResult engine_go(const SearchLimits *limits);

// Count the leaves of the legal move tree at DEPTH, printing the per-move split
// through the sink.
uint64_t engine_perft(int depth);

// Request that a running search stop at its next check.
void engine_stop(void);

// Clear the transposition table and any per-game search state. This is what
// `ucinewgame` and the `Clear Hash` button both run. Port of upstream
// `Engine::search_clear` (engine.cpp).
void engine_search_clear(void);

// Resize the transposition table to MB megabytes. Emit an `info string` through
// the sink when the allocation fails, rather than failing silently.
void engine_set_tt_size(size_t mb);

// Report the transposition table's fill in permille.
int engine_get_hashfull(void);

// Render the live position as `d` prints it, and as the evaluation trace.
void engine_visualize(char *buf, int buf_len);
void engine_trace_eval(char *buf, int buf_len);

#endif  // CCFISH_ENGINE_H
