#include "position_query.h"

#include <string.h>

#include "bitboard.h"

Color pos_side_to_move(const Position *pos) { return pos->side_to_move; }

bool pos_is_chess960(const Position *pos) { return pos->chess960; }

int pos_game_ply(const Position *pos) { return pos->game_ply; }

bool pos_has_checkers(const Position *pos) { return pos->st->checkers != 0; }

int pos_wdl_material(const Position *pos) {
    const int *const pc = pos->piece_count;
    return (pc[W_PAWN] + pc[B_PAWN]) + 3 * (pc[W_KNIGHT] + pc[B_KNIGHT])
         + 3 * (pc[W_BISHOP] + pc[B_BISHOP]) + 5 * (pc[W_ROOK] + pc[B_ROOK])
         + 9 * (pc[W_QUEEN] + pc[B_QUEEN]);
}

void pos_fill_snapshot(const Position *pos, PositionSnapshot *out) {
    const StateInfo *const st = pos->st;

    out->side_to_move = pos->side_to_move;
    out->pieces_all = pos->by_type[ALL_PIECES];
    out->pieces_by_color[WHITE] = pos->by_color[WHITE];
    out->pieces_by_color[BLACK] = pos->by_color[BLACK];
    memcpy(out->pieces_by_type, pos->by_type, sizeof out->pieces_by_type);
    out->blockers_for_king[WHITE] = st->blockers[WHITE];
    out->blockers_for_king[BLACK] = st->blockers[BLACK];
    out->pinners[WHITE] = st->pinners[WHITE];
    out->pinners[BLACK] = st->pinners[BLACK];
    out->king_square[WHITE] = king_square(pos, WHITE);
    out->king_square[BLACK] = king_square(pos, BLACK);
    out->ep_square = st->ep_square;
    out->checkers = st->checkers;

    out->castling_rights = st->castling_rights;
    memcpy(out->castling_rook_square, pos->castling_rook_square, sizeof out->castling_rook_square);

    out->pawn_key = st->pawn_key;
    out->key = st->key;

    // Weight pawns at 534, not PAWN_VALUE: this is the scaling model's pawn, which
    // upstream keeps distinct from the search's.
    const int pawns = pos->piece_count[W_PAWN] + pos->piece_count[B_PAWN];
    out->material_value =
      (Value) (534 * pawns) + pos_non_pawn_material(pos, WHITE) + pos_non_pawn_material(pos, BLACK);

    out->rule50_count = st->rule50;
    out->game_ply = pos->game_ply;
    out->is_chess960 = pos->chess960;

    // Copy the board in bulk. It is the whole cost of this function: a square-at-a-
    // time loop is 64 scalar stores per node, where memcpy lowers to vector moves.
    memcpy(out->board, pos->board, sizeof out->board);
}

void pos_accumulator_snapshot(const Position *pos, Piece *pieces_out) {
    memcpy(pieces_out, pos->board, sizeof pos->board);
}
