#include "history.h"

#include "../board/position.h"
#include "../board/types.h"

#include <stdint.h>

static Histories Tables;

Histories *histories(void) { return &Tables; }

// Test move validity as upstream's Move::is_ok does — by the two reserved
// encodings, not by from != to. Any other move with from == to would be a bug
// elsewhere, and treating it as "not ok" here would hide it.
static inline bool is_ok(Move m) { return m != MOVE_NONE && m != MOVE_NULL; }

static inline PieceType piece_type_on(const Position *pos, Square s) {
    return type_of_piece(piece_on(pos, s));
}

// Scale the quiet-history bonus (update_quiet_histories). Each is bonus * N / 1024
// with toward-zero division; the pawn-history scale picks its weight by sign.
static inline int quiet_low_ply_scale(int bonus) { return bonus * 663 / 1024; }
static inline int quiet_cont_scale(int bonus) { return bonus * 820 / 1024; }

static inline int quiet_pawn_scale(int bonus) {
    const int weight = bonus > -7 ? 1038 : 525;
    return bonus * weight / 1024;
}

// Compute the base stat bonus/malus applied at the end of search() when a
// bestMove is found (update_all_stats).
static inline int stat_bonus(int depth, bool is_tt_move, int prev_stat_score) {
    const int base = 134 * depth - 79;
    return (base < 1572 ? base : 1572) + 382 * (int) is_tt_move + prev_stat_score / 30;
}

static inline int stat_malus(int depth) {
    const int base = 1005 * depth - 205;
    return base < 2218 ? base : 2218;
}

// Index the continuation-history positive-consistency multipliers by the running
// positive_count in update_continuation_histories.
static const int CmhcMultipliers[7] = { 96, 113, 101, 105, 127, 121, 126 };

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
    return (int32_t) product / 131072 + 71 * (int) (i < 2);
}

typedef struct {
    int i;  // plies back from the walk's base
    int w;
} ConthistBonus;

static const ConthistBonus ConthistBonuses[6] = {
    { 1, 1040 }, { 2, 780 }, { 3, 300 }, { 4, 537 }, { 5, 129 }, { 6, 423 },
};

void history_clear(Histories *h) {
    // Worker tables: mainHistory=-5, captureHistory=-699, ttMoveHistory=0,
    // continuationCorrectionHistory=5, continuationHistory=-552.
    for (size_t i = 0; i < COLOR_NB * HIST_UINT16; ++i)
        h->main_history[i] = -5;
    for (size_t i = 0; i < PIECE_NB * SQUARE_NB * HIST_PIECE_TYPE_NB; ++i)
        h->capture_history[i] = -699;
    h->tt_move_history = 0;
    for (size_t i = 0; i < HIST_PIECETO * HIST_PIECETO; ++i)
        h->continuation_correction_history[i] = 5;
    for (size_t i = 0; i < 2 * 2 * HIST_PIECETO * HIST_PIECETO; ++i)
        h->continuation_history[i] = -552;

    // Shared tables, through the fill constants history.h declares: the striped clear a
    // worker runs over its own slice of the same tables must use the same values.
    int16_t *corr = (int16_t *) &h->correction_history[0][0];
    const size_t corr_entries = CORRECTION_HISTORY_SIZE * COLOR_NB * 4;
    for (size_t i = 0; i < corr_entries; ++i)
        corr[i] = CORRECTION_HISTORY_FILL;
    for (size_t i = 0; i < PAWN_HISTORY_SIZE * HIST_PIECETO; ++i)
        h->pawn_history[i] = PAWN_HISTORY_FILL;

    // low_ply_history is refilled per search by history_fill_low_ply, never here.
}

void history_age_main(Histories *h) {
    for (size_t i = 0; i < COLOR_NB * HIST_UINT16; ++i) {
        const int v = h->main_history[i];
        h->main_history[i] = (int16_t) (v * 789 / 1024);  // upstream 3c858c19e: drop the +5
    }
}

void history_fill_low_ply(Histories *h) {
    for (size_t i = 0; i < LOW_PLY_HISTORY_SIZE * HIST_UINT16; ++i)
        h->low_ply_history[i] = 100;
}

void history_update_continuation(
  const ContHistFrame *frames, bool in_check, Piece pc, Square to, int bonus) {
    int positive_count = 0;

    for (size_t b = 0; b < 6; ++b) {
        if (in_check && ConthistBonuses[b].i > 2)
            break;

        const ContHistFrame *frame = &frames[b];
        if (!is_ok(frame->current_move))
            continue;

        int16_t *entry = &frame->continuation_history[(size_t) pc * SQUARE_NB + (size_t) to];
        if (*entry > 0)
            ++positive_count;

        const int delta =
          conthist_delta(bonus, ConthistBonuses[b].w, positive_count, ConthistBonuses[b].i);
        stats_update(entry, delta, 30000);
    }
}

