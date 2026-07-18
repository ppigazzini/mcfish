#include "movegen.h"

// Emit the four promotion pieces for a pawn arriving on the back rank. Captures
// and quiets both reach here; the queen goes in the capture set because it is the
// only promotion the capture-only search wants to see.
static ExtMove *make_promotions(ExtMove *list, Square to, Square from, GenType type) {
    if (type == GEN_CAPTURES || type == GEN_EVASIONS || type == GEN_NON_EVASIONS)
        (list++)->move = make_move_typed(PROMOTION, from, to, QUEEN);

    if (type == GEN_QUIETS || type == GEN_EVASIONS || type == GEN_NON_EVASIONS) {
        (list++)->move = make_move_typed(PROMOTION, from, to, ROOK);
        (list++)->move = make_move_typed(PROMOTION, from, to, BISHOP);
        (list++)->move = make_move_typed(PROMOTION, from, to, KNIGHT);
    }
    return list;
}

static ExtMove *
generate_pawn_moves(const Position *pos, ExtMove *list, Color us, Bitboard target, GenType type) {
    const Color them = flip_color(us);
    const Direction up = (us == WHITE) ? NORTH : SOUTH;
    const Direction up_right = (us == WHITE) ? NORTH_EAST : SOUTH_WEST;
    const Direction up_left = (us == WHITE) ? NORTH_WEST : SOUTH_EAST;
    const Bitboard rank7 = rank_bb(us == WHITE ? 6 : 1);
    const Bitboard rank3 = rank_bb(us == WHITE ? 2 : 5);
    const Bitboard empty = ~pieces(pos);
    const Bitboard enemies = pieces_c(pos, them);

    const Bitboard pawns_on7 = pieces_cp(pos, us, PAWN) & rank7;
    const Bitboard pawns_not7 = pieces_cp(pos, us, PAWN) & ~rank7;

    if (type != GEN_CAPTURES) {
        Bitboard b1 = shift_bb(up, pawns_not7) & empty;
        Bitboard b2 = shift_bb(up, b1 & rank3) & empty;

        // In evasions, target is the block-or-capture mask, so intersecting it
        // here is what keeps a pawn push from "evading" to an irrelevant square.
        if (type == GEN_EVASIONS) {
            b1 &= target;
            b2 &= target;
        }

        while (b1) {
            const Square to = pop_lsb(&b1);
            (list++)->move = make_move(sq_sub(to, up), to);
        }
        while (b2) {
            const Square to = pop_lsb(&b2);
            (list++)->move = make_move(sq_sub(sq_sub(to, up), up), to);
        }
    }

    if (pawns_on7) {
        Bitboard b1 = shift_bb(up_right, pawns_on7) & enemies;
        Bitboard b2 = shift_bb(up_left, pawns_on7) & enemies;
        Bitboard b3 = shift_bb(up, pawns_on7) & empty;

        if (type == GEN_EVASIONS) {
            b1 &= target;
            b2 &= target;
            b3 &= target;
        }

        while (b1) {
            const Square to = pop_lsb(&b1);
            list = make_promotions(list, to, sq_sub(to, up_right), type);
        }
        while (b2) {
            const Square to = pop_lsb(&b2);
            list = make_promotions(list, to, sq_sub(to, up_left), type);
        }
        while (b3) {
            const Square to = pop_lsb(&b3);
            list = make_promotions(list, to, sq_sub(to, up), type);
        }
    }

    if (type != GEN_QUIETS) {
        Bitboard b1 = shift_bb(up_right, pawns_not7) & enemies;
        Bitboard b2 = shift_bb(up_left, pawns_not7) & enemies;

        if (type == GEN_EVASIONS) {
            b1 &= target;
            b2 &= target;
        }

        while (b1) {
            const Square to = pop_lsb(&b1);
            (list++)->move = make_move(sq_sub(to, up_right), to);
        }
        while (b2) {
            const Square to = pop_lsb(&b2);
            (list++)->move = make_move(sq_sub(to, up_left), to);
        }

        if (pos->st->ep_square != SQ_NONE) {
            const Square ep = pos->st->ep_square;

            // The captured pawn sits behind the ep square, so in evasions the
            // relevant target test is on that pawn, not on the landing square.
            if (type != GEN_EVASIONS || (target & square_bb(sq_sub(ep, up)))) {
                Bitboard b = pawns_not7 & PawnAttacksBB[them][ep];
                while (b)
                    (list++)->move = make_move_typed(EN_PASSANT, pop_lsb(&b), ep, KNIGHT);
            }
        }
    }

    return list;
}

