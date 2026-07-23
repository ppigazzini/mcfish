#include "search_common.h"

#include "option_source.h"
#include "output_sink.h"
#include "tb_source.h"
#include "time_source.h"

#include "../board/attacks.h"
#include "../board/bitboard.h"
#include "../board/legality.h"
#include "../board/movegen.h"
#include "../eval/evaluate.h"

#include <math.h>
#include <stddef.h>

// ---- injection seams ---------------------------------------------------
//
// The four seam headers declare only the pointers; their storage lives here so
// the whole search zone links with no extra translation unit. Every default is
// the headless answer documented in its header.

static void sink_drop_line(const char *str, size_t len) {
    (void) str;
    (void) len;
}
static bool sink_not_quiet(void) { return false; }
static void sink_ignore_nodes(uint64_t nodes) { (void) nodes; }

void (*OutputPrintLine)(const char *str, size_t len) = sink_drop_line;
bool (*OutputIsQuiet)(void) = sink_not_quiet;
void (*OutputSetLastNodesSearched)(uint64_t nodes) = sink_ignore_nodes;

static int option_zero_by_name(const char *name) {
    (void) name;
    return 0;
}
static int option_zero(void) { return 0; }
static bool option_false(void) { return false; }

int (*OptionIntByName)(const char *name) = option_zero_by_name;
int (*OptionSyzygyProbeDepth)(void) = option_zero;
int (*OptionSyzygyProbeLimit)(void) = option_zero;
bool (*OptionSyzygy50MoveRule)(void) = option_false;

// Provide a deterministic monotonic counter as the headless fallback. Reading a
// real OS clock is a platform service, so the engine cannot do it and stay
// portable; with no platform attached a per-call counter is a valid monotonic
// clock and keeps the run deterministic.
static int64_t HeadlessTicks = 0;
static int64_t time_default_now(void) { return ++HeadlessTicks; }

int64_t (*TimeNowMs)(void) = time_default_now;

static size_t tb_no_tablebases(void) { return 0; }
static TbProbeResult tb_unavailable(const char *fen, size_t fen_len, bool chess960) {
    (void) fen;
    (void) fen_len;
    (void) chess960;
    return (TbProbeResult) { 0, 0, 0, 0, 0 };
}
static TbProbeResult tb_unavailable_pos(Position *pos) {
    (void) pos;
    return (TbProbeResult) { 0, 0, 0, 0, 0 };
}

size_t (*TbMaxCardinality)(void) = tb_no_tablebases;
TbProbeResult (*TbProbeFen)(const char *fen, size_t fen_len, bool chess960) = tb_unavailable;
TbProbeResult (*TbProbeWdlPos)(Position *pos) = tb_unavailable_pos;

// ---- tuned tables ------------------------------------------------------

const int32_t PieceValueByPiece[PIECE_NB] = { 0, 208, 781, 825, 1276, 2538, 0, 0,
                                              0, 208, 781, 825, 1276, 2538, 0, 0 };

const int32_t LmrDivisor[16] = { 3637, 2787, 2761, 2939, 3171, 3347, 3147, 2762,
                                 2772, 3106, 3107, 3060, 3112, 2991, 3090, 3542 };

// ---- value model -------------------------------------------------------

Value search_value_draw(uint64_t nodes) {
    return (Value) (VALUE_DRAW - 1 + (int32_t) (nodes & 0x2));
}

Value search_value_to_tt(Value v, int ply) {
    if (value_is_win(v))
        return (Value) (v + ply);
    if (value_is_loss(v))
        return (Value) (v - ply);
    return v;
}

Value search_value_from_tt(Value v, int ply, int r50c) {
    if (!value_is_valid(v))
        return VALUE_NONE;

    // Handle a TB win or better.
    if (value_is_win(v)) {
        // Downgrade a potentially false mate score.
        if (value_is_mate(v) && VALUE_MATE - v > 100 - r50c)
            return (Value) (VALUE_TB_WIN_IN_MAX_PLY - 1);
        // Downgrade a potentially false TB score.
        if (VALUE_TB - v > 100 - r50c)
            return (Value) (VALUE_TB_WIN_IN_MAX_PLY - 1);
        return (Value) (v - ply);
    }

    // Handle a TB loss or worse.
    if (value_is_loss(v)) {
        if (value_is_mated(v) && VALUE_MATE + v > 100 - r50c)
            return (Value) (VALUE_TB_LOSS_IN_MAX_PLY + 1);
        if (VALUE_TB + v > 100 - r50c)
            return (Value) (VALUE_TB_LOSS_IN_MAX_PLY + 1);
        return (Value) (v + ply);
    }

    return v;
}

