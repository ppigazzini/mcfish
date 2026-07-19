#include "evaluate.h"

#include "../board/board_props.h"
#include "../search/uci_wdl.h"
#include "nnue/network.h"
#include "nnue/nnue_accumulator.h"
#include "nnue/nnue_feature.h"
#include "nnue/nnue_ft.h"
#include "nnue/nnue_weight_storage.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// ---------------------------------------------------------------------------
// NNUE runtime state
// ---------------------------------------------------------------------------

// Own the two arenas ONE worker evaluates through. Only the feature-transformer blob
// is shared, and it is read-only (nnue/PORT_NOTES_accumulator.md §6); everything here
// is a running diff of one recursion's board and must never be shared between
// workers.
//
// `acc_depth` counts the plies pushed above the root slot, so a push can never run
// past the stack. The search's `ply >= MAX_PLY` guard already bounds it below
// NNUE_MAX_STACK_SIZE; the check exists so a caller that loses that guard degrades to
// a stale evaluation rather than writing outside the arena.
//
// `scratch_dp` / `scratch_dts` absorb the diff of a ply that could not be pushed, so
// eval_acc_push always returns writable records and its callers need no branch. They
// are per-arena for the same reason the stack is: two workers sharing one scratch
// record would interleave their overflow plies.
struct EvalArena {
    NnueAccumulatorStack *acc_stack;
    NnueRefreshCache *refresh_cache;
    size_t acc_depth;
    DirtyPiece scratch_dp;
    DirtyThreats scratch_dts;
};

// Hold the arena every caller with no worker of its own evaluates through.
static EvalArena *DefaultArena = nullptr;

static bool NetLoaded = false;
static char StatusMessage[512] = "";

// Assert the contract PORT_NOTES_accumulator.md states: the board zone writes
// through DirtyPiece / DirtyThreats and the accumulator reads the same bytes back
// as NnueDirtyPiece / NnueDirtyThreats, so the two pairs must be byte-identical
// views. Pin every field the decoders address.
static_assert(sizeof(DirtyPiece) == sizeof(NnueDirtyPiece), "DirtyPiece must be NnueDirtyPiece");
static_assert(alignof(DirtyPiece) == alignof(NnueDirtyPiece), "DirtyPiece must stay unaligned");
static_assert(offsetof(DirtyPiece, pc) == offsetof(NnueDirtyPiece, pc), "");
static_assert(offsetof(DirtyPiece, from) == offsetof(NnueDirtyPiece, from), "");
static_assert(offsetof(DirtyPiece, to) == offsetof(NnueDirtyPiece, to), "");
static_assert(offsetof(DirtyPiece, remove_sq) == offsetof(NnueDirtyPiece, remove_sq), "");
static_assert(offsetof(DirtyPiece, add_sq) == offsetof(NnueDirtyPiece, add_sq), "");
static_assert(offsetof(DirtyPiece, remove_pc) == offsetof(NnueDirtyPiece, remove_pc), "");
static_assert(offsetof(DirtyPiece, add_pc) == offsetof(NnueDirtyPiece, add_pc), "");
static_assert(sizeof(DirtyThreats) == sizeof(NnueDirtyThreats),
              "DirtyThreats must be NnueDirtyThreats");
static_assert(offsetof(DirtyThreats, list_values) == offsetof(NnueDirtyThreats, list.values), "");
static_assert(offsetof(DirtyThreats, list_size) == offsetof(NnueDirtyThreats, list.size), "");
static_assert(offsetof(DirtyThreats, us) == offsetof(NnueDirtyThreats, us), "");
static_assert(offsetof(DirtyThreats, prev_ksq) == offsetof(NnueDirtyThreats, prev_ksq), "");
static_assert(offsetof(DirtyThreats, ksq) == offsetof(NnueDirtyThreats, ksq), "");

const char *eval_nnue_default_file_name(void) { return NETWORK_DEFAULT_EVAL_FILE_NAME; }

