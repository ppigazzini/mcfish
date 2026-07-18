#include "fen.h"

#include <stdio.h>

// Map a piece code to its FEN letter. Index by the engine's piece encoding, so the
// two unused codes (7, 8) are the two spaces in the middle.
static const char PieceToChar[] = " PNBRQK  pnbrqk";

void pos_fen(const Position *pos, char *buf) {
    int n = 0;

    for (int r = 7; r >= 0; --r) {
        for (int f = 0; f < 8;) {
            int empty = 0;
            while (f < 8 && is_empty(pos, make_square(f, r))) {
                ++empty;
                ++f;
            }
            if (empty)
                n += sprintf(buf + n, "%d", empty);
            if (f < 8)
                n += sprintf(buf + n, "%c", PieceToChar[piece_on(pos, make_square(f++, r))]);
        }
        if (r > 0)
            buf[n++] = '/';
    }

    n += sprintf(buf + n, " %c ", pos->side_to_move == WHITE ? 'w' : 'b');

    const uint8_t cr = pos->st->castling_rights;
    if (!cr)
        buf[n++] = '-';
    else {
        if (cr & WHITE_OO)
            buf[n++] = 'K';
        if (cr & WHITE_OOO)
            buf[n++] = 'Q';
        if (cr & BLACK_OO)
            buf[n++] = 'k';
        if (cr & BLACK_OOO)
            buf[n++] = 'q';
    }

    if (pos->st->ep_square == SQ_NONE)
        n += sprintf(buf + n, " -");
    else
        n += sprintf(buf + n, " %c%c", 'a' + file_of(pos->st->ep_square),
                     '1' + rank_of(pos->st->ep_square));

    // Derive the fullmove number from the ply, undoing the black-to-move offset
    // pos_set folded in. Truncating division is what upstream does; a rounded one
    // shifts the number on every black move.
    sprintf(buf + n, " %d %d", pos->st->rule50,
            1 + (pos->game_ply - (pos->side_to_move == BLACK)) / 2);
}

void pos_pretty(const Position *pos, char *buf, int buf_len) {
    char fen[128];
    pos_fen(pos, fen);
    int n = 0;

    n += snprintf(buf + n, (size_t) (buf_len - n), "\n +---+---+---+---+---+---+---+---+\n");
    for (int r = 7; r >= 0; --r) {
        for (int f = 0; f < 8; ++f)
            n += snprintf(buf + n, (size_t) (buf_len - n), " | %c",
                          PieceToChar[piece_on(pos, make_square(f, r))]);
        n += snprintf(buf + n, (size_t) (buf_len - n),
                      " | %d\n +---+---+---+---+---+---+---+---+\n", r + 1);
    }
    n += snprintf(buf + n, (size_t) (buf_len - n), "   a   b   c   d   e   f   g   h\n");
    snprintf(buf + n, (size_t) (buf_len - n), "\nFen: %s\nKey: %016llX\n", fen,
             (unsigned long long) pos_key(pos));
}
