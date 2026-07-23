#include "root_move_build.h"

#include "option_source.h"
#include "tb_source.h"

#include "../board/board_props.h"
#include "../board/legality.h"
#include "../board/movegen.h"
#include "../board/repetition.h"
#include "../board/score.h"

#include <stdlib.h>
#include <string.h>

enum : int32_t {
    MAX_DTZ = 1 << 18,
    PROBE_FAIL = 0,
    WDL_LOSS = -2,
    WDL_BLESSED_LOSS = -1,
    WDL_DRAW = 0,
    WDL_CURSED_WIN = 1,
    WDL_WIN = 2,
};

static const int32_t WdlToRank[5] = { -MAX_DTZ, -MAX_DTZ + 101, 0, MAX_DTZ - 101, MAX_DTZ };

static const int32_t WdlToValue[5] = { -VALUE_MATE + MAX_PLY + 1, VALUE_DRAW - 2, VALUE_DRAW,
                                       VALUE_DRAW + 2, VALUE_MATE - MAX_PLY - 1 };

// Hold a throwaway board the root ranking replays its moves on, anchored at the
// root FEN, so a probe never mutates the position the search owns.
typedef struct {
    Position pos;
    StateInfo root_st;
    StateInfo move_st;
} ScratchPosition;

static bool aborted(TbTimeAbort time_abort, void *abort_ctx) {
    return time_abort != nullptr && time_abort(abort_ctx);
}

static int count_pieces(const Position *pos) {
    Piece board[SQUARE_NB];
    board_copy_pieces(pos, board);
    int n = 0;
    for (size_t i = 0; i < SQUARE_NB; ++i)
        if (board[i] != NO_PIECE)
            ++n;
    return n;
}

static TbConfig load_tb_config(void) {
    // Read the Syzygy options off the global option seam, not a handle, so no
    // options parameter is threaded through the ranking.
    TbConfig config = {
        .cardinality = OptionSyzygyProbeLimit(),
        .root_in_tb = false,
        .use_rule50 = OptionSyzygy50MoveRule(),
        .probe_depth = OptionSyzygyProbeDepth(),
    };

    const int32_t max_cardinality = (int32_t) TbMaxCardinality();
    if (config.cardinality > max_cardinality) {
        config.cardinality = max_cardinality;
        config.probe_depth = 0;
    }
    // Upstream does NOT zero cardinality for positions larger than the TB: it
    // keeps it so the in-search Step 6 probe still fires at smaller in-tree
    // positions. Whether the ROOT is ranked is a separate gate below.
    return config;
}

static TbProbeResult probe_position(const Position *pos) {
    char fen[128];
    pos_fen(pos, fen);
    return TbProbeFen(fen, strlen(fen), board_is_chess960(pos));
}

static int32_t dtz_before_zeroing(int32_t wdl) {
    switch (wdl) {
    case WDL_WIN :
        return 1;
    case WDL_CURSED_WIN :
        return 101;
    case WDL_BLESSED_LOSS :
        return -101;
    case WDL_LOSS :
        return -1;
    default :
        return 0;
    }
}

// Mirror upstream Position::dtz_is_dtm: pawnless and (3-men or 4-men minors
// only). When true the DTZ tables rank exactly like DTM, so the root ranking uses
// the exact DTZ; otherwise every winning move ties at MAX_DTZ and the tie-break
// is generator order.
static bool dtz_is_dtm(const Position *pos) {
    Piece board[SQUARE_NB];
    board_copy_pieces(pos, board);
    int total = 0, pawns = 0, queens_rooks = 0;
    for (size_t i = 0; i < SQUARE_NB; ++i) {
        const Piece p = board[i];
        if (p == NO_PIECE)
            continue;
        ++total;
        const PieceType t = type_of_piece(p);
        if (t == PAWN)
            ++pawns;
        if (t == ROOK || t == QUEEN)
            ++queens_rooks;
    }
    return pawns == 0 && (total == 3 || (total == 4 && queens_rooks == 0));
}

typedef enum { DTZ_SUCCESS, DTZ_FALLBACK_TO_WDL } DtzRankResult;