// Allocate N bytes at NNUE_ALIGN. aligned_alloc requires a size that is a
// multiple of the alignment, so round up rather than pass the exact figure.
static void *alloc_arena(size_t n) {
    const size_t rounded = (n + NNUE_ALIGN - 1) / NNUE_ALIGN * NNUE_ALIGN;
    void *p = aligned_alloc(NNUE_ALIGN, rounded);
    if (p != nullptr)
        memset(p, 0, rounded);
    return p;
}

EvalArena *eval_arena_create(void) {
    EvalArena *const arena = calloc(1, sizeof *arena);
    if (arena == nullptr)
        return nullptr;

    arena->acc_stack = alloc_arena(nnue_accumulator_stack_bytes());
    arena->refresh_cache = alloc_arena(nnue_refresh_cache_bytes());
    if (arena->acc_stack == nullptr || arena->refresh_cache == nullptr) {
        eval_arena_destroy(arena);
        return nullptr;
    }
    return arena;
}

void eval_arena_destroy(EvalArena *arena) {
    if (arena == nullptr)
        return;
    free(arena->acc_stack);
    free(arena->refresh_cache);
    free(arena);
}

EvalArena *eval_default_arena(void) {
    if (DefaultArena == nullptr)
        DefaultArena = eval_arena_create();
    return DefaultArena;
}

bool eval_nnue_init(void) {
    // Build the feature index tables first: every accumulator refresh reads them,
    // and they are zero rather than garbage beforehand, so a missing call shows up
    // as an all-zero feature set instead of a crash.
    nnue_feature_init();

    snprintf(StatusMessage, sizeof StatusMessage,
             "NNUE evaluation is not in use: no network file has been loaded, so the "
             "classical placeholder evaluation is active");

    return eval_default_arena() != nullptr;
}

void eval_nnue_shutdown(void) {
    eval_arena_destroy(DefaultArena);
    DefaultArena = nullptr;
    NetLoaded = false;
    nnue_weight_storage_free();
}

bool eval_nnue_load(const char *root_directory, const char *evalfile_path) {
    NetLoaded = false;

    EvalArena *const arena = eval_default_arena();
    if (arena == nullptr) {
        snprintf(StatusMessage, sizeof StatusMessage,
                 "NNUE evaluation is unavailable: the accumulator arenas could not be "
                 "allocated, so the classical placeholder evaluation is active");
        return false;
    }

    const size_t root_len = root_directory == nullptr ? 0 : strlen(root_directory);
    const size_t name_len = evalfile_path == nullptr ? 0 : strlen(evalfile_path);

    network_load(root_directory, root_len, evalfile_path, name_len);

    const NetworkVerifyResult result = network_verify(evalfile_path, name_len);

    // Diverge from upstream here on purpose: Network::verify calls exit(EXIT_FAILURE)
    // on a net it could not load (network.cpp:188). mcfish must keep playing on the
    // classical fallback instead, so report the failure and carry on.
    if (result.should_exit) {
        snprintf(StatusMessage, sizeof StatusMessage,
                 "NNUE network %s was not loaded; falling back to the classical placeholder "
                 "evaluation. Set the EvalFile option to the full path of the net.",
                 name_len == 0 ? NETWORK_DEFAULT_EVAL_FILE_NAME : evalfile_path);
    } else {
        snprintf(StatusMessage, sizeof StatusMessage, "%s",
                 result.message == nullptr ? "NNUE evaluation using an unnamed network"
                                           : result.message);
        NetLoaded = true;
    }
    network_free_message(result.message);

    if (NetLoaded) {
        // Seed every refresh entry from the freshly loaded transformer biases, then
        // drop any accumulator state that describes the previous net.
        const NnueFeatureTransformer *ft =
          (const NnueFeatureTransformer *) (const void *) nnue_ft_ptr();
        nnue_clear_refresh_cache(arena->refresh_cache, nnue_ft_biases(ft));
    }
    eval_acc_reset(arena);

    return NetLoaded;
}

