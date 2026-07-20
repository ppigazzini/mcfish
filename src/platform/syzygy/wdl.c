#include "wdl.h"

#include "decode.h"
#include "encode.h"
#include "registry.h"
#include "tables.h"

#include "../../engine/board/bitboard.h"
#include "../../engine/board/movegen.h"

#include <string.h>

uint64_t syzygy_position_key(const Position *pos) { return syzygy_material_key(pos->piece_count); }

static inline unsigned sq_file(uint8_t sq) { return sq & 7u; }
static inline unsigned sq_rank(uint8_t sq) { return sq >> 3; }
static inline int32_t map_pawns_of(uint8_t sq) { return MapPawns[sq]; }

// Sort by insertion — stable, as upstream's std::stable_sort is here.
static void stable_sort_by_map_pawns(uint8_t *sq, size_t n) {
    for (size_t i = 1; i < n; ++i) {
        const uint8_t v = sq[i];
        size_t j = i;
        while (j > 0 && map_pawns_of(sq[j - 1]) > map_pawns_of(v)) {
            sq[j] = sq[j - 1];
            --j;
        }
        sq[j] = v;
    }
}

static void stable_sort_squares(uint8_t *sq, size_t n) {
    for (size_t i = 1; i < n; ++i) {
        const uint8_t v = sq[i];
        size_t j = i;
        while (j > 0 && sq[j - 1] > v) {
            sq[j] = sq[j - 1];
            --j;
        }
        sq[j] = v;
    }
}

// Port upstream `map_score<DTZ>` (syzygy/tbprobe.cpp:732): remap the raw value
// through this WDL class's map, then convert it to plies — doubled unless the
// flags say the class already stores plies — and add one.
static int32_t
map_score_dtz(const TBTable *t, const PairsData *d, int32_t value, int32_t wdl, bool *ok) {
    static const size_t WdlMap[5] = { 1, 3, 0, 2, 0 };  // index by wdl + 2

    const uint8_t flags = d->flags;
    if (flags & TB_FLAG_MAPPED) {
        if (wdl < -2 || wdl > 2 || value < 0 || t->dtz_map == nullptr) {
            *ok = false;
            return 0;
        }
        const size_t mi = d->map_idx[WdlMap[wdl + 2]];
        const size_t off = mi + (size_t) value;
        const size_t width = (flags & TB_FLAG_WIDE) ? 2 : 1;
        if (off > t->dtz_map_size / width || (off + 1) * width > t->dtz_map_size) {
            *ok = false;
            return 0;
        }
        if (flags & TB_FLAG_WIDE) {
            value = (int32_t) rd_u16le(t->dtz_map + off * 2);
        } else {
            value = (int32_t) t->dtz_map[off];
        }
    }
    if ((wdl == WDL_WIN && (flags & TB_FLAG_WIN_PLIES) == 0)
        || (wdl == WDL_LOSS && (flags & TB_FLAG_LOSS_PLIES) == 0) || wdl == WDL_CURSED_WIN
        || wdl == WDL_BLESSED_LOSS) {
        value *= 2;
    }
    return value + 1;
}

