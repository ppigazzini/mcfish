#include "movegen.h"

#include "legality.h"

// Every helper below is always_inline into the four type-literal entry points,
// mirroring upstream's generate_all<Us, Type> template instantiation: US and
// TYPE are compile-time constants at each expansion, so the per-stage tests,
// the shift_bb direction switches and the attacks_bb piece dispatch all fold
// away instead of running per call.

// Emit the promotion pieces for a pawn arriving on the back rank.
//
// ENEMY says whether this promotion is a capture. It decides which stage the
// UNDER-promotions belong to (movegen.cpp:87): a capture-underpromotion is a
// capture and belongs with GEN_CAPTURES; a push-underpromotion is quiet. The
// queen goes in the capture set either way, because it is the only promotion the
// capture-only search wants to see.
//
// Dropping ENEMY leaves the union over both stages unchanged, so perft — which
// only ever asks for the full legal list — cannot see the mistake.
// The search can: MovePicker maps the two stages separately, so a misfiled
// capture-underpromotion is scored, SEE-tested and pruned as a quiet, and is
// generated after the whole capture stage instead of inside it.
__attribute__((always_inline)) static inline ExtMove *
make_promotions(ExtMove *list, Square to, Square from, GenType type, bool enemy) {
    const bool all = type == GEN_EVASIONS;

    if (type == GEN_CAPTURES || all)
        (list++)->move = make_move_typed(PROMOTION, from, to, QUEEN);

    if ((type == GEN_CAPTURES && enemy) || (type == GEN_QUIETS && !enemy) || all) {
        (list++)->move = make_move_typed(PROMOTION, from, to, ROOK);
        (list++)->move = make_move_typed(PROMOTION, from, to, BISHOP);
        (list++)->move = make_move_typed(PROMOTION, from, to, KNIGHT);
    }
    return list;
}

__attribute__((always_inline)) static inline ExtMove *
generate_pawn_moves(const Position *pos, ExtMove *list, Color us, Bitboard target, GenType type) {
    const Color them = flip_color(us);
    const Direction up = (us == WHITE) ? NORTH : SOUTH;
    const Direction up_right = (us == WHITE) ? NORTH_EAST : SOUTH_WEST;
    const Direction up_left = (us == WHITE) ? NORTH_WEST : SOUTH_EAST;
    const Bitboard rank7 = rank_bb(us == WHITE ? 6 : 1);
    const Bitboard rank3 = rank_bb(us == WHITE ? 2 : 5);
    const Bitboard empty = ~pieces(pos);
    const Bitboard enemies = pieces_c(pos, them);
    const Bitboard pinned = pos->st->blockers[us];
    const Square ksq = king_square(pos, us);

    const Bitboard pawns_on7 = pieces_cp(pos, us, PAWN) & rank7;
    const Bitboard pawns_not7 = pieces_cp(pos, us, PAWN) & ~rank7;
    // A pinned pawn stays on its pin ray: it may push only when pinned along the
    // king's file, and capture toward one side only when pinned along the diagonal
    // that side lies on (movegen.cpp:125). up_right runs along the a1-h8 diagonal
    // for both colours, up_left along the a8-h1 one.
    const Bitboard pushable = ~pinned | file_bb(file_of(ksq));
    const Bitboard can_capture_left = ~pinned | AntiDiagBB[ksq];
    const Bitboard can_capture_right = ~pinned | DiagBB[ksq];

    if (type != GEN_CAPTURES) {
        Bitboard b1 = shift_bb(up, pawns_not7 & pushable) & empty;
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
        Bitboard b1 = shift_bb(up_right, pawns_on7 & can_capture_right) & enemies;
        Bitboard b2 = shift_bb(up_left, pawns_on7 & can_capture_left) & enemies;
        Bitboard b3 = shift_bb(up, pawns_on7 & pushable) & empty;

        if (type == GEN_EVASIONS) {
            b1 &= target;
            b2 &= target;
            b3 &= target;
        }

        while (b1) {
            const Square to = pop_lsb(&b1);
            list = make_promotions(list, to, sq_sub(to, up_right), type, true);
        }
        while (b2) {
            const Square to = pop_lsb(&b2);
            list = make_promotions(list, to, sq_sub(to, up_left), type, true);
        }
        while (b3) {
            const Square to = pop_lsb(&b3);
            list = make_promotions(list, to, sq_sub(to, up), type, false);
        }
    }

    if (type != GEN_QUIETS) {
        Bitboard b1 = shift_bb(up_right, pawns_not7 & can_capture_right) & enemies;
        Bitboard b2 = shift_bb(up_left, pawns_not7 & can_capture_left) & enemies;

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

        const Square ep = pos->st->ep_square;

        // An en-passant capture cannot resolve a check discovered through the
        // square the double-pushed pawn left (movegen.cpp:177). `ep_square` is set
        // only when some capturer is legal, which leaves the diagonal pins to
        // filter here: each capturer arrives along one diagonal.
        if (ep != SQ_NONE && !(type == GEN_EVASIONS && (target & square_bb(sq_add(ep, up))))) {
            Bitboard b = pawns_not7
                       & ((can_capture_right & shift_bb((Direction) -up_right, square_bb(ep)))
                          | (can_capture_left & shift_bb((Direction) -up_left, square_bb(ep))));
            while (b)
                (list++)->move = make_move_typed(EN_PASSANT, pop_lsb(&b), ep, KNIGHT);
        }
    }

    return list;
}