static DtzRankResult rank_root_moves_dtz(Position *pos,
                                         StateInfo *st,
                                         bool rule50,
                                         bool rank_dtz,
                                         int32_t root_rule50,
                                         bool root_has_repeated,
                                         RankedRootMove *ranked,
                                         size_t count,
                                         TbTimeAbort time_abort,
                                         void *abort_ctx) {
    const int32_t bound = rule50 ? MAX_DTZ / 2 - 100 : 1;

    for (size_t i = 0; i < count; ++i) {
        RankedRootMove *const rm = &ranked[i];
        pos_do_move(pos, rm->raw_move, st, pos_gives_check(pos, rm->raw_move), &pos->scratch_dp,
                    &pos->scratch_dts);

        int32_t dtz = 0;
        bool probe_failed = false;
        if (pos->st->rule50 == 0) {
            const TbProbeResult probe = probe_position(pos);
            probe_failed = probe.wdl_state == PROBE_FAIL;
            dtz = dtz_before_zeroing(-probe.wdl);
        } else if ((rule50 && pos_is_draw(pos, 1)) || pos_is_repetition(pos, 1)) {
            dtz = 0;
        } else {
            const TbProbeResult probe = probe_position(pos);
            probe_failed = probe.dtz_state == PROBE_FAIL;
            dtz = -probe.dtz;
            dtz = dtz > 0 ? dtz + 1 : (dtz < 0 ? dtz - 1 : 0);
        }

        if (checkers(pos) != 0 && dtz == 2) {
            ExtMove legal_moves[MAX_MOVES];
            if (generate_legal(pos, legal_moves) == legal_moves)
                dtz = 1;
        }

        // Undo before the bail-out test, as upstream does (tbprobe.cpp:1713): the
        // caller keeps walking this position, so an early return must still leave
        // it exactly as it was handed over.
        pos_undo_move(pos, rm->raw_move);

        if (probe_failed || aborted(time_abort, abort_ctx))
            return DTZ_FALLBACK_TO_WDL;

        const int32_t rank_term = rank_dtz ? dtz : 0;
        int32_t rank;
        if (dtz > 0)
            rank = (dtz + root_rule50 <= 99 && !root_has_repeated)
                   ? MAX_DTZ - rank_term
                   : MAX_DTZ / 2 - (dtz + root_rule50);
        else if (dtz < 0)
            rank = (-dtz * 2 + root_rule50 < 100) ? -MAX_DTZ - rank_term
                                                  : -(MAX_DTZ / 2) + (-dtz + root_rule50);
        else
            rank = 0;

        rm->tb_rank = rank;
        if (rank >= bound)
            rm->tb_score = VALUE_MATE - MAX_PLY - 1;
        else if (rank > 0)
            rm->tb_score =
              (rank - (MAX_DTZ / 2 - 200) > 3 ? rank - (MAX_DTZ / 2 - 200) : 3) * PAWN_VALUE / 200;
        else if (rank == 0)
            rm->tb_score = VALUE_DRAW;
        else if (rank > -bound)
            rm->tb_score = (rank + (MAX_DTZ / 2 - 200) < -3 ? rank + (MAX_DTZ / 2 - 200) : -3)
                         * PAWN_VALUE / 200;
        else
            rm->tb_score = -VALUE_MATE + MAX_PLY + 1;
    }

    return DTZ_SUCCESS;
}

static bool rank_root_moves_wdl(
  Position *pos, StateInfo *st, bool rule50, RankedRootMove *ranked, size_t count) {
    for (size_t i = 0; i < count; ++i) {
        RankedRootMove *const rm = &ranked[i];
        pos_do_move(pos, rm->raw_move, st, pos_gives_check(pos, rm->raw_move), &pos->scratch_dp,
                    &pos->scratch_dts);

        int32_t wdl = WDL_DRAW;
        bool probe_failed = false;
        if (!pos_is_draw(pos, 1)) {
            const TbProbeResult probe = probe_position(pos);
            probe_failed = probe.wdl_state == PROBE_FAIL;
            wdl = -probe.wdl;
        }

        pos_undo_move(pos, rm->raw_move);
        if (probe_failed)
            return false;

        rm->tb_rank = WdlToRank[wdl + 2];

        int32_t score_wdl = wdl;
        if (!rule50)
            score_wdl = wdl > 0 ? WDL_WIN : (wdl < 0 ? WDL_LOSS : WDL_DRAW);
        rm->tb_score = WdlToValue[score_wdl + 2];
    }

    return true;
}

void tb_stable_sort_by_rank(RankedRootMove *ranked, size_t count) {
    for (size_t index = 1; index < count; ++index) {
        const RankedRootMove current = ranked[index];
        size_t insert_at = index;
        while (insert_at > 0 && ranked[insert_at - 1].tb_rank < current.tb_rank) {
            ranked[insert_at] = ranked[insert_at - 1];
            --insert_at;
        }
        ranked[insert_at] = current;
    }
}

