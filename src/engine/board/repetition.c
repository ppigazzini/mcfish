#include "repetition.h"

#include <string.h>

#include "attacks.h"
#include "bitboard.h"
#include "movegen.h"

// Hold every reversible one-piece move key in a Robin Hood table (upstream
// RepetitionTable): open addressing from key & 8191, each entry recording its probe
// distance, one-based, so a zero distance marks an empty slot. A lookup walks forward
// from its home slot and stops at the first slot whose distance is shorter than its
// own, which a resident key can never have.
enum { REPETITION_TABLE_SIZE = 8192, REPETITION_TABLE_MASK = REPETITION_TABLE_SIZE - 1 };

static Key RepetitionKeys[REPETITION_TABLE_SIZE];
static Move RepetitionMoves[REPETITION_TABLE_SIZE];
static uint8_t RepetitionDistances[REPETITION_TABLE_SIZE];
static Key Zobrist_side_key;

static inline size_t repetition_hash(Key key) { return (size_t) (key & REPETITION_TABLE_MASK); }

void repetition_init(const Key (*zobrist_psq)[SQUARE_NB], Key zobrist_side) {
    static constexpr Piece init_pieces[12] = {
        W_PAWN, W_KNIGHT, W_BISHOP, W_ROOK, W_QUEEN, W_KING,
        B_PAWN, B_KNIGHT, B_BISHOP, B_ROOK, B_QUEEN, B_KING
    };

    Zobrist_side_key = zobrist_side;
    memset(RepetitionKeys, 0, sizeof RepetitionKeys);
    memset(RepetitionMoves, 0, sizeof RepetitionMoves);
    memset(RepetitionDistances, 0, sizeof RepetitionDistances);

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
                size_t i = repetition_hash(key);
                unsigned distance = 1;

                // Walk forward until an empty slot takes the entry, handing the slot to
                // whichever of the two is further from home and carrying the other on.
                for (;;) {
                    // A distance past a byte cannot be recorded; upstream refuses the table
                    // at compile time, and this table is built at startup.
                    if (distance > 255)
                        __builtin_trap();

                    if (RepetitionDistances[i] == 0) {
                        RepetitionKeys[i] = key;
                        RepetitionMoves[i] = move;
                        RepetitionDistances[i] = (uint8_t) distance;
                        break;
                    }

                    if (RepetitionDistances[i] < distance) {
                        const Key tk = RepetitionKeys[i];
                        RepetitionKeys[i] = key;
                        key = tk;

                        const Move tm = RepetitionMoves[i];
                        RepetitionMoves[i] = move;
                        move = tm;

                        const unsigned td = RepetitionDistances[i];
                        RepetitionDistances[i] = (uint8_t) distance;
                        distance = td;
                    }

                    ++distance;
                    i = (i + 1) & REPETITION_TABLE_MASK;
                }
            }
        }
    }
}

// Swap the pieces on S1 and S2 in a packed placement (upstream swap_nibble): the
// reversible move between them, applied to the nibbles alone.
static void swap_nibble(uint8_t placement[static 32], Square s1, Square s2) {
    const unsigned i1 = (unsigned) s1 & 31;
    const unsigned sh1 = ((unsigned) s1 & 0x20) >> 3;
    const unsigned i2 = (unsigned) s2 & 31;
    const unsigned sh2 = ((unsigned) s2 & 0x20) >> 3;
    const unsigned swap =
      ((unsigned) (placement[i1] >> sh1) ^ (unsigned) (placement[i2] >> sh2)) & 0xF;
    placement[i1] ^= (uint8_t) (swap << sh1);
    placement[i2] ^= (uint8_t) (swap << sh2);
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
        size_t j = repetition_hash(move_key);
        unsigned distance = 1;
        while (RepetitionDistances[j] >= distance && RepetitionKeys[j] != move_key) {
            ++distance;
            j = (j + 1) & REPETITION_TABLE_MASK;
        }
        if (RepetitionDistances[j] < distance)
            continue;

        const Move mv = RepetitionMoves[j];
        const Square s1 = move_from(mv);
        const Square s2 = move_to(mv);
        if (((BetweenBB[s1][s2] ^ square_bb(s2)) & pieces(pos)) == 0) {
            // A key match alone may be a collision: the move must also turn this
            // placement into that one (position.cpp:1595-1604).
            uint8_t moved[32];
            memcpy(moved, pos->st->piece_placement, sizeof moved);
            swap_nibble(moved, s1, s2);
            if (memcmp(moved, stp->piece_placement, sizeof moved) != 0)
                continue;

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