bool eval_nnue_available(void) { return NetLoaded; }

const char *eval_nnue_status(void) { return StatusMessage; }

// ---------------------------------------------------------------------------
// Accumulator bracketing
// ---------------------------------------------------------------------------

void eval_arena_clear_refresh_cache(EvalArena *arena) {
    if (!NetLoaded || arena == nullptr)
        return;

    const NnueFeatureTransformer *const ft =
      (const NnueFeatureTransformer *) (const void *) nnue_ft_ptr();
    nnue_clear_refresh_cache(arena->refresh_cache, nnue_ft_biases(ft));
}

void eval_acc_reset(EvalArena *arena) {
    if (arena == nullptr)
        return;
    arena->acc_depth = 0;
    nnue_acc_stack_reset(arena->acc_stack);
}

void eval_acc_push(EvalArena *arena, DirtyPiece **dp, DirtyThreats **dts) {
    if (arena->acc_depth + 2 <= (size_t) NNUE_MAX_STACK_SIZE) {
        const NnueStackPushOutput out = nnue_acc_stack_push(arena->acc_stack);
        ++arena->acc_depth;
        // Hand the arena slot straight to the make-move. The two record pairs are
        // byte-identical by contract (static_asserts above), which is what lets the
        // board zone write through its own names.
        *dp = (DirtyPiece *) (void *) out.dirty_piece;
        *dts = (DirtyThreats *) (void *) out.dirty_threats;
        return;
    }

    // Absorb the overflow ply in this arena's own scratch. Per-arena, not file-scope:
    // a shared pair would let two workers interleave their overflow plies.
    *dp = &arena->scratch_dp;
    *dts = &arena->scratch_dts;
    arena->scratch_dts.list_size = 0;
}

void eval_acc_pop(EvalArena *arena) {
    if (arena->acc_depth != 0) {
        nnue_acc_stack_pop(arena->acc_stack);
        --arena->acc_depth;
    }
}

// ---------------------------------------------------------------------------
// NNUE scaling
// ---------------------------------------------------------------------------

// Bound the evaluation away from the tablebase range, as upstream's
// VALUE_TB_WIN_IN_MAX_PLY does. mcfish has no tablebase sentinels yet, so derive
// the same figure from VALUE_MATE and MAX_PLY rather than pin a literal.
enum { EVAL_TB_WIN_IN_MAX_PLY = VALUE_MATE_IN_MAX_PLY - MAX_PLY - 1 };

static int64_t abs64(int64_t v) { return v < 0 ? -v : v; }

// Blend the network's two terms with optimism and material, then damp for the
// halfmove clock. Every divide is truncating, and every intermediate is widened to
// int64 before the multiply — both are load-bearing for bit-exactness.
//
// Golden: Stockfish/src/evaluate.cpp:48-67.
static Value
nnue_scaled_value(const Position *pos, int32_t psqt, int32_t positional, int optimism) {
    int64_t nnue = (int64_t) psqt + (int64_t) positional;

    const int64_t complexity = abs64((int64_t) psqt - (int64_t) positional);
    int64_t opt = (int64_t) optimism;
    opt += opt * complexity / 476;
    nnue -= nnue * complexity / 18236;

    const int64_t material = 534 * (count_p(pos, WHITE, PAWN) + count_p(pos, BLACK, PAWN))
                           + pos_non_pawn_material(pos, WHITE) + pos_non_pawn_material(pos, BLACK);

    int64_t v = (nnue * (77871 + material) + opt * (7191 + material)) / 77871;

    v -= v * pos->st->rule50 / 199;

    const int64_t lo = -(int64_t) EVAL_TB_WIN_IN_MAX_PLY + 1;
    const int64_t hi = (int64_t) EVAL_TB_WIN_IN_MAX_PLY - 1;
    v = v < lo ? lo : v > hi ? hi : v;

    return (Value) v;
}