static ExtMove *
generate_piece_moves(const Position *pos, ExtMove *list, Color us, PieceType pt, Bitboard target) {
    Bitboard from_bb = pieces_cp(pos, us, pt);

    while (from_bb) {
        const Square from = pop_lsb(&from_bb);
        Bitboard b = attacks_bb(pt, from, pieces(pos)) & target;
        while (b)
            (list++)->move = make_move(from, pop_lsb(&b));
    }
    return list;
}

// Emit castling as king-captures-rook (`to` is the ROOK square). Check only the
// path is clear here; whether the king walks through attack is pos_legal's job.
static ExtMove *generate_castling(const Position *pos, ExtMove *list, Color us) {
    const Square ksq = king_square(pos, us);

    for (int side = 0; side < 2; ++side) {
        const CastlingRights cr = (CastlingRights) ((us == WHITE ? WHITE_OO : BLACK_OO) << side);

        if (!(pos->st->castling_rights & cr))
            continue;

        const Square rfrom = pos->castling_rook_square[cr];
        const Square kto = make_square(side == 0 ? 6 : 2, rank_of(ksq));
        const Square rto = make_square(side == 0 ? 5 : 3, rank_of(ksq));

        // Every square the king or rook must traverse has to be empty, ignoring
        // the two movers themselves — in Chess960 either may already stand there.
        const Bitboard movers = square_bb(ksq) | square_bb(rfrom);
        Bitboard path =
          (BetweenBB[ksq][kto] | BetweenBB[rfrom][rto] | square_bb(kto) | square_bb(rto)) & ~movers;

        if (path & pieces(pos))
            continue;

        (list++)->move = make_move_typed(CASTLING, ksq, rfrom, KNIGHT);
    }
    return list;
}

ExtMove *generate(const Position *pos, ExtMove *list, GenType type) {
    const Color us = pos->side_to_move;
    const Square ksq = king_square(pos, us);
    Bitboard target;

    if (type == GEN_EVASIONS) {
        const Bitboard checkers_bb = pos->st->checkers;

        // A double check can only be answered by a king move, so skip every other
        // generator rather than generating moves that pos_legal must all reject.
        if (!bb_more_than_one(checkers_bb)) {
            const Square checksq = lsb(checkers_bb);
            target = BetweenBB[ksq][checksq];

            list = generate_pawn_moves(pos, list, us, target, GEN_EVASIONS);
            for (PieceType pt = KNIGHT; pt <= QUEEN; ++pt)
                list = generate_piece_moves(pos, list, us, pt, target);
        }

        // Step the king anywhere off-check; pos_legal removes the squares still attacked.
        Bitboard b = PseudoAttacks[KING][ksq] & ~pieces_c(pos, us);
        while (b)
            (list++)->move = make_move(ksq, pop_lsb(&b));
        return list;
    }

    target = (type == GEN_CAPTURES) ? pieces_c(pos, flip_color(us))
           : (type == GEN_QUIETS)   ? ~pieces(pos)
                                    : ~pieces_c(pos, us);

    list = generate_pawn_moves(pos, list, us, target, type);
    for (PieceType pt = KNIGHT; pt <= QUEEN; ++pt)
        list = generate_piece_moves(pos, list, us, pt, target);

    Bitboard b = PseudoAttacks[KING][ksq] & target;
    while (b)
        (list++)->move = make_move(ksq, pop_lsb(&b));

    if (type != GEN_CAPTURES && pos->st->castling_rights)
        list = generate_castling(pos, list, us);

    return list;
}

ExtMove *generate_legal(const Position *pos, ExtMove *list) {
    ExtMove buf[MAX_MOVES];
    const ExtMove *end = generate(pos, buf, pos->st->checkers ? GEN_EVASIONS : GEN_NON_EVASIONS);

    for (const ExtMove *it = buf; it != end; ++it)
        if (pos_legal(pos, it->move))
            (list++)->move = it->move;

    return list;
}
