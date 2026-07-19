#include "uci_format.h"

#include "uci_strings.h"

#include <inttypes.h>
#include <math.h>
#include <string.h>

// ---------------------------------------------------------------------------
// The win-rate model
// ---------------------------------------------------------------------------

typedef struct {
    double a;
    double b;
} WinRateParams;

// Fit a and b from the non-pawn-weighted material count. The polynomials are
// the published WDL fit (github.com/official-stockfish/WDL_model); the constants
// are data, not tunables, and every digit is load-bearing for the `wdl` and `cp`
// numbers a GUI displays. Upstream `uci.cpp:520`.
static const double WinRateAs[4] = { -72.32565836, 185.93832038, -144.58862193, 416.44950446 };
static const double WinRateBs[4] = { 83.86794042, -136.06112997, 69.98820887, 47.62901433 };

static WinRateParams win_rate_params(int material) {
    // The fit only uses data for material in [17, 78] and is anchored at 58.
    const int clamped = material < 17 ? 17 : (material > 78 ? 78 : material);
    const double m = (double) clamped / 58.0;

    const double a = (((WinRateAs[0] * m + WinRateAs[1]) * m + WinRateAs[2]) * m) + WinRateAs[3];
    const double b = (((WinRateBs[0] * m + WinRateBs[1]) * m + WinRateBs[2]) * m) + WinRateBs[3];
    return (WinRateParams) { .a = a, .b = b };
}

// Return the win rate in permille, rounded to nearest by the +0.5 then truncate
// that upstream spells. Upstream `uci.cpp:539`.
static int win_rate_model(int value, int material) {
    const WinRateParams p = win_rate_params(material);
    return (int) (0.5 + 1000.0 / (1.0 + exp((p.a - (double) value) / p.b)));
}

int uci_to_cp(int value, int material) {
    const WinRateParams p = win_rate_params(material);

    // Compute the numerator in int, as upstream does, then divide by the double
    // `a`. The narrowing to int after std::round truncates; round has already
    // moved the value to a whole number, so the two agree.
    const int scaled = 100 * value;
    return (int) round((double) scaled / p.a);
}

size_t uci_format_wdl(int value, int material, char *buf, size_t buf_len) {
    const int win = win_rate_model(value, material);
    const int loss = win_rate_model(-value, material);
    const int draw = 1000 - win - loss;

    UciBuf b;
    uci_buf_init(&b, buf, buf_len);
    uci_buf_appendf(&b, "%d %d %d", win, draw, loss);
    return b.len;
}

// ---------------------------------------------------------------------------
// Score and info lines
// ---------------------------------------------------------------------------

size_t uci_format_score(UciScoreKind kind, int value, int extra, char *buf, size_t buf_len) {
    enum { TB_CP = 20000 };

    UciBuf b;
    uci_buf_init(&b, buf, buf_len);

    switch (kind) {
    case UCI_SCORE_MATE : {
        // Plies to moves, rounded away from zero. The positive side adds one
        // before halving; the negative side relies on C's truncation toward
        // zero, which is what upstream's C++ does.
        const int moves = (value > 0 ? value + 1 : value) / 2;
        uci_buf_appendf(&b, "mate %d", moves);
        break;
    }
    case UCI_SCORE_TABLEBASE :
        uci_buf_appendf(&b, "cp %d", (extra != 0 ? TB_CP : -TB_CP) - value);
        break;
    case UCI_SCORE_INTERNAL :
    default :
        uci_buf_appendf(&b, "cp %d", value);
        break;
    }

    return b.len;
}

size_t uci_format_info_full(const UciInfoFull *info, char *buf, size_t buf_len) {
    UciBuf b;
    uci_buf_init(&b, buf, buf_len);

    // Emit the fields in this order and no other: a GUI parses `info` by
    // position after the keyword, so a swap here misreads every line.
    uci_buf_append_str(&b, "info");
    uci_buf_appendf(&b, " depth %d", info->depth);
    uci_buf_appendf(&b, " seldepth %d", info->sel_depth);
    uci_buf_appendf(&b, " multipv %zu", info->multi_pv);
    uci_buf_append_str(&b, " score ");
    uci_buf_append_str(&b, info->score ? info->score : "");

    if (info->bound && info->bound[0] != '\0') {
        uci_buf_append_char(&b, ' ');
        uci_buf_append_str(&b, info->bound);
    }

    if (info->show_wdl) {
        uci_buf_append_str(&b, " wdl ");
        uci_buf_append_str(&b, info->wdl ? info->wdl : "");
    }

    uci_buf_appendf(&b, " nodes %" PRIu64, info->nodes);
    uci_buf_appendf(&b, " nps %" PRIu64, info->nps);
    uci_buf_appendf(&b, " hashfull %d", info->hashfull);
    uci_buf_appendf(&b, " tbhits %" PRIu64, info->tb_hits);
    uci_buf_appendf(&b, " time %" PRIu64, info->time_ms);
    uci_buf_append_str(&b, " pv ");
    uci_buf_append_str(&b, info->pv ? info->pv : "");

    return b.len;
}

size_t uci_format_info_no_moves(int depth, const char *score, char *buf, size_t buf_len) {
    UciBuf b;
    uci_buf_init(&b, buf, buf_len);
    uci_buf_appendf(&b, "info depth %d score %s", depth, score ? score : "");
    return b.len;
}

size_t uci_format_info_iter(
  int depth, const char *currmove, int currmove_number, char *buf, size_t buf_len) {
    UciBuf b;
    uci_buf_init(&b, buf, buf_len);
    uci_buf_appendf(&b, "info depth %d currmove %s currmovenumber %d", depth,
                    currmove ? currmove : "", currmove_number);
    return b.len;
}

size_t uci_format_bestmove(const char *bestmove, const char *ponder, char *buf, size_t buf_len) {
    UciBuf b;
    uci_buf_init(&b, buf, buf_len);
    uci_buf_appendf(&b, "bestmove %s", bestmove ? bestmove : "");
    if (ponder && ponder[0] != '\0')
        uci_buf_appendf(&b, " ponder %s", ponder);
    return b.len;
}

// ---------------------------------------------------------------------------
// Free-form lines
// ---------------------------------------------------------------------------

size_t uci_format_info_string(const char *input, char *buf, size_t buf_len) {
    UciBuf b;
    uci_buf_init(&b, buf, buf_len);

    UciLines lines;
    uci_lines_init(&lines, input, strlen(input));

    const char *line;
    size_t line_len;
    while (uci_lines_next(&lines, &line, &line_len)) {
        const char *trimmed;
        size_t trimmed_len;
        uci_trim_whitespace(line, line_len, &trimmed, &trimmed_len);
        if (trimmed_len == 0)
            continue;

        if (b.len != 0)
            uci_buf_append_char(&b, '\n');
        uci_buf_append_str(&b, "info string ");
        uci_buf_append(&b, line, line_len);
    }

    return b.len;
}

const char *uci_format_help(void) { return UciHelpText; }

size_t uci_format_unknown_command(const char *command, char *buf, size_t buf_len) {
    UciBuf b;
    uci_buf_init(&b, buf, buf_len);
    uci_buf_appendf(&b, "Unknown command: '%s'. Type help for more information.",
                    command ? command : "");
    return b.len;
}

size_t
uci_format_critical_error(const char *command, const char *message, char *buf, size_t buf_len) {
    UciBuf b;
    uci_buf_init(&b, buf, buf_len);
    uci_buf_appendf(&b, "info string CRITICAL ERROR: Command `%s` failed. Reason: %s\n",
                    command ? command : "", message ? message : "");
    return b.len;
}