// ---------------------------------------------------------------------------
// Classical fallback
//
// This is NOT upstream's evaluation. It runs only when no net is resident, so a
// netless build still plays legal, ordered chess. It is scaffolding to be deleted
// once NNUE is the only path (AGENTS.md); do not tune it or extend it.
// ---------------------------------------------------------------------------

// Score each piece type per square from White's point of view, indexed by the
// square as seen by the piece's owner (flip_rank for Black). Values are small
// relative to piece_value: they order candidate squares, they do not decide
// material. The pawn table's push gradient and the king's back-rank preference
// are what keep the placeholder eval from shuffling.
static const int8_t PSQT[PIECE_TYPE_NB][SQUARE_NB] = {
    [PAWN] = {
          0,   0,   0,   0,   0,   0,   0,   0,
         -5,  -3,  -3, -10, -10,  -3,  -3,  -5,
         -5,  -2,   0,   5,   5,   0,  -2,  -5,
         -3,   0,   5,  12,  12,   5,   0,  -3,
          0,   3,  10,  20,  20,  10,   3,   0,
         10,  15,  25,  35,  35,  25,  15,  10,
         40,  45,  50,  55,  55,  50,  45,  40,
          0,   0,   0,   0,   0,   0,   0,   0,
    },
    [KNIGHT] = {
        -50, -30, -20, -20, -20, -20, -30, -50,
        -30, -15,   0,   5,   5,   0, -15, -30,
        -20,   5,  12,  16,  16,  12,   5, -20,
        -20,   2,  16,  20,  20,  16,   2, -20,
        -20,   5,  16,  20,  20,  16,   5, -20,
        -20,   2,  12,  16,  16,  12,   2, -20,
        -30, -15,   0,   2,   2,   0, -15, -30,
        -50, -30, -20, -20, -20, -20, -30, -50,
    },
    [BISHOP] = {
        -20, -10, -10, -10, -10, -10, -10, -20,
        -10,   6,   0,   0,   0,   0,   6, -10,
        -10,  10,  10,  10,  10,  10,  10, -10,
        -10,   0,  10,  10,  10,  10,   0, -10,
        -10,   5,   5,  10,  10,   5,   5, -10,
        -10,   0,   5,  10,  10,   5,   0, -10,
        -10,   0,   0,   0,   0,   0,   0, -10,
        -20, -10, -10, -10, -10, -10, -10, -20,
    },
    [ROOK] = {
          0,   0,   2,   5,   5,   2,   0,   0,
         -5,   0,   0,   0,   0,   0,   0,  -5,
         -5,   0,   0,   0,   0,   0,   0,  -5,
         -5,   0,   0,   0,   0,   0,   0,  -5,
         -5,   0,   0,   0,   0,   0,   0,  -5,
         -5,   0,   0,   0,   0,   0,   0,  -5,
         10,  15,  15,  15,  15,  15,  15,  10,
          0,   0,   2,   5,   5,   2,   0,   0,
    },
    [QUEEN] = {
        -20, -10, -10,  -5,  -5, -10, -10, -20,
        -10,   0,   2,   0,   0,   0,   0, -10,
        -10,   2,   2,   2,   2,   2,   0, -10,
          0,   0,   2,   2,   2,   2,   0,  -5,
         -5,   0,   2,   2,   2,   2,   0,  -5,
        -10,   0,   2,   2,   2,   2,   0, -10,
        -10,   0,   0,   0,   0,   0,   0, -10,
        -20, -10, -10,  -5,  -5, -10, -10, -20,
    },
    [KING] = {
         20,  30,  10,   0,   0,  10,  30,  20,
         20,  20,   0,   0,   0,   0,  20,  20,
        -10, -20, -20, -20, -20, -20, -20, -10,
        -20, -30, -30, -40, -40, -30, -30, -20,
        -30, -40, -40, -50, -50, -40, -40, -30,
        -30, -40, -40, -50, -50, -40, -40, -30,
        -30, -40, -40, -50, -50, -40, -40, -30,
        -30, -40, -40, -50, -50, -40, -40, -30,
    },
};

