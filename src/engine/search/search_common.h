// Own the search's tuned arithmetic: every pruning margin, reduction weight and
// bonus scale the node bodies read, plus the small board predicates the search
// zone still has to carry itself.
//
// The invariant: nothing here is derived, everything is transcribed. Each
// constant is upstream's tuned value and each division truncates toward zero,
// the way C and upstream's C++ both do. A cleaner formulation that moves a
// rounding boundary moves the node count, so a change here is a behaviour change
// even when it looks like a simplification.
//
// Wrapping is deliberate where it is marked. Upstream computes some of these in
// `int` and relies on two's-complement wrap; those spots go through unsigned
// arithmetic and cast back, because signed overflow is undefined in C.
//
// Golden: `Stockfish/src/search.cpp`.

#ifndef MCFISH_SEARCH_COMMON_H
#define MCFISH_SEARCH_COMMON_H

#include "search_types.h"
#include "tt.h"

#include "../board/position.h"
#include "../board/score.h"
#include "../board/types.h"

#include <stdint.h>

// Name the depths that are not depths. DEPTH_QS is the quiescence entry depth;
// the other two are the TT's "no entry" and "entry with no search behind it"
// markers (upstream types.h).
enum : int32_t {
    DEPTH_QS = 0,
    DEPTH_UNSEARCHED = -2,
    DEPTH_NONE = -3,
};

// tt.h carries the same value as DEPTH_ENTRY_OFFSET because it cannot include this
// header without a cycle. Hold the two spellings together.
static_assert(DEPTH_ENTRY_OFFSET == DEPTH_NONE, "the TT depth bias is DEPTH_NONE");

// Index by Piece, not PieceType: the search reads it straight off `board[sq]`,
// where the black pieces sit at 9..14.
extern const int32_t PieceValueByPiece[PIECE_NB];

// Divide the accumulated quiet history by depth bucket in Step 14.
extern const int32_t LmrDivisor[16];

// ---- value model -------------------------------------------------------

static inline bool value_is_valid(Value v) { return v != VALUE_NONE; }
static inline bool value_is_mate(Value v) { return v >= VALUE_MATE_IN_MAX_PLY; }
static inline bool value_is_mated(Value v) { return v <= VALUE_MATED_IN_MAX_PLY; }

// Score a draw off the low bits of the node count, so repeated draw scores in one
// tree are not all identical and cannot form a stable cycle.
Value search_value_draw(uint64_t nodes);

// Re-base a mate or TB score to plies-from-this-node before storing it. Standard
// scores pass through. This is NOT tt.h's value_to_tt: that one only knows the
// mate band, this one covers the tablebase band too.
Value search_value_to_tt(Value v, int ply);

// Invert search_value_to_tt, downgrading potentially false mate / TB scores that
// the 50-move rule or graph-history interaction could invalidate.
Value search_value_from_tt(Value v, int ply, int r50c);

// Shift the raw eval by the correction history and clamp it clear of the decisive
// band, so a corrected eval can never masquerade as a proven win or loss.
Value to_corrected_static_eval(Value v, int cv);

// ---- Step 7 / 8 / 9 / 11: pre-loop pruning -----------------------------

int futility_margin(
  int depth, bool tt_hit, bool improving, bool opponent_worsening, int correction_value);
int futility_return(int beta, int eval);

int razor_margin(int depth);

int null_move_threshold(int beta, int depth, bool improving);
int null_move_reduction(int depth);
int nmp_min_ply_of(int ply, int depth, int r);

int probcut_beta(int beta, bool improving);
int probcut_beta_deep(int beta);

// ---- Step 14: move-loop pruning ----------------------------------------

int move_count_limit(int depth, bool improving);

int history_prune_threshold(int depth);
int quiet_futility_value(int static_eval, bool no_best_move, int lmr_depth, bool eval_gt_alpha);
int quiet_see_margin(int lmr_depth);

int capture_futility_value(int static_eval, int lmr_depth, int piece_val, int capt_hist);
int capture_see_margin(int depth, int capt_hist);

// ---- Step 15: singular extension ---------------------------------------

int singular_beta(int tt_value, bool ttpv_and_not_pv, int depth);
int singular_double_margin(
  bool pv_node, bool not_tt_capture, int correction_value, int tt_move_history, bool ply_gt_root);
int singular_triple_margin(
  bool pv_node, bool not_tt_capture, bool ttpv, int correction_value, bool ply_gt_root);

// ---- Step 16 / 17: reductions ------------------------------------------

// Fill REDUCTIONS[1, count) with int(2872/128 * ln i). Index 0 is left untouched,
// matching upstream's clear().
void search_fill_reductions(int32_t *reductions, size_t count);

// Compute the 1024-scaled base reduction. DELTA is the node's window,
// ROOT_DELTA the root window this iteration searched with.
int reduction_of(
  const int32_t *reductions, int depth, int move_number, int delta, int root_delta, bool improving);

int lmr_ttpv_reduction(bool pv_node, bool value_gt_alpha, bool depth_ge, bool cut_node);
int lmr_corr_reduction(int correction_value);
int lmr_stat_score_reduction(int stat_score);
int lmr_all_node_scale(int r, int depth);

