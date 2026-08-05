// Upstream: bitboard.cpp (Bitboards::pretty) for the board rendering.
#include "fen.h"

#include <stdio.h>

// Map a piece code to its FEN letter. Index by the engine's piece encoding, so the
// two unused codes (7, 8) are the two spaces in the middle.

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

    // Under Chess960 a castling right is named by the FILE OF ITS ROOK, not by side:
    // the rook does not start on a1/h1, so `K` and `Q` do not identify it. Upstream
    // emits the rook file in Shredder-FEN notation (position.cpp:588-599), uppercase
    // for White and lowercase for Black. mcfish's PARSER already accepts that form,
    // so emitting KQkq here made the round-trip asymmetric: it read Shredder-FEN in
    // and wrote standard notation out, silently renaming the rights on the way.
    const uint8_t cr = pos->st->castling_rights;
    if (!cr)
        buf[n++] = '-';
    else {
        if (cr & WHITE_OO)
            buf[n++] =
              pos->chess960 ? (char) ('A' + file_of(pos->castling_rook_square[WHITE_OO])) : 'K';
        if (cr & WHITE_OOO)
            buf[n++] =
              pos->chess960 ? (char) ('A' + file_of(pos->castling_rook_square[WHITE_OOO])) : 'Q';
        if (cr & BLACK_OO)
            buf[n++] =
              pos->chess960 ? (char) ('a' + file_of(pos->castling_rook_square[BLACK_OO])) : 'k';
        if (cr & BLACK_OOO)
            buf[n++] =
              pos->chess960 ? (char) ('a' + file_of(pos->castling_rook_square[BLACK_OOO])) : 'q';
    }

    if (pos->st->ep_square == SQ_NONE)
        n += sprintf(buf + n, " -");
    else
        n += sprintf(buf + n, " %c%c", 'a' + file_of(pos->st->ep_square),
                     '1' + rank_of(pos->st->ep_square));

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
    // The RULE50-ADJUSTED key, not the raw one. Upstream's `d` prints `pos.key()`,
    // and `Position::key()` IS `adjust_key50<false>(st->key)` -- above a rule50 of 14
    // it mixes in `(rule50 - 14) / 8`. mcfish keeps the raw key in StateInfo and
    // adjusts at each reader instead (adjust_key50 for the probe, pos_do_move for the
    // prefetch), which is the same value everywhere it matters -- except here, where
    // the display had simply been left on the raw one.
    //
    // NO GATE IN THIS TREE COULD SEE IT. Every bench position and every golden case
    // sits below rule50 14, where the adjustment is the identity, so the two keys
    // agree everywhere the goldens look. That is the bit-exactness-is-not-faithfulness
    // trap in one line: the anchor pins the search, not what the shell reports about
    // positions nobody thought to choose.
    n += snprintf(buf + n, (size_t) (buf_len - n), "\nFen: %s\nKey: %016llX\nCheckers: ", fen,
                  (unsigned long long) pos_adjust_key50_of(pos_key(pos), pos->st->rule50));

    // Upstream lists the checking squares here, each followed by a space, and emits
    // the label even when there are none (position.cpp:84-87). The trailing space is
    // upstream's, not a slip: it writes `square + " "` per checker, so a position in
    // check ends the line with one. Reproduce the whitespace exactly -- this line is
    // compared byte-for-byte.
    for (Bitboard b = checkers(pos); b != 0;) {
        const Square s = pop_lsb(&b);
        n += snprintf(buf + n, (size_t) (buf_len - n), "%c%c ", 'a' + file_of(s), '1' + rank_of(s));
    }
    snprintf(buf + n, (size_t) (buf_len - n), "\n");
}