// Sum material and placement for C, from C's point of view.
static Value evaluate_side(const Position *pos, Color c) {
    Value v = 0;

    for (PieceType pt = PAWN; pt <= KING; ++pt) {
        Bitboard b = pieces_cp(pos, c, pt);
        v += piece_value(pt) * popcount_bb(b);

        while (b) {
            const Square s = pop_lsb(&b);
            v += PSQT[pt][c == WHITE ? s : flip_rank(s)];
        }
    }

    // Reward the bishop pair: two bishops are worth more than the sum of two.
    if (count_p(pos, c, BISHOP) >= 2)
        v += 50;

    return v;
}

static Value evaluate_classical(const Position *pos) {
    const Color us = pos->side_to_move;
    const Value v = evaluate_side(pos, us) - evaluate_side(pos, flip_color(us));

    // Add a small tempo bonus so the eval is not symmetric under a null move,
    // which would otherwise make null-move pruning see a free score.
    return v + 28;
}

// ---------------------------------------------------------------------------
// Entry points
// ---------------------------------------------------------------------------

Value evaluate_with_optimism(EvalArena *arena, const Position *pos, int optimism) {
    // The classical placeholder produces neither network half, so there is nothing
    // for optimism to scale against; it is scaffolding to be deleted and never
    // grows a blend of its own.
    if (!NetLoaded || arena == nullptr)
        return evaluate_classical(pos);

    const NnueEvalOutput out = network_evaluate(pos, arena->acc_stack, arena->refresh_cache);
    return nnue_scaled_value(pos, out.psqt, out.positional, optimism);
}

// Evaluate with no search bias, through the default arena. Upstream's own value at
// `eval` and in the trace.
Value evaluate(const Position *pos) { return evaluate_with_optimism(eval_default_arena(), pos, 0); }

// Write one table cell: a sign column, then the magnitude in pawns right-aligned
// in six columns. The sign is read off the raw internal value, NOT off the pawn
// figure -- a value that survives the normalisation as a rounded 0.00 still prints
// its own sign, and reading the sign off the rounded double would print '+' for a
// negative eval. Port of upstream `nnue/nnue_misc.cpp` format_cp_aligned_dot.
static int format_cp_aligned_dot(Value v, int material, char *buf, size_t n) {
    const double pawns = fabs(0.01 * uci_wdl_to_cp((int) v, material));
    const char sign = v < 0 ? '-' : (v > 0 ? '+' : ' ');
    return snprintf(buf, n, "%c%6.2f", sign, pawns);
}