void history_update_quiet(
  Histories *h, const Position *pos, Key pawn_key, const HistoryStack *hs, Move move, int bonus) {
    const size_t raw = move;
    int16_t *main_entry = &h->main_history[(size_t) pos->side_to_move * HIST_UINT16 + raw];
    const Piece pc = piece_on(pos, move_from(move));
    const Square to = move_to(move);
    int16_t *pawn_entry = &pawn_history_row(h, pawn_key)[(size_t) pc * SQUARE_NB + (size_t) to];

    stats_update(main_entry, bonus, 7183);

    if (hs->ply < LOW_PLY_HISTORY_SIZE) {
        int16_t *lowply_entry = &h->low_ply_history[(size_t) hs->ply * HIST_UINT16 + raw];
        stats_update(lowply_entry, quiet_low_ply_scale(bonus), 7183);
    }

    history_update_continuation(hs->frames, hs->in_check, pc, to, quiet_cont_scale(bonus));
    stats_update(pawn_entry, quiet_pawn_scale(bonus), 8192);
}

void history_update_all_stats(
  Histories *h, const Position *pos, Key pawn_key, const HistoryStack *hs, const HistoryStats *st) {
    const bool is_tt = st->best_move == st->tt_move;
    int bonus = stat_bonus(st->depth, is_tt, hs->prev_stat_score);
    const int malus = stat_malus(st->depth);

    // upstream 645b636df: at non-PV nodes, scale the best-move bonus by the number
    // of searched moves. Match upstream's `bonus += bonus * uint64_t(N) / 256`
    // EXACTLY: the mul/div are UNSIGNED (int promoted to uint64_t), which differs
    // from signed when bonus < 0; the u64 sum narrows back to i32.
    if (!st->pv_node) {
        const uint64_t n = (uint64_t) (st->n_quiets + st->n_captures);
        const uint64_t bu = (uint64_t) (int64_t) bonus;
        bonus = (int32_t) (uint32_t) (bu + bu * n / 256);
    }

    const bool capture_best =
      is_capture(pos, st->best_move) || move_promotion(st->best_move) == QUEEN;

    if (!capture_best) {
        history_update_quiet(h, pos, pawn_key, hs, st->best_move, bonus * 824 / 1024);

        int actual_malus = malus * 1136 / 1024;
        for (size_t i = 0; i < st->n_quiets; ++i) {
            actual_malus = actual_malus * 956 / 1024;
            history_update_quiet(h, pos, pawn_key, hs, st->quiets[i], -actual_malus);
        }
    } else {
        const Piece moved_pc = piece_on(pos, move_from(st->best_move));
        const Square to = move_to(st->best_move);
        stats_update(capture_entry(h, moved_pc, to, piece_type_on(pos, to)), bonus * 1366 / 1024,
                     10692);
    }

    if (st->prev_sq != SQ_NONE && hs->prev_move_count == 1 + (int) hs->prev_tt_hit
        && captured_piece(pos) == NO_PIECE) {
        // Walk from (ss - 1): frames + 1 is (ss - 2) .. (ss - 7).
        history_update_continuation(hs->frames + 1, hs->prev_in_check, piece_on(pos, st->prev_sq),
                                    st->prev_sq, -malus * 683 / 1024);
    }

    for (size_t j = 0; j < st->n_captures; ++j) {
        const Move move = st->captures[j];
        const Piece moved_pc = piece_on(pos, move_from(move));
        const Square to = move_to(move);
        stats_update(capture_entry(h, moved_pc, to, piece_type_on(pos, to)), -malus * 1518 / 1024,
                     10692);
    }
}

void history_update_correction(Histories *h,
                               const Position *pos,
                               Color us,
                               const CorrectionKeys *keys,
                               const HistoryStack *hs,
                               int bonus) {
    stats_update(&corr_bundle(h, keys->pawn, us)->pawn, bonus, CORRECTION_HISTORY_LIMIT);
    stats_update(&corr_bundle(h, keys->minor, us)->minor, bonus * 152 / 128,
                 CORRECTION_HISTORY_LIMIT);
    stats_update(&corr_bundle(h, keys->non_pawn[WHITE], us)->nonpawn_white, bonus * 186 / 128,
                 CORRECTION_HISTORY_LIMIT);
    stats_update(&corr_bundle(h, keys->non_pawn[BLACK], us)->nonpawn_black, bonus * 186 / 128,
                 CORRECTION_HISTORY_LIMIT);

    const Move m = hs->frames[0].current_move;
    if (is_ok(m)) {
        const Square to = move_to(m);
        const size_t idx = (size_t) piece_on(pos, to) * SQUARE_NB + (size_t) to;
        stats_update(&hs->cont_corr[0][idx], bonus * 136 / 128, CORRECTION_HISTORY_LIMIT);
        stats_update(&hs->cont_corr[1][idx], bonus * 68 / 128, CORRECTION_HISTORY_LIMIT);
    }
}
