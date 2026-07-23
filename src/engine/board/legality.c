#include "legality.h"

#include "attacks.h"
#include "bitboard.h"
#include "movegen.h"

// Isolate the least significant set bit. `~b + 1` is the two's-complement negation
// written so the wrap is on an unsigned type: `-b` on a signed one would be UB at
// the extremes.
static inline Bitboard least_significant_bb(Bitboard b) { return b & (~b + 1); }

bool pos_legal(const Position *pos, Move m) {
    const Color us = pos->side_to_move;
    const Square from = move_from(m);
    const Square to = move_to(m);

    // Compute the opponent colour and the king square inside the branch that consumes
    // each, as upstream Position::legal does: the dominant not-a-blocker exit needs
    // neither, and the hoisted loads do not sink past the early returns on their own.

    if (move_type(m) == EN_PASSANT) {
        // Two pieces leave the board at once, so no pin test covers this: rebuild
        // the occupancy and ask the question directly.
        const Color them = flip_color(us);
        const Square ksq = king_square(pos, us);
        const Square cap = sq_sub(to, us == WHITE ? NORTH : SOUTH);
        const Bitboard occ = (pieces(pos) ^ square_bb(from) ^ square_bb(cap)) | square_bb(to);
        return !(attacks_bb(ROOK, ksq, occ) & pieces_c(pos, them)
                 & (pos->by_type[ROOK] | pos->by_type[QUEEN]))
            && !(attacks_bb(BISHOP, ksq, occ) & pieces_c(pos, them)
                 & (pos->by_type[BISHOP] | pos->by_type[QUEEN]));
    }

    if (move_type(m) == CASTLING) {
        // `to` encodes the ROOK square (king-captures-rook), so derive the king's
        // real destination before walking the path.
        const Color them = flip_color(us);
        const bool king_side = to > from;
        const Square kto = make_square(king_side ? 6 : 2, rank_of(from));
        const int step = king_side ? 1 : -1;

        for (Square s = from; s != kto; s = (Square) ((int) s + step))
            if (pos_attackers_to_occ(pos, (Square) ((int) s + step), pieces(pos))
                & pieces_c(pos, them))
                return false;

        // In Chess960 the castling rook can itself be the piece screening the king
        // from an enemy slider (queen a1 behind the castling rook on b1): reject if
        // moving it uncovers a check. `to` is the rook square. Golden:
        // Stockfish/src/position.cpp:686.
        return !pos->chess960 || !(pos->st->blockers[us] & square_bb(to));
    }

    if (type_of_piece(piece_on(pos, from)) == KING)
        // Step the king off the board before testing, or a slider checking it
        // through the from-square looks blocked.
        return !(pos_attackers_to_occ(pos, to, pieces(pos) ^ square_bb(from))
                 & pieces_c(pos, flip_color(us)));

    // A non-king mover is legal iff it is not a blocker, or it stays on the pin ray.
    return !(pos->st->blockers[us] & square_bb(from)) || aligned(from, to, king_square(pos, us));
}

bool pos_pseudo_legal(const Position *pos, Move m) {
    const Color us = pos->side_to_move;
    const Color them = flip_color(us);
    const Square from = move_from(m);
    const Square to = move_to(m);
    const Piece pc = piece_on(pos, from);
    const Bitboard all = pieces(pos);

    // Take the slower but simpler path for non-NORMAL moves: membership in the
    // generator, exactly as upstream does.
    if (move_type(m) != NORMAL) {
        ExtMove list[MAX_MOVES];
        const ExtMove *const end =
          generate(pos, list, checkers(pos) != 0 ? GEN_EVASIONS : GEN_NON_EVASIONS);
        for (const ExtMove *it = list; it != end; ++it)
            if (it->move == m)
                return true;
        return false;
    }

    if (pc == NO_PIECE || color_of_piece(pc) != us)
        return false;
    if ((pieces_c(pos, us) & square_bb(to)) != 0)
        return false;

    if (type_of_piece(pc) == PAWN) {
        // A NORMAL pawn move can never land on a back rank: that would have to be
        // encoded as a PROMOTION.
        if (((rank_bb(7) | rank_bb(0)) & square_bb(to)) != 0)
            return false;

        const int push = us == WHITE ? 8 : -8;
        const bool cap =
          (pawn_attacks_bb(us, square_bb(from)) & pieces_c(pos, them) & square_bb(to)) != 0;
        const bool single = (int) from + push == (int) to && is_empty(pos, to);
        const bool dbl = (int) from + 2 * push == (int) to && relative_rank(us, from) == 1
                      && is_empty(pos, to) && is_empty(pos, (Square) ((int) to - push));

        if (!cap && !single && !dbl)
            return false;
    } else if ((attacks_bb(type_of_piece(pc), from, all) & square_bb(to)) == 0) {
        return false;
    }

    // Only NON-KING moves are constrained here, exactly as upstream constrains
    // them (position.cpp: pseudo_legal). A king move that walks into an attacked
    // square is rejected by `pos_legal`, not here: adding that test would make
    // this function stricter than upstream's and put an attackers_to scan on a
    // path upstream keeps cheap. Every caller pairs the two.
    if (checkers(pos) && type_of_piece(pc) != KING) {
        // Only a king move evades a double check.
        if (bb_more_than_one(checkers(pos)))
            return false;

        // Otherwise the move must block the checker or capture it -- BetweenBB
        // includes the checker's own square, so both are one test.
        if (!(BetweenBB[king_square(pos, us)][lsb(checkers(pos))] & square_bb(to)))
            return false;
    }

    return true;
}