// Render the NNUE breakdown: the per-bucket material/positional split with the
// used bucket marked, then the three summary lines. Every figure is normalised
// through `uci_wdl_to_cp` before it is printed -- the table header says
// "(Normalized, ...)", and printing raw internal units under that header would
// misreport every cell by the win-rate `a` factor for the position's material.
// Port of upstream `nnue/nnue_misc.cpp:59` trace and `evaluate.cpp:75` Eval::trace.
static void trace_nnue(EvalArena *arena, const Position *pos, char *buf, int buf_len) {
    eval_acc_reset(arena);
    const NnueTraceOutput trace =
      network_trace_evaluate(pos, arena->acc_stack, arena->refresh_cache);
    const int material = board_wdl_material(pos);

    // Two leading newlines: upstream `engine.cpp:331` prints one before the trace
    // and `Eval::trace` opens with another.
    int n = snprintf(buf, (size_t) buf_len,
                     "\n\nNNUE network contributions (Normalized, %s to move)\n"
                     "+------------+------------+------------+------------+\n"
                     "|   Bucket   |  Material  | Positional |   Total    |\n"
                     "|            |   (PSQT)   |  (Layers)  |            |\n"
                     "+------------+------------+------------+------------+\n",
                     pos->side_to_move == WHITE ? "White" : "Black");

    for (size_t b = 0; b < NNUE_LAYER_STACKS && n > 0 && n < buf_len; ++b) {
        const Value psqt = (Value) trace.psqt[b];
        const Value positional = (Value) trace.positional[b];
        char mat_cell[16], pos_cell[16], tot_cell[16];
        format_cp_aligned_dot(psqt, material, mat_cell, sizeof mat_cell);
        format_cp_aligned_dot(positional, material, pos_cell, sizeof pos_cell);
        format_cp_aligned_dot((Value) (psqt + positional), material, tot_cell, sizeof tot_cell);
        n += snprintf(buf + n, (size_t) (buf_len - n),
                      "|  %zu         |  %s   |  %s   |  %s   |%s\n", b, mat_cell, pos_cell,
                      tot_cell, b == trace.correct_bucket ? " <-- this bucket is used" : "");
    }

    if (n <= 0 || n >= buf_len)
        return;

    eval_acc_reset(arena);
    const NnueEvalOutput out = network_evaluate(pos, arena->acc_stack, arena->refresh_cache);
    const Value internal = (Value) (out.psqt + out.positional);
    const Value white_internal = pos->side_to_move == WHITE ? internal : (Value) -internal;
    const Value scaled = nnue_scaled_value(pos, out.psqt, out.positional, 0);
    const Value white_scaled = pos->side_to_move == WHITE ? scaled : (Value) -scaled;

    // The trailing blank line is upstream's `sync_endl` after a string that already
    // ends in a newline; dropping it runs the next command's output into this one.
    snprintf(buf + n, (size_t) (buf_len - n),
             "+------------+------------+------------+------------+\n"
             "\nNNUE evaluation          %+d (side to move, internal units)\n"
             "NNUE evaluation        %+.2f (white side)\n"
             "Final evaluation      %+.2f (white side) [with scaled NNUE, ...]\n\n",
             internal, 0.01 * uci_wdl_to_cp((int) white_internal, material),
             0.01 * uci_wdl_to_cp((int) white_scaled, material));
}

static void trace_classical(const Position *pos, char *buf, int buf_len) {
    const Value white = evaluate_side(pos, WHITE);
    const Value black = evaluate_side(pos, BLACK);
    const Value final =
      pos->side_to_move == WHITE ? evaluate_classical(pos) : (Value) -evaluate_classical(pos);

    snprintf(buf, (size_t) buf_len,
             "\n Classical evaluation (mcfish placeholder, not upstream NNUE)\n"
             "\n     Term     |  White  |  Black  |  Total \n"
             " -------------+---------+---------+--------\n"
             "     Material |  %6.2f |  %6.2f | %6.2f\n"
             " -------------+---------+---------+--------\n"
             "\nFinal evaluation: %+.2f (white side)\n",
             white / 100.0, black / 100.0, (white - black) / 100.0, final / 100.0);
}

void evaluate_trace(const Position *pos, char *buf, int buf_len) {
    // In check there is no static evaluation to decompose: upstream refuses rather
    // than tracing a position the search would never evaluate statically.
    // Upstream `evaluate.cpp:77`.
    // One trailing newline, not two: upstream returns this string WITHOUT a newline
    // and `sync_endl` supplies the only one. The NNUE trace below ends in a newline
    // of its own, which is why that path prints a blank line here and this one does
    // not.
    if (board_has_checkers(pos)) {
        snprintf(buf, (size_t) buf_len, "\nFinal evaluation: none (in check)\n");
        return;
    }

    EvalArena *const arena = eval_default_arena();
    if (NetLoaded && arena != nullptr)
        trace_nnue(arena, pos, buf, buf_len);
    else
        trace_classical(pos, buf, buf_len);
}
