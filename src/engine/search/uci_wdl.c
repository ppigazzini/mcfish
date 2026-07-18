#include "uci_wdl.h"

#include <math.h>
#include <stdio.h>

typedef struct {
    double a;
    double b;
} WinRateParams;

// Return the UCI_WinRateModel params for NON-PAWN material, clamped to 17..78.
static WinRateParams win_rate_params(int material) {
    const int clamped = material < 17 ? 17 : (material > 78 ? 78 : material);
    const double m = (double) clamped / 58.0;
    static const double as[4] = { -72.32565836, 185.93832038, -144.58862193, 416.44950446 };
    static const double bs[4] = { 83.86794042, -136.06112997, 69.98820887, 47.62901433 };
    const double a = (((as[0] * m + as[1]) * m + as[2]) * m) + as[3];
    const double b = (((bs[0] * m + bs[1]) * m + bs[2]) * m) + bs[3];
    return (WinRateParams) { a, b };
}

static int win_rate_model(int value, int material) {
    const WinRateParams p = win_rate_params(material);
    return (int) (0.5 + 1000.0 / (1.0 + exp((p.a - (double) value) / p.b)));
}

int uci_wdl_to_cp(int value, int material) {
    const WinRateParams p = win_rate_params(material);
    return (int) round(100.0 * (double) value / p.a);
}

void uci_wdl_text(int value, int material, char *buf, size_t n) {
    const int win = win_rate_model(value, material);
    const int loss = win_rate_model(-value, material);
    const int draw = 1000 - win - loss;
    snprintf(buf, n, "%d %d %d", win, draw, loss);
}

void uci_format_score(uint8_t kind, int value, int extra, char *buf, size_t n) {
    switch (kind) {
    case UCI_SCORE_MATE : {
        // Round away from zero so "mate 1" never prints as "mate 0".
        const int mate = (value > 0 ? value + 1 : value) / 2;
        snprintf(buf, n, "mate %d", mate);
        break;
    }
    case UCI_SCORE_TABLEBASE : {
        const int tb_cp = 20000;
        snprintf(buf, n, "cp %d", (extra != 0 ? tb_cp : -tb_cp) - value);
        break;
    }
    default :
        snprintf(buf, n, "cp %d", value);
        break;
    }
}

void uci_format_info_no_moves(int depth, const char *score_text, char *buf, size_t n) {
    snprintf(buf, n, "info depth %d score %s", depth, score_text);
}

void uci_format_info_full(int depth,
                          int sel_depth,
                          size_t multi_pv,
                          const char *score_text,
                          const char *bound_text,
                          const char *wdl_text,
                          bool show_wdl,
                          uint64_t nodes,
                          uint64_t nps,
                          int hashfull,
                          uint64_t tb_hits,
                          uint64_t time_ms,
                          const char *pv,
                          char *buf,
                          size_t n) {
    snprintf(buf, n,
             "info depth %d seldepth %d multipv %zu score %s%s%s%s%s nodes %llu nps %llu "
             "hashfull %d tbhits %llu time %llu pv %s",
             depth, sel_depth, multi_pv, score_text, bound_text[0] != '\0' ? " " : "", bound_text,
             show_wdl ? " wdl " : "", show_wdl ? wdl_text : "", (unsigned long long) nodes,
             (unsigned long long) nps, hashfull, (unsigned long long) tb_hits,
             (unsigned long long) time_ms, pv);
}

void uci_format_info_iter(
  int depth, const char *currmove, int currmove_number, char *buf, size_t n) {
    snprintf(buf, n, "info depth %d currmove %s currmovenumber %d", depth, currmove,
             currmove_number);
}

void uci_format_bestmove(const char *bestmove, const char *ponder, char *buf, size_t n) {
    if (ponder[0] == '\0')
        snprintf(buf, n, "bestmove %s", bestmove);
    else
        snprintf(buf, n, "bestmove %s ponder %s", bestmove, ponder);
}