bool pos_gives_check(const Position *pos, Move m) {
    const Color us = pos->side_to_move;
    const Color them = flip_color(us);
    const Square from = move_from(m);
    const Square to = move_to(m);

    // Compute the occupancy, the enemy-king bitboard and the move type inside the
    // branch that consumes each, as upstream Position::gives_check does: the dominant
    // paths -- the direct-check hit and the NORMAL fall-through -- need none of them,
    // and the hoisted loads do not sink past the early returns on their own.

    // Detect a direct check.
    if ((pos->st->check_squares[type_of_piece(piece_on(pos, from))] & square_bb(to)) != 0)
        return true;

    // Detect a discovered check: the mover was blocking, and either leaves the
    // line or is a castling king whose rook lands on it.
    if ((pos->st->blockers[them] & square_bb(from)) != 0)
        return (LineBB[from][to] & pieces_cp(pos, them, KING)) == 0 || move_type(m) == CASTLING;

    switch (move_type(m)) {
    case NORMAL :
        return false;
    case PROMOTION :
        return (attacks_bb(move_promotion(m), to, pieces(pos) ^ square_bb(from))
                & pieces_cp(pos, them, KING))
            != 0;
    case EN_PASSANT : {
        const Square capsq = make_square(file_of(to), rank_of(from));
        const Bitboard b = (pieces(pos) ^ square_bb(from) ^ square_bb(capsq)) | square_bb(to);
        const Square ksq = king_square(pos, them);
        const Bitboard our = pieces_c(pos, us);
        const Bitboard our_qr = our & (pos->by_type[QUEEN] | pos->by_type[ROOK]);
        const Bitboard our_qb = our & (pos->by_type[QUEEN] | pos->by_type[BISHOP]);
        return ((attacks_bb(ROOK, ksq, b) & our_qr) | (attacks_bb(BISHOP, ksq, b) & our_qb)) != 0;
    }
    default : {  // CASTLING: only the rook's destination can give check
        const Square base = to > from ? SQ_F1 : SQ_D1;
        const Square rto = (Square) ((int) base ^ (us * 56));
        return (pos->st->check_squares[ROOK] & square_bb(rto)) != 0;
    }
    }
}

bool see_ge(const Position *pos, Move m, int threshold) {
    if (move_type(m) != NORMAL)
        return 0 >= threshold;

    const Square from = move_from(m);
    const Square to = move_to(m);

    int swap = piece_value(type_of_piece(piece_on(pos, to))) - threshold;
    if (swap < 0)
        return false;

    swap = piece_value(type_of_piece(piece_on(pos, from))) - swap;
    if (swap <= 0)
        return true;

    Bitboard occupied = pieces(pos) ^ square_bb(from) ^ square_bb(to);
    Color stm = pos->side_to_move;
    Bitboard attackers = pos_attackers_to_occ(pos, to, occupied);
    int res = 1;

    const Bitboard bishops_queens = pos->by_type[BISHOP] | pos->by_type[QUEEN];
    const Bitboard rooks_queens = pos->by_type[ROOK] | pos->by_type[QUEEN];

    for (;;) {
        stm = flip_color(stm);
        attackers &= occupied;

        Bitboard stm_attackers = attackers & pieces_c(pos, stm);
        if (stm_attackers == 0)
            break;

        // Ignore an attacker that is pinned to its own king, but only while the
        // pinner is still on the board.
        if ((pos->st->pinners[flip_color(stm)] & occupied) != 0) {
            stm_attackers &= ~pos->st->blockers[stm];
            if (stm_attackers == 0)
                break;
        }

        res ^= 1;

        // Capture with the least valuable attacker each time, and fold in the
        // slider the capture may have uncovered behind it.
        Bitboard bb;
        if ((bb = stm_attackers & pos->by_type[PAWN]) != 0) {
            swap = PAWN_VALUE - swap;
            if (swap < res)
                break;
            occupied ^= least_significant_bb(bb);
            attackers |= attacks_bb(BISHOP, to, occupied) & bishops_queens;
        } else if ((bb = stm_attackers & pos->by_type[KNIGHT]) != 0) {
            swap = KNIGHT_VALUE - swap;
            if (swap < res)
                break;
            occupied ^= least_significant_bb(bb);
        } else if ((bb = stm_attackers & pos->by_type[BISHOP]) != 0) {
            swap = BISHOP_VALUE - swap;
            if (swap < res)
                break;
            occupied ^= least_significant_bb(bb);
            attackers |= attacks_bb(BISHOP, to, occupied) & bishops_queens;
        } else if ((bb = stm_attackers & pos->by_type[ROOK]) != 0) {
            swap = ROOK_VALUE - swap;
            if (swap < res)
                break;
            occupied ^= least_significant_bb(bb);
            attackers |= attacks_bb(ROOK, to, occupied) & rooks_queens;
        } else if ((bb = stm_attackers & pos->by_type[QUEEN]) != 0) {
            swap = QUEEN_VALUE - swap;
            occupied ^= least_significant_bb(bb);
            attackers |= (attacks_bb(BISHOP, to, occupied) & bishops_queens)
                       | (attacks_bb(ROOK, to, occupied) & rooks_queens);
        } else {
            // Only the king is left: reverse the result if the opponent still has
            // an attacker, because capturing into check is not available.
            return (attackers & ~pieces_c(pos, stm)) != 0 ? (res ^ 1) != 0 : res != 0;
        }
    }

    return res != 0;
}