// Port upstream `do_probe_table` (syzygy/tbprobe.cpp:772).
int32_t
do_probe_table(const Position *pos, TBTable *t, bool dtz, int32_t wdl_score, int32_t *state) {
    uint8_t squares[TB_PIECES];
    uint8_t pieces_arr[TB_PIECES];
    size_t size = 0;
    size_t lead_pawns_cnt = 0;
    size_t tb_file = 0;

    if (popcount_bb(pieces(pos)) > TB_PIECES || (size_t) t->piece_count > TB_PIECES) {
        *state = PROBE_FAIL;
        return 0;
    }

    const uint64_t material_key = syzygy_position_key(pos);
    const size_t stm_pos = pos->side_to_move;

    // Swap when the table stores the mirrored configuration: either black is the
    // stronger side, or the material is symmetric and black is to move.
    const bool symmetric_btm = (t->key == t->key2) && stm_pos != 0;
    const bool black_stronger = material_key != t->key;
    const bool swap = symmetric_btm || black_stronger;
    const uint8_t flip_color = swap ? 8u : 0u;
    const uint8_t flip_squares = swap ? 56u : 0u;
    const size_t stm = (size_t) (swap ? 1 : 0) ^ stm_pos;

    Bitboard lead_pawns = 0;
    if (t->has_pawns) {
        const uint8_t pc = (uint8_t) (tbtable_get(t, dtz, 0, 0)->pieces[0] ^ flip_color);
        const Color lead_color = (Color) (pc >> 3);
        lead_pawns = pos->by_color[lead_color] & pos->by_type[PAWN];
        Bitboard b = lead_pawns;
        while (b) {
            const Square s = pop_lsb(&b);
            squares[size++] = (uint8_t) ((uint8_t) s ^ flip_squares);
        }
        lead_pawns_cnt = size;
        if (lead_pawns_cnt == 0) {
            *state = PROBE_FAIL;
            return 0;
        }

        // Move the pawn with the largest MapPawns[] — the first such — to squares[0].
        size_t maxi = 0;
        for (size_t j = 1; j < lead_pawns_cnt; ++j) {
            if (map_pawns_of(squares[j]) > map_pawns_of(squares[maxi])) {
                maxi = j;
            }
        }
        const uint8_t tmp = squares[0];
        squares[0] = squares[maxi];
        squares[maxi] = tmp;

        tb_file = edge_distance(sq_file(squares[0]));
    }

    // Treat a DTZ table as one-sided: when the stored side is not the side to
    // move, bail out to the caller's one-ply search.
    if (dtz) {
        const uint8_t flags = tbtable_get(t, true, stm, tb_file)->flags;
        const bool stm_ok =
          ((size_t) (flags & TB_FLAG_STM) == stm) || (t->key == t->key2 && !t->has_pawns);
        if (!stm_ok) {
            *state = PROBE_CHANGE_STM;
            return 0;
        }
    }

    // Gather every remaining piece — everything but the leading pawns.
    Bitboard b = pos->by_type[ALL_PIECES] ^ lead_pawns;
    while (b) {
        const Square s = pop_lsb(&b);
        squares[size] = (uint8_t) ((uint8_t) s ^ flip_squares);
        pieces_arr[size] = (uint8_t) ((uint8_t) pos->board[s] ^ flip_color);
        ++size;
    }

    PairsData *const d = tbtable_get(t, dtz, stm, tb_file);

    // Reorder the pieces to match the file's canonical sequence.
    for (size_t i = lead_pawns_cnt; i + 1 < size; ++i) {
        for (size_t j = i + 1; j < size; ++j) {
            if (d->pieces[i] == pieces_arr[j]) {
                const uint8_t ps = pieces_arr[i];
                pieces_arr[i] = pieces_arr[j];
                pieces_arr[j] = ps;
                const uint8_t sq = squares[i];
                squares[i] = squares[j];
                squares[j] = sq;
                break;
            }
        }
    }

    // Map the leading square into the a1-d1-d4 triangle (file <= D).
    if (sq_file(squares[0]) > 3) {
        for (size_t i = 0; i < size; ++i) {
            squares[i] ^= 7u;
        }
    }

    uint64_t idx = 0;
    if (t->has_pawns) {
        idx = (uint64_t) lead_pawn_idx_at(lead_pawns_cnt, squares[0]);
        stable_sort_by_map_pawns(squares + 1, lead_pawns_cnt - 1);
        for (size_t i = 1; i < lead_pawns_cnt; ++i) {
            idx += (uint64_t) binomial_at((int32_t) i, map_pawns_of(squares[i]));
        }
    } else {
        // Flip so the leading piece sits below RANK_5.
        if (sq_rank(squares[0]) > 3) {
            for (size_t i = 0; i < size; ++i) {
                squares[i] ^= 56u;
            }
        }
        // Take the first leading-group piece off the a1-h8 diagonal and reflect
        // the whole set below it.
        for (size_t i = 0; i < (size_t) d->group_len[0] && i < size; ++i) {
            if (off_a1h8(squares[i]) == 0) {
                continue;
            }
            if (off_a1h8(squares[i]) > 0) {
                for (size_t j = i; j < size; ++j) {
                    const unsigned sq = squares[j];
                    squares[j] = (uint8_t) (((sq >> 3) | (sq << 3)) & 63u);
                }
            }
            break;
        }

        if (t->has_unique_pieces) {
            if (size < 3) {
                *state = PROBE_FAIL;
                return 0;
            }
            const int64_t adjust1 = squares[1] > squares[0] ? 1 : 0;
            const int64_t adjust2 =
              (squares[2] > squares[0] ? 1 : 0) + (squares[2] > squares[1] ? 1 : 0);
            const int64_t s1 = squares[1];
            const int64_t s2 = squares[2];
            if (off_a1h8(squares[0]) != 0) {
                idx = (uint64_t) ((((int64_t) MapA1D1D4[squares[0]] * 63 + (s1 - adjust1)) * 62)
                                  + s2 - adjust2);
            } else if (off_a1h8(squares[1]) != 0) {
                idx = (uint64_t) ((6 * 63 + (int64_t) sq_rank(squares[0]) * 28
                                   + (int64_t) MapB1H1H7[squares[1]])
                                    * 62
                                  + s2 - adjust2);
            } else if (off_a1h8(squares[2]) != 0) {
                idx = (uint64_t) (6 * 63 * 62 + 4 * 28 * 62 + (int64_t) sq_rank(squares[0]) * 7 * 28
                                  + ((int64_t) sq_rank(squares[1]) - adjust1) * 28
                                  + (int64_t) MapB1H1H7[squares[2]]);
            } else {
                idx = (uint64_t) (6 * 63 * 62 + 4 * 28 * 62 + 4 * 7 * 28
                                  + (int64_t) sq_rank(squares[0]) * 7 * 6
                                  + ((int64_t) sq_rank(squares[1]) - adjust1) * 6
                                  + ((int64_t) sq_rank(squares[2]) - adjust2));
            }
        } else {
            if (size < 2) {
                *state = PROBE_FAIL;
                return 0;
            }
            const int32_t kk = MapA1D1D4[squares[0]];
            if (kk < 0 || kk >= 10) {
                *state = PROBE_FAIL;
                return 0;
            }
            idx = (uint64_t) MapKK[kk][squares[1]];
        }
    }

    idx *= d->group_idx[0];

    // Encode the remaining groups.
    size_t group_off = (size_t) d->group_len[0];
    bool remaining_pawns = t->has_pawns && t->pawn_count[1] != 0;
    size_t next = 0;
    while (next < TB_PIECES) {
        ++next;
        const size_t glen = (size_t) d->group_len[next];
        if (glen == 0) {
            break;
        }
        if (group_off + glen > size) {
            *state = PROBE_FAIL;
            return 0;
        }
        stable_sort_squares(squares + group_off, glen);
        uint64_t n = 0;
        for (size_t gi = 0; gi < glen; ++gi) {
            int64_t adjust = 0;
            for (size_t si = 0; si < group_off; ++si) {
                adjust += squares[group_off + gi] > squares[si] ? 1 : 0;
            }
            const int64_t col =
              (int64_t) squares[group_off + gi] - adjust - (remaining_pawns ? 8 : 0);
            n += (uint64_t) binomial_at((int32_t) (gi + 1), (int32_t) col);
        }
        remaining_pawns = false;
        idx += n * d->group_idx[next];
        group_off += glen;
    }

    bool ok = true;
    const int32_t raw = decode_pairs(d, idx, &ok);
    if (!ok) {
        *state = PROBE_FAIL;
        return 0;
    }
    if (dtz) {
        const int32_t mapped = map_score_dtz(t, d, raw, wdl_score, &ok);
        if (!ok) {
            *state = PROBE_FAIL;
            return 0;
        }
        return mapped;
    }
    return raw - 2;  // map_score<WDL>
}

