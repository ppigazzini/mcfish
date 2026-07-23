#include "probe.h"

#include "registry.h"
#include "wdl.h"

#include "../../engine/board/legality.h"
#include "../../engine/board/movegen.h"
#include "../../engine/board/types.h"

#include <string.h>

static const TbProbeResult Unavailable = {
    .available = 0, .wdl = 0, .wdl_state = 0, .dtz = 0, .dtz_state = 0
};

static int32_t sign_of(int32_t x) { return (x > 0 ? 1 : 0) - (x < 0 ? 1 : 0); }

// Port upstream `dtz_before_zeroing` (syzygy/tbprobe.cpp:177): recover the DTZ of
// the move before a zeroing one.
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

// Port upstream `probe_dtz` (syzygy/tbprobe.cpp:1601).
int32_t probe_dtz(Position *pos, int32_t *state) {
    *state = PROBE_OK;
    const TbProbeValue w = search_wdl(pos, true);
    if (w.state == PROBE_FAIL) {
        *state = PROBE_FAIL;
        return 0;
    }
    const int32_t wdl = w.value;
    if (wdl == WDL_DRAW) {
        return 0;  // DTZ tables store no draws
    }
    if (w.state == PROBE_ZEROING) {
        return dtz_before_zeroing(wdl);  // the best move is a winning zeroing move
    }

    int32_t table_state = PROBE_OK;
    const int32_t table_dtz = probe_table(pos, true, wdl, &table_state);
    if (table_state == PROBE_FAIL) {
        *state = PROBE_FAIL;
        return 0;
    }
    if (table_state != PROBE_CHANGE_STM) {
        const int32_t cursed = (wdl == WDL_BLESSED_LOSS || wdl == WDL_CURSED_WIN) ? 1 : 0;
        return (table_dtz + 100 * cursed) * sign_of(wdl);
    }

    // The DTZ is stored for the other side: search one ply and take the move that
    // minimises DTZ.
    int32_t min_dtz = 0xFFFF;
    ExtMove list[MAX_MOVES];
    StateInfo st;
    const size_t total = (size_t) (generate_legal(pos, list) - list);

    for (size_t i = 0; i < total; ++i) {
        const Move m = list[i].move;
        const bool zeroing =
          is_capture(pos, m) || type_of_piece(piece_on(pos, move_from(m))) == PAWN;

        // pos_do_move trusts this argument for the child's checkers set: see the
        // note at the matching call in wdl.c.
        pos_do_move(pos, m, &st, pos_gives_check(pos, m), &pos->scratch_dp, &pos->scratch_dts);

        int32_t child_state = PROBE_OK;
        int32_t dtz;
        if (zeroing) {
            // Take the DTZ of the move BEFORE playing it — otherwise this is the
            // DTZ of the next move sequence — but search the position after it to
            // get the sign.
            const TbProbeValue s = search_wdl(pos, false);
            child_state = s.state;
            dtz = -dtz_before_zeroing(s.value);
        } else {
            dtz = -probe_dtz(pos, &child_state);
        }

        // Force DTZ 1 for a mating move: the child is in check with no legal reply.
        ExtMove mate_list[MAX_MOVES];
        if (dtz == 1 && checkers(pos) != 0 && generate_legal(pos, mate_list) == mate_list) {
            min_dtz = 1;
        }

        // Correct for the one-ply search. Zeroing moves are already accounted for
        // by dtz_before_zeroing.
        if (!zeroing) {
            dtz += sign_of(dtz);
        }
        // Skip the draws, and when winning take only a positive DTZ.
        if (dtz < min_dtz && sign_of(dtz) == sign_of(wdl)) {
            min_dtz = dtz;
        }

        pos_undo_move(pos, m);

        if (child_state == PROBE_FAIL) {
            *state = PROBE_FAIL;
            return 0;
        }
    }

    return min_dtz == 0xFFFF ? -1 : min_dtz;  // no legal move means mate
}

TbProbeResult syzygy_probe_fen(const char *fen, size_t len, bool chess960) {
    if (!registry_ready() || fen == nullptr || len == 0 || len >= 256) {
        return Unavailable;
    }

    char buf[256];
    memcpy(buf, fen, len);
    buf[len] = '\0';

    Position pos;
    StateInfo si;
    if (!pos_set(&pos, buf, chess960, &si)) {
        return Unavailable;
    }

    const TbProbeValue w = search_wdl(&pos, false);  // probe_wdl
    if (w.state == PROBE_FAIL) {
        return Unavailable;
    }

    int32_t dtz_state = PROBE_OK;
    const int32_t dtz = probe_dtz(&pos, &dtz_state);

    return (TbProbeResult) {
        .available = 1, .wdl = w.value, .wdl_state = w.state, .dtz = dtz, .dtz_state = dtz_state
    };
}

TbProbeResult syzygy_probe_wdl_pos(Position *pos) {
    if (!registry_ready()) {
        return Unavailable;
    }
    const TbProbeValue w = search_wdl(pos, false);
    if (w.state == PROBE_FAIL) {
        return Unavailable;
    }
    return (TbProbeResult) {
        .available = 1, .wdl = w.value, .wdl_state = w.state, .dtz = 0, .dtz_state = 0
    };
}