int capture_stat_score(int piece_val, int capture_hist);
int quiet_stat_score(int main_hist, int cont0, int cont1);

// ---- post-loop bonuses -------------------------------------------------

int tt_move_history_depth_bonus(int depth);
int tt_move_history_match_bonus(bool best_is_tt);

int prior_bonus_scale(
  int prev_stat_score, int depth, bool prev_movecount_gt9, bool cond_a, bool cond_b);
int prior_scaled_bonus_base(int depth);
int prior_conthist_scale(int scaled_bonus);
int prior_mainhist_scale(int scaled_bonus);
int prior_pawnhist_scale(int scaled_bonus);

int correction_history_bonus(int eval_delta, int depth, bool has_best_move);

// Blend the six correction reads. The caller resolves the table lookups; only the
// tuned weights live here.
int correction_value_blend(int pcv, int micv, int wnpcv, int bnpcv, int cch2, int cch4, bool m_ok);

// Order quiets by static-eval difference: clamp the negated sum of the previous
// and current static evals into [-189, 194] and bias by 60.
int eval_diff(int prev_static_eval, int static_eval);

// ---- qsearch blends ----------------------------------------------------

int qsearch_stand_pat_blend(int best_value, int beta);
int qsearch_fail_high_blend(int best_value, int beta);
int qsearch_futility_base(int static_eval);

// ---- aspiration --------------------------------------------------------

int aspiration_initial_delta(size_t thread_idx, int mean_squared_score);
int aspiration_delta_grow(int delta);
int optimism_of(int avg);

// ---- board predicates the search zone still carries --------------------

// Mirror upstream Move::is_ok: reject only the two reserved encodings.
static inline bool search_move_ok(Move m) { return m != MOVE_NONE && m != MOVE_NULL; }

// Classify a move the way the continuation-history index and the searched-move
// split do — upstream Position::capture_stage, which folds queen promotions in
// with captures and reads promotion_type unconditionally.
static inline bool search_capture_stage(const Position *pos, Move m) {
    return is_capture(pos, m) || move_promotion(m) == QUEEN;
}

// Test whether M is pseudo-legal for POS. Belongs in the board zone as
// pos_pseudo_legal (PORT_NOTES_search.md §1); carried here until that lands,
// because MovePicker returns the TT move unchecked and pos_legal is only defined
// on pseudo-legal input.
bool search_pseudo_legal(const Position *pos, Move m);

// Test whether M — pseudo-legal for POS — gives check. Belongs in the board zone
// as pos_gives_check; carried here for the same reason.
bool search_gives_check(const Position *pos, Move m);

// ---- node-level primitives ---------------------------------------------

// Raise the reported selective depth to PLY + 1.
void search_update_sel_depth(SearchCtx *ctx, int ply);

// Run the static evaluation for this node.
Value search_evaluate(SearchCtx *ctx, const Position *pos);

// Count the node, make the move, and publish the child's continuation pages on
// SS. CAPTURE is read pre-move, the moved piece post-move — the order matters for
// a promotion, where the piece on `to` is not the piece that left `from`.
void search_do_move(
  SearchCtx *ctx, Position *pos, Move m, StateInfo *st, bool gives_check, Stack *ss);
void search_undo_move(SearchCtx *ctx, Position *pos, Move m);

// Publish the continuation pages for a child on SS without making a move. The
// null move and the iterative-deepening sentinels pass all-zero indices, which
// resolve to the table bases.
void search_set_cont_hist(
  SearchCtx *ctx, Stack *ss, bool in_check, bool capture, Piece pc, Square to);

// ---- history-write adapters --------------------------------------------
//
// history.c must not depend on the Stack layout, so each write gathers the
// fields it reads into a HistoryStack first. `frames[k]` is `(ss - 1 - k)`, so
// gathering from SS gives the walk from SS and `frames + 1` the walk from SS - 1.

HistoryStack search_gather_stack(const Stack *ss);

void search_update_quiet_histories(
  SearchCtx *ctx, const Position *pos, const Stack *ss, Move m, int bonus);

// Apply BONUS along the continuation histories of the walk based at SS.
void search_update_continuation_histories(const Stack *ss, Piece pc, Square to, int bonus);

// ---- transposition-table adapter ---------------------------------------

// Present one probe with the full upstream field set, so the node bodies read the
// same shape they do upstream regardless of what the table stores today.
typedef struct {
    TTEntry *writer;
    bool found;
    Move move;
    Value value;
    Value eval;
    int depth;
    Bound bound;
    bool is_pv;
} TTProbe;

TTProbe search_tt_probe(Key key);
void search_tt_save(
  TTEntry *writer, Key key, Value v, bool is_pv, Bound b, int depth, Move m, Value ev);

// Decrement a useless entry's stored depth, so it stops winning the replacement
// race it cannot justify (upstream 319d61eff).
void search_tt_penalize(TTEntry *writer, int amount);

#endif  // MCFISH_SEARCH_COMMON_H