// Port upstream `probe_table` (syzygy/tbprobe.cpp:1305).
int32_t probe_table(const Position *pos, bool dtz, int32_t wdl_score, int32_t *state) {
    if (popcount_bb(pieces(pos)) == 2) {
        return 0;  // KvK is a draw and has no table
    }
    TBTable *t = registry_get(syzygy_position_key(pos));
    if (t == nullptr) {
        *state = PROBE_FAIL;
        return 0;
    }
    if (!(dtz ? registry_map_dtz(t) : registry_map_wdl(t))) {
        *state = PROBE_FAIL;
        return 0;
    }
    return do_probe_table(pos, t, dtz, wdl_score, state);
}

// Port upstream `search<CheckZeroingMoves>` (syzygy/tbprobe.cpp:1332). A capture —
// and, when CHECK_ZEROING, a pawn move — zeroes the fifty-move counter, so its
// result must be probed and compared against the position's own stored value.
// Children recurse with check_zeroing false.
TbProbeValue search_wdl(Position *pos, bool check_zeroing) {
    int32_t best = WDL_LOSS;
    size_t move_count = 0;
    ExtMove list[MAX_MOVES];
    StateInfo st;

    const size_t total = (size_t) (generate_legal(pos, list) - list);

    for (size_t i = 0; i < total; ++i) {
        const Move m = list[i].move;
        if (!is_capture(pos, m)
            && (!check_zeroing || type_of_piece(piece_on(pos, move_from(m))) != PAWN)) {
            continue;
        }
        ++move_count;
        // The `false` is a placeholder, not a claim that M gives no check.
        // pos_do_move ignores the parameter today — set_check_info recomputes the
        // checkers from the board (position.c:148, called from do_move) — so every caller
        // passes something inert here. Upstream's `search<CheckZeroingMoves>` uses
        // the two-argument do_move, which computes `pos.gives_check(m)` itself. The
        // day pos_do_move starts trusting this argument, this call and the one in
        // probe.c must pass `search_gives_check(pos, m)` or the prober will read a
        // wrong checkers set and mis-probe.
        pos_do_move(pos, m, &st, false, &pos->scratch_dp, &pos->scratch_dts);
        const TbProbeValue child = search_wdl(pos, false);
        pos_undo_move(pos, m);
        if (child.state == PROBE_FAIL) {
            return (TbProbeValue) { .value = 0, .state = PROBE_FAIL };
        }
        const int32_t v = -child.value;
        if (v > best) {
            best = v;
            if (v >= WDL_WIN) {  // a winning zeroing move
                return (TbProbeValue) { .value = v, .state = PROBE_ZEROING };
            }
        }
    }

    // Use the best value rather than the stored one when every legal move is a
    // zeroing move and all were searched: the stored value can be wrong there —
    // tables hold no en-passant rights, for one.
    const bool no_more_moves = move_count != 0 && move_count == total;
    int32_t value;
    if (no_more_moves) {
        value = best;
    } else {
        int32_t st_probe = PROBE_OK;
        value = probe_table(pos, false, 0, &st_probe);
        if (st_probe == PROBE_FAIL) {
            return (TbProbeValue) { .value = 0, .state = PROBE_FAIL };
        }
    }

    // DTZ stores a "don't care" value when the best value is a win.
    if (best >= value) {
        const int32_t state = (best > WDL_DRAW || no_more_moves) ? PROBE_ZEROING : PROBE_OK;
        return (TbProbeValue) { .value = best, .state = state };
    }
    return (TbProbeValue) { .value = value, .state = PROBE_OK };
}
