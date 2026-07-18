// Parse the argument text of the UCI commands that carry one: `go` and
// `position`.
//
// Parsing is TOTAL: no input crashes it, and no input is rejected by a return
// path the caller must handle mid-scan. A `go` argument that will not convert is
// recorded in `bad_token` — naming the KEYWORD, not the offending value, because
// that is what upstream's `is.fail()` check after the if-chain reports
// (`go depth abc` yields "Invalid argument for 'depth'"). The caller decides
// whether to terminate on it.
//
// Storage is fixed. Every result field is an inline array and an input that
// overruns one sets `truncated` rather than allocating or overwriting; a
// truncated `position` fails at the move parser, which is the honest answer.
//
// setoption is NOT here: `options_setoption` (ucioption.h:108) already owns that
// parse, name-joining and all, and duplicating it would give the option table two
// front doors that could drift.
//
// Port source: zfish `shell/uci_parse.zig`. Golden: upstream `uci.cpp:186`
// (parse_limits), `uci.cpp:485` (UCIEngine::position).

#ifndef CCFISH_UCI_PARSE_H
#define CCFISH_UCI_PARSE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

enum {
    UCI_FEN_MAX = 128,           // the longest legal FEN record is well under this
    UCI_MOVES_MAX = 8192,        // '\n'-joined move list; five bytes a ply
    UCI_SEARCHMOVES_MAX = 4096,  // '\n'-joined, lower-cased
    UCI_KEYWORD_MAX = 16,        // "movestogo" is the longest `go` keyword
};

// Carry one parsed `go`. The widths are upstream's: time and increments are
// milliseconds in i64, the counters are i32, and the node limit is u64 because
// a GUI may legitimately ask for more nodes than an i64 holds.
typedef struct {
    int64_t wtime;
    int64_t btime;
    int64_t winc;
    int64_t binc;
    int32_t movestogo;
    int32_t depth;
    int32_t mate;
    int32_t perft;
    int32_t infinite;
    int64_t movetime;
    uint64_t nodes;
    uint8_t ponder_mode;

    // Hold the `searchmoves` list, one lower-cased move per line, '\n'
    // separated, with no trailing separator. Empty when the keyword was absent.
    char searchmoves[UCI_SEARCHMOVES_MAX];
    bool truncated;

    // Name the keyword whose argument would not parse, or "" when every
    // argument parsed. Parsing stops at the first such keyword, as upstream's
    // terminate-on-fail does.
    char bad_token[UCI_KEYWORD_MAX];
} ParsedLimits;

// Carry one parsed `position`. `ok` is false when the command names neither
// `startpos` nor `fen`, which upstream answers by returning without touching the
// board.
typedef struct {
    bool ok;
    char fen[UCI_FEN_MAX];
    // Hold the move list, one UCI move per line, '\n' separated, with no
    // trailing separator. Empty when `moves` was absent or had no argument.
    char moves[UCI_MOVES_MAX];
    bool truncated;
} ParsedPosition;

// Parse the whole `go` line, or just its arguments — a leading `go` token is not
// required and not special-cased, because no `go` keyword collides with one.
// Always fills *OUT; check `bad_token[0]` for a rejected argument.
void uci_parse_limits(const char *input, size_t len, ParsedLimits *out);

// Parse a `position` line. A leading `position` token is consumed when present,
// so both a whole command line and its argument tail parse the same — which is
// what lets the bench driver feed back the command scripts it composes. Return
// `out->ok`.
bool uci_parse_position(const char *input, size_t len, ParsedPosition *out);

// Convert a whole token, base 10, rejecting an empty token, a stray byte, and an
// out-of-range value. A leading '+' or '-' is accepted for the signed forms;
// `uci_parse_u64` rejects '-' outright rather than wrapping.
bool uci_parse_i64(const char *token, size_t len, int64_t *out);
bool uci_parse_i32(const char *token, size_t len, int32_t *out);
bool uci_parse_u64(const char *token, size_t len, uint64_t *out);

#endif  // CCFISH_UCI_PARSE_H
