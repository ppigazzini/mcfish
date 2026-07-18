#include "repetition.h"

#include <string.h>

#include "attacks.h"
#include "bitboard.h"
#include "movegen.h"

// Hold the cuckoo hash of every reversible one-piece move key. Two slots per key
// (H1/H2) with cuckoo displacement, so a lookup is at most two probes.
static Key Cuckoo[8192];
static Move CuckooMove[8192];
static Key Zobrist_side_key;

static inline size_t cuckoo_h1(Key key) { return (size_t) (key & 0x1FFFULL); }
static inline size_t cuckoo_h2(Key key) { return (size_t) ((key >> 16) & 0x1FFFULL); }

void repetition_init(const Key (*zobrist_psq)[SQUARE_NB], Key zobrist_side) {
    static const Piece init_pieces[12] = { W_PAWN, W_KNIGHT, W_BISHOP, W_ROOK, W_QUEEN, W_KING,
                                           B_PAWN, B_KNIGHT, B_BISHOP, B_ROOK, B_QUEEN, B_KING };

    Zobrist_side_key = zobrist_side;
    memset(Cuckoo, 0, sizeof Cuckoo);
    memset(CuckooMove, 0, sizeof CuckooMove);

    for (int p = 0; p < 12; ++p) {
        const Piece pc = init_pieces[p];
        const PieceType pt = type_of_piece(pc);
        if (pt == PAWN)
            continue;  // a pawn move is never reversible, so it cannot close a cycle

        for (Square s1 = SQ_A1; s1 <= SQ_H8; ++s1) {
            for (Square s2 = (Square) (s1 + 1); s2 <= SQ_H8; ++s2) {
                if ((attacks_bb(pt, s1, 0) & square_bb(s2)) == 0)
                    continue;

                Move move = make_move(s1, s2);
                Key key = zobrist_psq[pc][s1] ^ zobrist_psq[pc][s2] ^ zobrist_side;

                // Displace whatever occupies the slot and re-place it, until the
                // displaced entry is the empty one.
                size_t i = cuckoo_h1(key);
                for (;;) {
                    const Key tk = Cuckoo[i];
                    Cuckoo[i] = key;
                    key = tk;

                    const Move tm = CuckooMove[i];
                    CuckooMove[i] = move;
                    move = tm;

                    if (move == MOVE_NONE)
                        break;
                    i = (i == cuckoo_h1(key)) ? cuckoo_h2(key) : cuckoo_h1(key);
                }
            }
        }
    }
}

bool pos_upcoming_repetition(const Position *pos, int ply) {
    const int rule50 = pos->st->rule50;
    const int plies_from_null = pos->st->plies_from_null;
    const int end = rule50 < plies_from_null ? rule50 : plies_from_null;
    if (end < 3)
        return false;

    const Key original_key = pos->st->key;
    const StateInfo *stp = pos->st->previous;
    Key other = original_key ^ stp->key ^ Zobrist_side_key;

    for (int i = 3; i <= end; i += 2) {
        stp = stp->previous;
        other ^= stp->key ^ stp->previous->key ^ Zobrist_side_key;
        stp = stp->previous;

        if (other != 0)
            continue;

        const Key move_key = original_key ^ stp->key;
        size_t j = cuckoo_h1(move_key);
        if (Cuckoo[j] != move_key) {
            j = cuckoo_h2(move_key);
            if (Cuckoo[j] != move_key)
                continue;
        }

        const Move mv = CuckooMove[j];
        const Square s1 = move_from(mv);
        const Square s2 = move_to(mv);
        if (((BetweenBB[s1][s2] ^ square_bb(s2)) & pieces(pos)) == 0) {
            if (ply > i)
                return true;

            // At or before the root, require the cycle to be a repetition rather
            // than merely a move into the current position.
            if (state_repetition(stp) != 0)
                return true;
        }
    }
    return false;
}

bool pos_is_draw(const Position *pos, int ply) {
    if (pos->st->rule50 > 99) {
        if (pos->st->checkers == 0)
            return true;
        ExtMove list[MAX_MOVES];
        if (generate_legal(pos, list) != list)
            return true;  // in check, but not mate: the fifty-move rule still draws
    }
    return pos_is_repetition(pos, ply);
}

bool pos_is_repetition(const Position *pos, int ply) {
    const int rep = state_repetition(pos->st);
    return rep != 0 && rep < ply;
}

bool pos_has_repeated(const Position *pos) {
    const StateInfo *stc = pos->st;
    const int rule50 = pos->st->rule50;
    const int plies_from_null = pos->st->plies_from_null;
    int end = rule50 < plies_from_null ? rule50 : plies_from_null;

    while (end >= 4) {
        if (state_repetition(stc) != 0)
            return true;
        stc = stc->previous;
        --end;
    }
    return false;
}
