// Own the UCI win-rate model and the info-line text.
//
// The internal-eval -> centipawn conversion and the win/draw/loss estimate share
// one win-rate polynomial, so they live together: `to_cp` is the same sigmoid the
// WDL triple is read off, evaluated at a single point. Keep this a leaf — any
// layer must be able to format a score without reaching back into the search.
//
// The formatters write into caller-owned buffers rather than allocating, so the
// emit path can build a line with no allocator and no failure mode.
//
// Golden: `Stockfish/src/uci.cpp` (UCI::to_cp, UCI::wdl, the info line).

#ifndef MCFISH_UCI_WDL_H
#define MCFISH_UCI_WDL_H

#include <stddef.h>
#include <stdint.h>

// Name the three score shapes the classifier feeds the formatter.
enum : uint8_t {
    UCI_SCORE_MATE = 0,
    UCI_SCORE_TABLEBASE = 1,
    UCI_SCORE_CP = 2,
};

// Convert an internal eval to centipawns: normalise by the win-rate `a` param for
// this material count, so "100 cp" means the same win probability at every phase.
int uci_wdl_to_cp(int value, int material);

// Write the "win draw loss" permille triple. The three always sum to 1000.
void uci_wdl_text(int value, int material, char *buf, size_t n);

// Write the UCI score text. KIND selects the shape; for UCI_SCORE_TABLEBASE,
// EXTRA is non-zero for a win.
void uci_format_score(uint8_t kind, int value, int extra, char *buf, size_t n);

// Write "info depth D score S" for the no-legal-moves (mate / stalemate) case.
void uci_format_info_no_moves(int depth, const char *score_text, char *buf, size_t n);

// Write the full per-PV info line. BOUND_TEXT and WDL_TEXT are omitted when empty
// / SHOW_WDL is false, which is what keeps the line byte-identical to upstream's.
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
                          size_t n);

// Write "info depth D currmove M currmovenumber N".
void uci_format_info_iter(
  int depth, const char *currmove, int currmove_number, char *buf, size_t n);

// Write "bestmove M[ ponder P]". PONDER may be empty.
void uci_format_bestmove(const char *bestmove, const char *ponder, char *buf, size_t n);

#endif  // MCFISH_UCI_WDL_H