Value to_corrected_static_eval(Value v, int cv) {
    const int adjusted = v + cv / 131072;
    const int lo = VALUE_TB_LOSS_IN_MAX_PLY + 1;
    const int hi = VALUE_TB_WIN_IN_MAX_PLY - 1;
    return (Value) (adjusted < lo ? lo : adjusted > hi ? hi : adjusted);
}

// ---- Step 7 / 8 / 9 / 11 -----------------------------------------------

int futility_margin(
  int depth, bool tt_hit, bool improving, bool opponent_worsening, int correction_value) {
    const int capped = 45 + depth * 4 < 85 ? 45 + depth * 4 : 85;
    const int futility_mult = capped - 20 * (int) (!tt_hit);
    const int imp = (int) improving;
    const int opp = (int) opponent_worsening;
    const int abs_corr = correction_value < 0 ? -correction_value : correction_value;
    return futility_mult * depth - (2789 * imp + 335 * opp) * futility_mult / 1024
         + abs_corr / 198435;
}

int futility_return(int beta, int eval) { return (661 * beta + 363 * eval) / 1024; }

int razor_margin(int depth) { return 483 + 318 * depth * depth; }

int null_move_threshold(int beta, int depth, bool improving) {
    return beta - 13 * depth - 47 * (int) improving + 365;
}

int null_move_reduction(int depth) { return 7 + depth / 3; }

int nmp_min_ply_of(int ply, int depth, int r) { return ply + 3 * (depth - r) / 4; }

int probcut_beta(int beta, bool improving) { return beta + 241 - 64 * (int) improving; }

int probcut_beta_deep(int beta) { return beta + 428; }

// ---- Step 14 -----------------------------------------------------------

int move_count_limit(int depth, bool improving) {
    return (3 + depth * depth) / (2 - (int) improving);
}

int history_prune_threshold(int depth) { return -4136 * depth; }

int quiet_futility_value(int static_eval, bool no_best_move, int lmr_depth, bool eval_gt_alpha) {
    return static_eval + 39 + 127 * (int) no_best_move + 119 * lmr_depth + 90 * (int) eval_gt_alpha;
}

int quiet_see_margin(int lmr_depth) { return 23 * lmr_depth * lmr_depth; }

int capture_futility_value(int static_eval, int lmr_depth, int piece_val, int capt_hist) {
    return static_eval + 234 + 247 * lmr_depth + piece_val + 134 * capt_hist / 1024;
}

// upstream e4a635486: drop the max(.., 0) clamp.
int capture_see_margin(int depth, int capt_hist) { return 177 * depth + capt_hist * 34 / 1024; }

// ---- Step 15 -----------------------------------------------------------

// Share abs(correctionValue)/198368 between both singular margins.
static int corr_val_adj(int correction_value) {
    const int a = correction_value < 0 ? -correction_value : correction_value;
    return a / 198368;
}

int singular_beta(int tt_value, bool ttpv_and_not_pv, int depth) {
    return tt_value - (59 + 66 * (int) ttpv_and_not_pv) * depth / 63;
}

int singular_double_margin(
  bool pv_node, bool not_tt_capture, int correction_value, int tt_move_history, bool ply_gt_root) {
    return -2 + 204 * (int) pv_node - 152 * (int) not_tt_capture - corr_val_adj(correction_value)
         - 1175 * tt_move_history / 114178 - (int) ply_gt_root * 38;
}

int singular_triple_margin(
  bool pv_node, bool not_tt_capture, bool ttpv, int correction_value, bool ply_gt_root) {
    return 70 + 279 * (int) pv_node - 188 * (int) not_tt_capture + 81 * (int) ttpv
         - corr_val_adj(correction_value) - (int) ply_gt_root * 43;
}

