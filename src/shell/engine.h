// Own the engine session: the position and its unbounded state chain, the option
// table with its wired on-change callbacks, the resident net, and the search
// wiring. This is the seam that lets uci.c hold no engine state of its own --
// it parses text, prints text, and drives the calls below.
//
// There is exactly one session. Upstream's Engine is an object a caller can
// instantiate; mcfish runs one engine per process, so the state is file-scope in
// engine.c and every call here is implicitly against it.
//
// Golden: upstream `engine.cpp` (the constructor, its option registration, and
// search_clear), `uci.cpp` (the position/go plumbing).
//
// Upstream: engine.h:47 (class Engine).

#ifndef MCFISH_ENGINE_H
#define MCFISH_ENGINE_H

#include "../engine/board/position.h"
#include "../engine/search/search.h"
#include "ucioption.h"

#include <stddef.h>
#include <stdint.h>

// Build the session: the state chain, the option table in wire order, the search
// output/option seams, an empty transposition table sized from the `Hash`
// default, and the start position. ARGV0 fixes the directory the net loads from.
// Call once, after the engine-zone init tables are built. Installs the sinks that
// were handed to engine_set_output, so call that first.
void engine_init(const char *argv0);

// Release what engine_init acquired. Safe without a prior engine_init.
void engine_shutdown(void);

// Install the two line sinks the session emits through: EMIT_LINE for a search or
// status line (a trailing newline is added by the sink), EMIT_INFO for an option
// on-change message (one `info string` line per line of text). Forwarded to
// search_set_output and options_set_info. Call before engine_init.
void engine_set_output(void (*emit_line)(const char *line), void (*emit_info)(const char *message));

// Reach the option table and the live position for the readers that render or
// inspect them directly.
OptionsMap *engine_options(void);
Position *engine_position(void);

// Set the position from FEN, resetting the state chain. CHESS960 selects the
// castling parse. Return false with *REASON set on a malformed record; the caller
// decides whether to terminate. engine_set_startpos and engine_set_position use
// the live `UCI_Chess960` option; the explicit form is for `flip`, which reads the
// variant off the board being re-parsed, not off the option.
bool engine_set_position(const char *fen, const char **reason);
bool engine_set_startpos(const char **reason);
bool engine_set_position_variant(const char *fen, bool chess960, const char **reason);

// Re-set the board from the colour-reversed form of its own FEN. Silent no-op on a
// FEN that will not flip.
void engine_flip(const char **reason);

// Apply one UCI move, extending the state chain. Return false with *REASON set on a
// move that does not parse or an exhausted chain.
bool engine_play_move(const char *uci_move, const char **reason);

// `ucinewgame`: clear the table and per-game state and reset to the start position.
void engine_new_game(void);

// Search the live position under LIMITS and RETURN immediately; the search runs on
// worker 0's thread and emits its own `bestmove` through the installed sink. The UCI
// loop stays free to read stdin, which is what lets `stop`/`quit`/`ponderhit` be seen
// during a search.
void engine_go(const SearchLimits *limits);

// Block until the running search, if any, has finished. Call before teardown so the
// TT and net are not freed under a search thread still reading them.
void engine_wait(void);

// End a running search for `quit`/EOF/teardown: stop it only if it is unbounded
// (infinite/ponder), otherwise wait it out, then drain. A bounded `go depth N` before
// `quit` still completes; an infinite one cannot hang the exit.
void engine_end_search(void);

// Count the leaves of the legal move tree at DEPTH, printing the per-move split.
uint64_t engine_perft(int depth);

// Request that a running search stop at its next check.
void engine_stop(void);

// Handle `ponderhit`: let a `go ponder` search begin enforcing its time limits.
void engine_ponderhit(void);

// Announce the resident net (or the classical fallback) through the sink, as
// upstream prints before every go/perft/eval.
void engine_report_net(void);

// Terminate the process unless a usable net is loaded, printing upstream's five
// error lines to stderr. Called from the same three sites upstream checks.
void engine_verify_network(void);

// Apply a `setoption` body. Return OPTION_SET_UNKNOWN with *NAME filled when the
// option does not exist, so the caller can print upstream's `No such option` line.
int engine_setoption(const char *args, char name[OPTION_NAME_MAX]);

// Render the option table for the `uci` handshake, into BUF.
void engine_render_options(char *buf, size_t buf_len);

// Read the live position's FEN into BUF (needs >= 128 bytes), render the `d`
// board, and render the evaluation trace.
void engine_current_fen(char *buf, size_t buf_len);
void engine_visualize(char *buf, int buf_len);
void engine_trace_eval(char *buf, int buf_len);

#endif  // MCFISH_ENGINE_H
