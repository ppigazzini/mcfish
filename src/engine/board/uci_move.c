#include "uci_move.h"

#include "movegen.h"

#include <strings.h>
#include <string.h>

void move_to_uci(const Position *pos, Move m, char *buf) {
    if (m == MOVE_NONE) {
        strcpy(buf, "(none)");
        return;
    }
    if (m == MOVE_NULL) {
        strcpy(buf, "0000");
        return;
    }

    const Square from = move_from(m);
    Square to = move_to(m);

    // Standard chess reports the king's destination; Chess960 reports the rook's.
    if (move_type(m) == CASTLING && !pos->chess960)
        to = make_square(to > from ? 6 : 2, rank_of(from));

    int n = 0;
    buf[n++] = (char) ('a' + file_of(from));
    buf[n++] = (char) ('1' + rank_of(from));
    buf[n++] = (char) ('a' + file_of(to));
    buf[n++] = (char) ('1' + rank_of(to));

    if (move_type(m) == PROMOTION)
        buf[n++] = " pnbrqk"[move_promotion(m)];

    buf[n] = '\0';
}

Move move_from_uci(const Position *pos, const char *str) {
    ExtMove list[MAX_MOVES];
    const ExtMove *end = generate_legal(pos, list);
    char buf[8];

    for (const ExtMove *it = list; it != end; ++it) {
        move_to_uci(pos, it->move, buf);
        // Fold case before matching: upstream lowercases the input token first
        // (Stockfish/src/uci.cpp:628, UCIEngine::to_move), so "E2E4"/"E7E8Q" match
        // the always-lowercase rendered move.
        if (strcasecmp(buf, str) == 0)
            return it->move;
    }
    return MOVE_NONE;
}