// ---- Step 16 / 17 ------------------------------------------------------

void search_fill_reductions(int32_t *reductions, size_t count) {
    for (size_t i = 1; i < count; ++i)
        reductions[i] = (int32_t) (2872.0 / 128.0 * log((double) i));
}

const int32_t *search_reductions_table(void) {
    // Fill on first use. The values are a pure function of the index, so a benign
    // race would write byte-identical entries; first use is single-threaded anyway
    // (the main thread's per-worker setup runs before siblings start).
    static int32_t table[MAX_MOVES];
    static bool filled = false;
    if (!filled) {
        search_fill_reductions(table, MAX_MOVES);
        filled = true;
    }
    return table;
}

int reduction_of(const int32_t *reductions,
                 int depth,
                 int move_number,
                 int delta,
                 int root_delta,
                 bool improving) {
    const int reduction_scale = reductions[depth] * reductions[move_number];
    return reduction_scale - delta * 577 / root_delta
         + (!improving ? reduction_scale * 197 / 512 : 0) + 982;
}

int lmr_ttpv_reduction(bool pv_node, bool value_gt_alpha, bool depth_ge, bool cut_node) {
    return 3023 + (int) pv_node * 1004 + (int) value_gt_alpha * 885
         + (int) depth_ge * (816 + (int) cut_node * 940);
}

int lmr_corr_reduction(int correction_value) {
    const int a = correction_value < 0 ? -correction_value : correction_value;
    return a / 26310;
}

int lmr_stat_score_reduction(int stat_score) { return stat_score * 439 / 4096; }

int lmr_all_node_scale(int r, int depth) { return r * 276 / (256 * depth + 268); }

int capture_stat_score(int piece_val, int capture_hist) {
    return 873 * piece_val / 128 + capture_hist;
}

// Upstream reweighted this from `2*main + cont0 + cont1` to a /1024 blend
// (search.cpp). C and C++ both truncate toward zero, so the division needs no
// adjustment -- but it is a division now, and dropping it would silently scale
// every reduction by 1024.
int quiet_stat_score(int main_hist, int cont0, int cont1) {
    return (2252 * main_hist + 1126 * cont0 + 1093 * cont1) / 1024;
}

// ---- post-loop bonuses -------------------------------------------------

int tt_move_history_depth_bonus(int depth) { return -421 - 110 * depth; }

int tt_move_history_match_bonus(bool best_is_tt) { return best_is_tt ? 918 : -747; }

int prior_bonus_scale(
  int prev_stat_score, int depth, bool prev_movecount_gt9, bool cond_a, bool cond_b) {
    const int capped = 59 * depth < 420 ? 59 * depth : 420;
    const int s = -241 - prev_stat_score / 98 + capped + 186 * (int) prev_movecount_gt9
                + 142 * (int) cond_a + 159 * (int) cond_b;
    return s > 0 ? s : 0;
}

int prior_scaled_bonus_base(int depth) {
    const int v = 150 * depth - 85;
    return v < 1337 ? v : 1337;
}

int prior_conthist_scale(int scaled_bonus) { return scaled_bonus * 263 / 16384; }
int prior_mainhist_scale(int scaled_bonus) { return scaled_bonus * 215 / 32768; }
int prior_pawnhist_scale(int scaled_bonus) { return scaled_bonus * 324 / 8192; }

int correction_history_bonus(int eval_delta, int depth, bool has_best_move) {
    const int w = has_best_move ? 12 : 18;
    const int raw = eval_delta * depth * w / 128;
    const int clamped = raw < -256 ? -256 : raw > 256 ? 256 : raw;
    return 1061 * clamped / 1024;
}

int correction_value_blend(int pcv, int micv, int wnpcv, int bnpcv, int cch2, int cch4, bool m_ok) {
    const int cntcv = m_ok ? 8761 * (cch2 + cch4) : 64049;
    return 15341 * pcv + 10569 * micv + 12906 * (wnpcv + bnpcv) + cntcv;
}

int eval_diff(int prev_static_eval, int static_eval) {
    const int raw = -(prev_static_eval + static_eval);
    const int hi = raw < 194 ? raw : 194;
    return (hi > -189 ? hi : -189) + 60;
}

