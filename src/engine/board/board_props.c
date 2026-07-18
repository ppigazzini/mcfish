#include "board_props.h"

Color board_side_to_move(const Position *pos) { return pos->side_to_move; }

bool board_is_chess960(const Position *pos) { return pos->chess960; }

int board_game_ply(const Position *pos) { return pos->game_ply; }

bool board_has_checkers(const Position *pos) { return pos->st->checkers != 0; }

int board_wdl_material(const Position *pos) {
    const int *pc = pos->piece_count;
    return (pc[W_PAWN] + pc[B_PAWN]) + 3 * (pc[W_KNIGHT] + pc[B_KNIGHT])
         + 3 * (pc[W_BISHOP] + pc[B_BISHOP]) + 5 * (pc[W_ROOK] + pc[B_ROOK])
         + 9 * (pc[W_QUEEN] + pc[B_QUEEN]);
}

void board_copy_pieces(const Position *pos, Piece *pieces_out) {
    for (int s = 0; s < SQUARE_NB; ++s)
        pieces_out[s] = pos->board[s];
}
