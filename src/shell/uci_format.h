// Compose every line the engine puts on the wire: the score spelling, the WDL
// triple, and the `info` lines a GUI parses.
//
// FIELD ORDER IS THE CONTRACT. `info depth / seldepth / multipv / score
// [bound] / [wdl] / nodes / nps / hashfull / tbhits / time / pv` is what
// upstream emits and what every GUI's parser expects positionally; reordering a
// field or dropping one silently misparses. tools/handshake.golden and
// tools/search.golden diff this byte for byte.
//
// This module is a LEAF: std only, no position, no search, no stdio. That is why
// the win-rate model takes `material` as an integer rather than a Position —
// the caller computes `count(PAWN) + 3*count(KNIGHT) + 3*count(BISHOP) +
// 5*count(ROOK) + 9*count(QUEEN)` and passes it in, exactly as upstream's
// win_rate_params derives it (uci.cpp:522). Every renderer writes into caller
// storage and returns the byte count, so no protocol path allocates.
//
// Port source: zfish `engine/search/uci_wdl.zig`, `shell/uci_format.zig`.
// Golden: upstream `uci.cpp:520` (win_rate_params), `uci.cpp:539`
// (win_rate_model), `uci.cpp:553` (format_score), `uci.cpp:572` (to_cp),
// `uci.cpp:583` (wdl), `uci.cpp:637` (on_update_no_moves), `uci.cpp:641`
// (on_update_full), `uci.cpp:664` (on_iter), `uci.cpp:675` (on_bestmove),
// `uci.cpp:684` (terminate_on_critical_error).

#ifndef CCFISH_UCI_FORMAT_H
#define CCFISH_UCI_FORMAT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

enum {
    UCI_SCORE_MAX = 32,       // "cp -2147483648" and "mate -32768" both fit
    UCI_WDL_MAX = 32,         // three permille values, space separated
    UCI_INFO_LINE_MAX = 4096  // a deep PV is the only field that can grow
};

// Name the three score spellings upstream's Score variant carries. The numbering
// matches the port source's `kind` byte (uci_wdl.zig:40) so a caller ported from
// either side reads the same.
typedef enum : uint8_t {
    UCI_SCORE_MATE = 0,       // VALUE is a signed ply distance to mate
    UCI_SCORE_TABLEBASE = 1,  // VALUE is a ply distance; EXTRA != 0 means a win
    UCI_SCORE_INTERNAL = 2,   // VALUE is already in centipawns
} UciScoreKind;

// Convert an internal evaluation to centipawns, normalising by the win-rate
// model's `a` parameter. Port of upstream `UCIEngine::to_cp` (uci.cpp:572).
int uci_to_cp(int value, int material);

// Estimate the win/draw/loss split in permille and render it as "W D L". The
// three always sum to 1000 by construction — draw is the remainder, never
// modelled. Port of upstream `UCIEngine::wdl` (uci.cpp:583).
size_t uci_format_wdl(int value, int material, char *buf, size_t buf_len);

// Render the `score` field's value. For UCI_SCORE_MATE the ply distance becomes
// a distance in MOVES, rounded away from zero: `(plies > 0 ? plies + 1 : plies)
// / 2`, with C's truncating division doing the work on the negative side. A
// mate in 3 plies is "mate 2", not "mate 1". Port of upstream
// `UCIEngine::format_score` (uci.cpp:553).
size_t uci_format_score(UciScoreKind kind, int value, int extra, char *buf, size_t buf_len);

// Carry one full `info` line's fields. `bound` is "lowerbound", "upperbound", or
// empty/nullptr for an exact score; it is emitted immediately after `score` and
// before `wdl`, which is where upstream puts it (uci.cpp:650). `score` and `wdl`
// hold text already rendered by the two functions above.
typedef struct {
    int depth;
    int sel_depth;
    size_t multi_pv;
    const char *score;
    const char *bound;
    const char *wdl;
    bool show_wdl;
    uint64_t nodes;
    uint64_t nps;
    int hashfull;
    uint64_t tb_hits;
    uint64_t time_ms;
    const char *pv;
} UciInfoFull;

// Compose the full per-PV `info` line. Port of upstream
// `UCIEngine::on_update_full` (uci.cpp:641).
size_t uci_format_info_full(const UciInfoFull *info, char *buf, size_t buf_len);

// Compose the short line the search emits when the root has no legal move, so
// there is no PV to report. Port of upstream `UCIEngine::on_update_no_moves`
// (uci.cpp:637).
size_t uci_format_info_no_moves(int depth, const char *score, char *buf, size_t buf_len);

// Compose the per-root-move progress line. Port of upstream `UCIEngine::on_iter`
// (uci.cpp:664).
size_t uci_format_info_iter(
  int depth, const char *currmove, int currmove_number, char *buf, size_t buf_len);

// Compose the search's answer. PONDER is omitted when empty or nullptr — never
// emitted as `ponder (none)`. Port of upstream `UCIEngine::on_bestmove`
// (uci.cpp:675).
size_t uci_format_bestmove(const char *bestmove, const char *ponder, char *buf, size_t buf_len);

// Prefix every non-blank line of INPUT with `info string `, joining the results
// with '\n'. A blank or all-whitespace line is dropped, so a message with a
// trailing newline does not emit an empty `info string`. Port of zfish
// `uci_format.zig:21`.
size_t uci_format_info_string(const char *input, char *buf, size_t buf_len);

// Answer `help`. Returns static storage; never nullptr.
const char *uci_format_help(void);

// Compose the reply to a command the dispatcher does not know.
size_t uci_format_unknown_command(const char *command, char *buf, size_t buf_len);

// Compose the line that precedes exit(1) when a command fails in a way that
// leaves the engine's state unusable. The trailing '\n' is upstream's and is
// part of the line, giving the blank separator line a GUI log shows. Port of
// upstream `UCIEngine::terminate_on_critical_error` (uci.cpp:684).
size_t
uci_format_critical_error(const char *command, const char *message, char *buf, size_t buf_len);

#endif  // CCFISH_UCI_FORMAT_H
