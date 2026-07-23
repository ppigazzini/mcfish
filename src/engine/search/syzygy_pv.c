#include "syzygy_pv.h"

#include "option_source.h"
#include "output_sink.h"
#include "root_move_build.h"
#include "time_source.h"

#include "../board/legality.h"
#include "../board/movegen.h"
#include "../board/repetition.h"
#include "../board/score.h"

#include <stdlib.h>
#include <string.h>

// Bound the walk by the PV buffer, not by the tablebase: `PVMoves` holds
// MAX_PLY + 1 moves and one StateInfo backs each made move, plus one slot the
// ranking borrows as its trial ply.
enum { PV_MOVES_MAX = MAX_PLY, STATE_SLOTS = MAX_PLY + 2 };

typedef struct {
    TimePoint start;
    int32_t move_overhead;
    bool use_time_management;
} Deadline;

// Spend at most half of Move Overhead, and only when a clock is being managed.
// Upstream measures in fractional milliseconds (search.cpp:2109); the seam here
// is whole-millisecond, so the abort lands on the same side of the bound only up
// to one millisecond of granularity.
static bool deadline_expired(void *ctx) {
    const Deadline *const d = ctx;
    return d->use_time_management && 2 * (TimeNowMs() - d->start) > d->move_overhead;
}

static size_t collect_legal(const Position *pos, RankedRootMove *out) {
    ExtMove list[MAX_MOVES];
    const ExtMove *const end = generate_legal(pos, list);
    size_t n = 0;
    for (const ExtMove *m = list; m != end; ++m) {
        out[n].raw_move = m->move;
        out[n].tb_rank = 0;
        out[n].tb_score = 0;
        ++n;
    }
    return n;
}

static const RankedRootMove *find_ranked(const RankedRootMove *ranked, size_t n, Move m) {
    for (size_t i = 0; i < n; ++i)
        if (ranked[i].raw_move == m)
            return &ranked[i];
    return nullptr;
}

// Score each move by how far it restricts the opponent, and never by handing them
// a capture. This only breaks DTZ ties: the rank is overwritten wholesale by the
// DTZ ranking that follows, which sorts stably and so keeps this order among
// equal-DTZ moves. Upstream search.cpp:2172.
static void
rank_by_opponent_mobility(Position *pos, StateInfo *scratch, RankedRootMove *ranked, size_t n) {
    for (size_t i = 0; i < n; ++i) {
        pos_do_move(pos, ranked[i].raw_move, scratch, pos_gives_check(pos, ranked[i].raw_move),
                    &pos->scratch_dp, &pos->scratch_dts);

        ExtMove replies[MAX_MOVES];
        const ExtMove *const end = generate_legal(pos, replies);
        for (const ExtMove *r = replies; r != end; ++r)
            ranked[i].tb_rank -= is_capture(pos, r->move) ? 100 : 1;

        pos_undo_move(pos, ranked[i].raw_move);
    }
}

void syzygy_extend_pv(Position *pos, bool use_time_management, RootMove *rm, int32_t *v) {
    if (rm->pv.length == 0)
        return;

    const bool rule50 = OptionSyzygy50MoveRule();
    Deadline deadline = {
        .start = TimeNowMs(),
        .move_overhead = OptionIntByName("Move Overhead"),
        .use_time_management = use_time_management,
    };

    StateInfo *const sts = calloc(STATE_SLOTS, sizeof *sts);
    RankedRootMove *const ranked = calloc(MAX_MOVES, sizeof *ranked);
    if (!sts || !ranked) {
        free(sts);
        free(ranked);
        return;
    }

    // Step 0: play the root move itself. It is never re-ranked — under MultiPV in
    // a tablebase it is deliberately not the top-ranked move.
    size_t made = 0;
    pos_do_move(pos, rm->pv.moves[made], &sts[made], pos_gives_check(pos, rm->pv.moves[made]),
                &pos->scratch_dp, &pos->scratch_dts);
    ++made;
    size_t ply = 1;

    // Step 1: keep the search PV for as long as each of its moves is still one of
    // the top-ranked moves. The first move that is not is where the tablebase stops
    // vouching for the line, and everything from there on is dropped.
    while (ply < rm->pv.length && made < STATE_SLOTS - 1) {
        const Move pv_move = rm->pv.moves[ply];

        const size_t n = collect_legal(pos, ranked);
        const TbConfig config = tb_rank_moves(pos, &sts[made], ranked, n, false, pos->st->rule50,
                                              pos_has_repeated(pos), deadline_expired, &deadline);

        const RankedRootMove *const entry = find_ranked(ranked, n, pv_move);
        if (n == 0 || !entry || ranked[0].tb_rank != entry->tb_rank)
            break;

        ++ply;
        pos_do_move(pos, pv_move, &sts[made], pos_gives_check(pos, pv_move), &pos->scratch_dp,
                    &pos->scratch_dts);
        ++made;

        // Never show a repetition or a fifty-move draw inside a won tablebase line:
        // the score claims a win and the line would show it being thrown away.
        if (config.root_in_tb
            && ((rule50 && pos_is_draw(pos, (int) ply)) || pos_is_repetition(pos, (int) ply))) {
            pos_undo_move(pos, pv_move);
            --made;
            --ply;
            break;
        }

        // Show only a fully validated PV: one that could not be validated in time
        // is not shown as far as it got.
        if (config.root_in_tb && deadline_expired(&deadline))
            break;
    }

    rm->pv.length = ply;

    // Step 2: walk on with minimum-DTZ moves, the line a user clicking top-ranked
    // moves on syzygy-tables.info would follow. It is optimal only for endgames
    // simple enough that DTZ ranks as DTM, and it is a plausible continuation
    // rather than a proven mate.
    while (!(rule50 && pos_is_draw(pos, 0))) {
        if (deadline_expired(&deadline))
            break;
        if (rm->pv.length >= PV_MOVES_MAX || made >= STATE_SLOTS - 1)
            break;

        const size_t n = collect_legal(pos, ranked);
        if (n == 0)  // mate
            break;

        rank_by_opponent_mobility(pos, &sts[made], ranked, n);
        tb_stable_sort_by_rank(ranked, n);

        // The winning side minimises DTZ, the losing side maximises it.
        const TbConfig config = tb_rank_moves(pos, &sts[made], ranked, n, true, pos->st->rule50,
                                              pos_has_repeated(pos), deadline_expired, &deadline);

        // Without DTZ there is no mate to walk toward, so stop rather than guess.
        if (!config.root_in_tb || config.cardinality > 0)
            break;

        const Move pv_move = ranked[0].raw_move;
        rm->pv.moves[rm->pv.length++] = pv_move;
        pos_do_move(pos, pv_move, &sts[made], pos_gives_check(pos, pv_move), &pos->scratch_dp,
                    &pos->scratch_dts);
        ++made;
    }

    // Reaching a draw here is exceptional: it needs a position whose fifty-move
    // counter was already non-optimal when it was reached on the board, which DTZ
    // rounding cannot always rank correctly. Report the line that was actually
    // found rather than the score that was believed.
    if (pos_is_draw(pos, 0))
        *v = VALUE_DRAW;

    for (size_t i = made; i > 0; --i)
        pos_undo_move(pos, rm->pv.moves[i - 1]);

    free(sts);
    free(ranked);

    if (deadline_expired(&deadline)) {
        const char *const msg = "info string Syzygy based PV extension requires more time, "
                                "increase Move Overhead as needed.";
        OutputPrintLine(msg, strlen(msg));
    }
}