TbConfig tb_rank_moves(Position *pos,
                       StateInfo *st,
                       RankedRootMove *ranked,
                       size_t count,
                       bool rank_dtz,
                       int32_t cnt50,
                       bool has_repeated,
                       TbTimeAbort time_abort,
                       void *abort_ctx) {
    TbConfig config = load_tb_config();
    if (count == 0)
        return config;

    bool dtz_available = true;

    // Rank only when the position itself fits the tablebase and cannot castle.
    // Otherwise it is searched normally, with cardinality kept so the in-search
    // Step 6 probe still fires at smaller in-tree positions.
    if (config.cardinality >= count_pieces(pos) && pos->st->castling_rights == 0) {
        // Where DTZ ranks as DTM, the exact DTZ is the only ordering that puts the
        // fastest mate first; without this every certain win ties at MAX_DTZ and
        // the tie-break falls to generator order.
        rank_dtz = rank_dtz || dtz_is_dtm(pos);

        const DtzRankResult dtz_result =
          rank_root_moves_dtz(pos, st, config.use_rule50, rank_dtz, cnt50, has_repeated, ranked,
                              count, time_abort, abort_ctx);

        if (dtz_result == DTZ_SUCCESS) {
            config.root_in_tb = true;
        } else if (!aborted(time_abort, abort_ctx)) {
            dtz_available = false;
            config.root_in_tb = rank_root_moves_wdl(pos, st, config.use_rule50, ranked, count);
        }
    }

    if (config.root_in_tb) {
        tb_stable_sort_by_rank(ranked, count);
        // Probe during search only when DTZ is unavailable and we are not winning.
        if (dtz_available || ranked[0].tb_score <= VALUE_DRAW)
            config.cardinality = 0;
    } else {
        // Zero every rank when both probes failed (upstream tbprobe.cpp:1836).
        // A partially-ranked list left behind by an aborted DTZ pass would
        // otherwise order the moves by half a probe.
        for (size_t i = 0; i < count; ++i)
            ranked[i].tb_rank = 0;
    }

    return config;
}

static RootMove *root_moves_create_ranked(const RankedRootMove *items, size_t count) {
    if (count == 0)
        return nullptr;
    RootMove *const elems = calloc(count, sizeof *elems);
    if (!elems)
        return nullptr;
    for (size_t i = 0; i < count; ++i) {
        RootMove *const rm = &elems[i];
        rm->score = -VALUE_INFINITE;
        rm->previous_score = -VALUE_INFINITE;
        rm->average_score = -VALUE_INFINITE;
        rm->mean_squared_score = -(VALUE_INFINITE * VALUE_INFINITE);
        rm->uci_score = -VALUE_INFINITE;
        rm->tb_rank = items[i].tb_rank;
        rm->tb_score = items[i].tb_score;
        rm->pv.moves[0] = items[i].raw_move;
        rm->pv.length = 1;
    }
    return elems;
}

void root_moves_free(RootMoveList *list) {
    free(list->moves);
    list->moves = nullptr;
    list->count = 0;
}

bool root_moves_build(const Position *pos,
                      const char *root_fen,
                      bool chess960,
                      const Move *moves,
                      size_t count,
                      RootMoveList *out) {
    RankedRootMove *const ranked = count != 0 ? calloc(count, sizeof *ranked) : nullptr;
    if (count != 0 && !ranked)
        return false;

    for (size_t i = 0; i < count; ++i)
        ranked[i].raw_move = moves[i];

    TbConfig tb_config = load_tb_config();

    // Rank on a scratch board built from the root FEN, never on POS: the caller
    // owns POS and the ranking walks its argument with do/undo.
    // A zero cardinality is what a build with no SyzygyPath resolves to, and it
    // fails the gate inside tb_rank_moves for every position. Short-circuit it
    // here so that build never allocates the scratch board or re-parses the FEN.
    if (count != 0 && tb_config.cardinality != 0) {
        ScratchPosition *const sp = calloc(1, sizeof *sp);
        if (!sp) {
            free(ranked);
            return false;
        }
        if (pos_set(&sp->pos, root_fen, chess960, &sp->root_st))
            tb_config = tb_rank_moves(&sp->pos, &sp->move_st, ranked, count, false, pos->st->rule50,
                                      pos_has_repeated(pos), nullptr, nullptr);
        free(sp);
    }

    RootMove *const root_moves = root_moves_create_ranked(ranked, count);
    free(ranked);
    if (count != 0 && !root_moves)
        return false;

    out->moves = root_moves;
    out->count = count;
    out->tb_config = tb_config;
    return true;
}