// ---- qsearch blends ----------------------------------------------------

int qsearch_stand_pat_blend(int best_value, int beta) {
    return (441 * best_value + 583 * beta) / 1024;
}

int qsearch_fail_high_blend(int best_value, int beta) {
    return (462 * best_value + 562 * beta) / 1024;
}

int qsearch_futility_base(int static_eval) { return static_eval + 306; }

// ---- aspiration --------------------------------------------------------

int aspiration_initial_delta(size_t thread_idx, int mean_squared_score) {
    const int tmod = (int) (thread_idx % 8);
    const int abs_mss = mean_squared_score < 0 ? -mean_squared_score : mean_squared_score;
    return 5 + tmod + abs_mss / 10193;
}

int aspiration_delta_grow(int delta) { return delta + 47 * delta / 128; }

int optimism_of(int avg) {
    const int abs_avg = avg < 0 ? -avg : avg;
    return 114 * avg / (abs_avg + 85);
}

// ---- board predicates --------------------------------------------------

bool search_pseudo_legal(const Position *pos, Move m) {
    const Color us = pos->side_to_move;
    const Color them = flip_color(us);
    const Square from = move_from(m);
    const Square to = move_to(m);
    const Piece pc = piece_on(pos, from);
    const Bitboard all = pieces(pos);

    // Take the slower but simpler path for non-NORMAL moves: membership in the
    // generator, exactly as upstream does.
    if (move_type(m) != NORMAL) {
        ExtMove list[MAX_MOVES];
        const ExtMove *const end =
          checkers(pos) != 0 ? generate_evasions(pos, list) : generate_non_evasions(pos, list);
        for (const ExtMove *it = list; it != end; ++it)
            if (it->move == m)
                return true;
        return false;
    }

    if (pc == NO_PIECE || color_of_piece(pc) != us)
        return false;
    if ((pieces_c(pos, us) & square_bb(to)) != 0)
        return false;

    if (type_of_piece(pc) == PAWN) {
        // A NORMAL pawn move can never land on a back rank: that would have to be
        // encoded as a PROMOTION.
        if (((rank_bb(7) | rank_bb(0)) & square_bb(to)) != 0)
            return false;

        const int push = us == WHITE ? 8 : -8;
        const bool cap =
          (pawn_attacks_bb(us, square_bb(from)) & pieces_c(pos, them) & square_bb(to)) != 0;
        const bool single = (int) from + push == (int) to && is_empty(pos, to);
        const bool dbl = (int) from + 2 * push == (int) to && relative_rank(us, from) == 1
                      && is_empty(pos, to) && is_empty(pos, (Square) ((int) to - push));

        if (!cap && !single && !dbl)
            return false;
    } else if ((attacks_bb(type_of_piece(pc), from, all) & square_bb(to)) == 0) {
        return false;
    }

    const Bitboard ck = checkers(pos);
    if (ck != 0) {
        if (type_of_piece(pc) != KING) {
            if (bb_more_than_one(ck))  // double check: only the king may move
                return false;
            if ((BetweenBB[king_square(pos, us)][lsb(ck)] & square_bb(to)) == 0)
                return false;
        }
        // No king-safety test here. Upstream's pseudo_legal (position.cpp:748)
        // has no else branch: whether a king move walks into check is legal()'s
        // job, and adding it here changes which MovePicker stage the caller
        // enters for a TT king move.
    }

    return true;
}

bool search_gives_check(const Position *pos, Move m) {
    // The predicate lives in the board zone (legality.c): pos_do_move trusts its
    // gives_check argument to equal it, so every do_move caller — search or not —
    // must reach the one definition.
    return pos_gives_check(pos, m);
}

// ---- node-level primitives ---------------------------------------------

void search_update_sel_depth(SearchCtx *ctx, int ply) {
    if (ctx->sel_depth < ply + 1)
        ctx->sel_depth = ply + 1;
}