// Emit the moves of US's PT pieces onto TARGET: the free pieces first, then the
// pinned ones confined to their pin ray -- in that order, as upstream emits them
// (movegen.cpp:213), because generation order is the tie order the picker's sort
// keeps. A pinned knight can never stay on its ray, so it has no second loop.
__attribute__((always_inline)) static inline ExtMove *
generate_piece_moves(const Position *pos, ExtMove *list, Color us, PieceType pt, Bitboard target) {
    const Square ksq = king_square(pos, us);
    const Bitboard pinned = pos->st->blockers[us];

    Bitboard from_bb = pieces_cp(pos, us, pt) & ~pinned;
    while (from_bb) {
        const Square from = pop_lsb(&from_bb);
        Bitboard b = attacks_bb(pt, from, pieces(pos)) & target;
        while (b)
            (list++)->move = make_move(from, pop_lsb(&b));
    }

    if (pt != KNIGHT) {
        from_bb = pieces_cp(pos, us, pt) & pinned;
        while (from_bb) {
            const Square from = pop_lsb(&from_bb);
            Bitboard b = attacks_bb(pt, from, pieces(pos)) & target & LineBB[ksq][from];
            while (b)
                (list++)->move = make_move(from, pop_lsb(&b));
        }
    }
    return list;
}

// Emit castling as king-captures-rook (`to` is the ROOK square), and only when it
// is legal: the path is clear, no square the king crosses or lands on is attacked,
// and the castling rook is not the piece screening the king (movegen.cpp:261).
// Only reached out of check, so the king's own square needs no test.
//
// The screening test reads the geometry, never the Chess960 option: the FEN
// parser adopts the first rook a `K`/`Q` token meets, so `4k3/8/8/8/8/8/8/qR2K3 w
// Q - 0 1` castles with a 960 geometry while the option says standard, and the
// enemy queen on a1 is screened only by the castling rook. Upstream tests the
// blockers unconditionally too.
static ExtMove *generate_castling(const Position *pos, ExtMove *list, Color us) {
    const Square ksq = king_square(pos, us);
    const Bitboard attacked = threats_by(pos, ALL_PIECES);

    for (int side = 0; side < 2; ++side) {
        const CastlingRights cr = (CastlingRights) ((us == WHITE ? WHITE_OO : BLACK_OO) << side);

        if (!(pos->st->castling_rights & cr))
            continue;

        const Square rfrom = pos->castling_rook_square[cr];

        // One AND against the occupancy, as upstream's castling_impeded is
        // (position.h:283): the path was precomputed with the right.
        if (pos->castling_path[cr] & pieces(pos))
            continue;

        // After castling, the king lands where it would in standard chess.
        const Square kto = make_square(rfrom > ksq ? 6 : 2, rank_of(ksq));
        if ((BetweenBB[ksq][kto] & attacked) || (pos->st->blockers[us] & square_bb(rfrom)))
            continue;

        (list++)->move = make_move_typed(CASTLING, ksq, rfrom, KNIGHT);
    }
    return list;
}

__attribute__((always_inline)) static inline ExtMove *
generate_all(const Position *pos, ExtMove *list, Color us, GenType type) {
    const Square ksq = king_square(pos, us);
    Bitboard target = 0;

    // A double check can only be answered by a king move, so skip every other
    // generator there.
    if (type != GEN_EVASIONS || !bb_more_than_one(pos->st->checkers)) {
        target = type == GEN_EVASIONS ? BetweenBB[ksq][lsb(pos->st->checkers)]
               : type == GEN_CAPTURES ? pieces_c(pos, flip_color(us))
                                      : ~pieces(pos);  // GEN_QUIETS

        list = generate_pawn_moves(pos, list, us, target, type);
        list = generate_piece_moves(pos, list, us, KNIGHT, target);
        list = generate_piece_moves(pos, list, us, BISHOP, target);
        list = generate_piece_moves(pos, list, us, ROOK, target);
        list = generate_piece_moves(pos, list, us, QUEEN, target);
    }

    // The king steps anywhere the opponent does not attack: off the board's own
    // pieces when evading, onto the stage's target otherwise.
    if (type == GEN_EVASIONS)
        target = ~pieces_c(pos, us);
    target &= ~threats_by(pos, ALL_PIECES);

    Bitboard b = PseudoAttacks[KING][ksq] & target;
    while (b)
        (list++)->move = make_move(ksq, pop_lsb(&b));

    if (type == GEN_QUIETS && pos->st->castling_rights)
        list = generate_castling(pos, list, us);

    return list;
}

ExtMove *generate_captures(const Position *pos, ExtMove *list) {
    return pos->side_to_move == WHITE ? generate_all(pos, list, WHITE, GEN_CAPTURES)
                                      : generate_all(pos, list, BLACK, GEN_CAPTURES);
}

ExtMove *generate_quiets(const Position *pos, ExtMove *list) {
    return pos->side_to_move == WHITE ? generate_all(pos, list, WHITE, GEN_QUIETS)
                                      : generate_all(pos, list, BLACK, GEN_QUIETS);
}

ExtMove *generate_evasions(const Position *pos, ExtMove *list) {
    return pos->side_to_move == WHITE ? generate_all(pos, list, WHITE, GEN_EVASIONS)
                                      : generate_all(pos, list, BLACK, GEN_EVASIONS);
}

ExtMove *generate(const Position *pos, ExtMove *list, GenType type) {
    switch (type) {
    case GEN_CAPTURES :
        return generate_captures(pos, list);
    case GEN_QUIETS :
        return generate_quiets(pos, list);
    default :
        return generate_evasions(pos, list);
    }
}

// Generate every legal move: the evasions in check, else the captures followed by
// the quiets -- upstream's generate<ALL> (movegen.cpp:305). The order is observable:
// the root move list is built from this function, and root order drives the whole
// search.
ExtMove *generate_legal(const Position *pos, ExtMove *list) {
    return pos->st->checkers ? generate_evasions(pos, list)
                             : generate_quiets(pos, generate_captures(pos, list));
}