Value search_evaluate(SearchCtx *ctx, const Position *pos) {
    // Index optimism by the side to move at THIS node, not at the root: the
    // aspiration loop writes both colours and the sign has to follow the mover
    // down the tree (upstream search.cpp:1867).
    //
    // Branch on the per-go readiness byte, not on the eval zone's global load
    // state plus the arena pointer: the answer cannot change inside a go, and
    // upstream's Worker::evaluate carries no per-node re-test at all.
    if (ctx->eval_nnue_ready)
        return evaluate_nnue_with_optimism(ctx->eval_arena, pos, ctx->optimism[pos->side_to_move]);
    return evaluate_with_optimism(ctx->eval_arena, pos, ctx->optimism[pos->side_to_move]);
}

// ---- history-update family (upstream search.cpp:1928-2030) --------------
//
// Read the search Stack directly, as upstream's update_all_stats /
// update_quiet_histories / update_continuation_histories do; history.c keeps
// only the table primitives. Walking the real stack frames here removes the
// per-call frame gather the old history.c seam copied.

// Test move validity as upstream's Move::is_ok does — by the two reserved
// encodings, not by from != to.
static inline bool hist_move_ok(Move m) { return m != MOVE_NONE && m != MOVE_NULL; }

static inline PieceType hist_piece_type_on(const Position *pos, Square s) {
    return type_of_piece(piece_on(pos, s));
}

// Scale the quiet-history bonus (update_quiet_histories). Each is bonus * N / 1024
// with toward-zero division; the pawn-history scale picks its weight by sign.
static inline int quiet_low_ply_scale(int bonus) { return bonus * 712 / 1024; }
static inline int quiet_cont_scale(int bonus) { return bonus * 750 / 1024; }

static inline int quiet_pawn_scale(int bonus) {
    const int weight = bonus > -4 ? 1104 : 459;
    return bonus * weight / 1024;
}

// Compute the base stat bonus/malus applied at the end of search() when a
// bestMove is found (update_all_stats).
static inline int stat_bonus(int depth, bool is_tt_move, int prev_stat_score) {
    const int base = 133 * depth - 81;
    return (base < 1487 ? base : 1487) + 364 * (int) is_tt_move + prev_stat_score / 28;
}

static inline int stat_malus(int depth) {
    const int base = 968 * depth - 235;
    return base < 2244 ? base : 2244;
}

// Index the continuation-history positive-consistency multipliers by the running
// positive_count in search_update_continuation_histories.
static const int CmhcMultipliers[7] = { 94, 103, 110, 106, 119, 126, 121 };

// Compute the per-entry continuation-history update delta.
//
// Upstream (search.cpp: `bonus * weight * multiplier / 131072`) computes this in
// `int`, so the 3-way product overflows and WRAPS (2's complement on x86 — UB in
// C++ but relied upon). Do the product in uint32_t so the wrap is defined, then
// reinterpret; C23 fixes the signed representation, so the reinterpretation is
// bit-identical to the wrap upstream gets.
static inline int conthist_delta(int bonus, int weight, int positive_count, int i) {
    const int multiplier = CmhcMultipliers[positive_count];
    const uint32_t product = (uint32_t) bonus * (uint32_t) weight * (uint32_t) multiplier;
    return (int32_t) product / 131072 + 73 * (int) (i < 2);
}

typedef struct {
    int i;  // plies back from the walk's base
    int w;
} ConthistBonus;

static const ConthistBonus ConthistBonuses[6] = {
    { 1, 1040 }, { 2, 780 }, { 3, 290 }, { 4, 502 }, { 5, 132 }, { 6, 418 },
};

void search_update_continuation_histories(const Stack *ss, Piece pc, Square to, int bonus) {
    int positive_count = 0;

    for (size_t b = 0; b < 6; ++b) {
        if (ss->in_check && ConthistBonuses[b].i > 2)
            break;

        const Stack *const frame = ss - ConthistBonuses[b].i;
        if (!hist_move_ok(frame->current_move))
            continue;

        SharedStat *entry = &frame->continuation_history[(size_t) pc * SQUARE_NB + (size_t) to];
        if (shared_stat_load(entry) > 0)
            ++positive_count;

        const int delta =
          conthist_delta(bonus, ConthistBonuses[b].w, positive_count, ConthistBonuses[b].i);
        shared_stats_update(entry, delta, 30000);
    }
}

void search_update_quiet_histories(
  SearchCtx *ctx, const Position *pos, const Stack *ss, Move m, int bonus) {
    Histories *const h = ctx->hist;
    const size_t raw = m;
    int16_t *main_entry = &h->main_history[(size_t) pos->side_to_move * HIST_UINT16 + raw];
    const Piece pc = piece_on(pos, move_from(m));
    const Square to = move_to(m);
    SharedStat *pawn_entry =
      &pawn_history_row(h, pos->st->pawn_key)[(size_t) pc * SQUARE_NB + (size_t) to];

    stats_update(main_entry, bonus, 7183);

    if (ss->ply < LOW_PLY_HISTORY_SIZE) {
        int16_t *lowply_entry = &h->low_ply_history[(size_t) ss->ply * HIST_UINT16 + raw];
        stats_update(lowply_entry, quiet_low_ply_scale(bonus), 7183);
    }

    search_update_continuation_histories(ss, pc, to, quiet_cont_scale(bonus));
    shared_stats_update(pawn_entry, quiet_pawn_scale(bonus), 8192);
}

void search_update_all_stats(SearchCtx *ctx,
                             const Position *pos,
                             const Stack *ss,
                             Move best_move,
                             Square prev_sq,
                             const Move *quiets,
                             size_t n_quiets,
                             const Move *captures,
                             size_t n_captures,
                             int depth,
                             Move tt_move,
                             bool pv_node) {
    Histories *const h = ctx->hist;
    const bool is_tt = best_move == tt_move;
    int bonus = stat_bonus(depth, is_tt, (ss - 1)->stat_score);
    const int malus = stat_malus(depth);

    // upstream 645b636df: at non-PV nodes, scale the best-move bonus by the number
    // of searched moves. Match upstream's `bonus += bonus * uint64_t(N) / 256`
    // EXACTLY: the mul/div are UNSIGNED (int promoted to uint64_t), which differs
    // from signed when bonus < 0; the u64 sum narrows back to i32.
    if (!pv_node) {
        const uint64_t n = (uint64_t) (n_quiets + n_captures);
        const uint64_t bu = (uint64_t) (int64_t) bonus;
        bonus = (int32_t) (uint32_t) (bu + bu * n / 256);
    }

    const bool capture_best = search_capture_stage(pos, best_move);

    if (!capture_best) {
        search_update_quiet_histories(ctx, pos, ss, best_move, bonus * 899 / 1024);

        int actual_malus = malus * 1159 / 1024;
        for (size_t i = 0; i < n_quiets; ++i) {
            actual_malus = actual_malus * 921 / 1024;
            search_update_quiet_histories(ctx, pos, ss, quiets[i], -actual_malus);
        }
    } else {
        const Piece moved_pc = piece_on(pos, move_from(best_move));
        const Square to = move_to(best_move);
        stats_update(capture_entry(h, moved_pc, to, hist_piece_type_on(pos, to)),
                     bonus * 1427 / 1024, 10692);
    }

    if (prev_sq != SQ_NONE && (ss - 1)->move_count == 1 + (int) (ss - 1)->tt_hit
        && captured_piece(pos) == NO_PIECE) {
        // Walk from (ss - 1), as upstream's update_continuation_histories(ss - 1, ...).
        search_update_continuation_histories(ss - 1, piece_on(pos, prev_sq), prev_sq,
                                             -malus * 713 / 1024);
    }

    for (size_t j = 0; j < n_captures; ++j) {
        const Move move = captures[j];
        const Piece moved_pc = piece_on(pos, move_from(move));
        const Square to = move_to(move);
        stats_update(capture_entry(h, moved_pc, to, hist_piece_type_on(pos, to)),
                     -malus * 1489 / 1024, 10692);
    }
}

// ---- transposition-table adapter ---------------------------------------

void search_tt_save(
  TTEntry *writer, Key key, Value v, bool is_pv, Bound b, int depth, Move m, Value ev) {
    tt_save(writer, key, v, is_pv, b, depth, m, ev);
}

void search_tt_penalize(TTEntry *writer, int amount) { tt_penalize(writer, amount); }
