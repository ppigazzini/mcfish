#include "fen_parse.h"

#include <stdio.h>
#include <string.h>

#include "attacks.h"
#include "bitboard.h"
#include "zobrist.h"

// Map a FEN letter to its piece code by position. `strchr` on this table also
// matches the terminating NUL and the two interior spaces, so both are rejected
// explicitly at the call site.
static const char PieceToChar[] = " PNBRQK  pnbrqk";

// Place PC on S, maintaining every board that indexes it.
//
// Duplicated from position.c deliberately: parsing is the only writer that runs
// before a StateInfo chain exists, and position.c's copy is `static`. When the
// make/unmake split lands, both call one `board_core` definition — see
// PORT_NOTES_board_rest.md.
static void put_piece(Position *pos, Piece pc, Square s) {
    pos->board[s] = pc;
    pos->by_type[ALL_PIECES] |= square_bb(s);
    pos->by_type[type_of_piece(pc)] |= square_bb(s);
    pos->by_color[color_of_piece(pc)] |= square_bb(s);
    pos->piece_count[pc]++;
}

// Fill blockers[c] with the pieces standing between C's king and an enemy slider,
// and pinners[c] with those sliders. A blocker may be either color: an enemy
// blocker is the one that can discover a check by moving away.
static void slider_blockers(const Position *pos, Color c, Bitboard *blockers, Bitboard *pinners) {
    const Square ksq = king_square(pos, c);
    const Color them = flip_color(c);
    *blockers = 0;
    *pinners = 0;

    // Consider only sliders that would attack ksq on an empty board.
    Bitboard snipers =
      ((PseudoAttacks[ROOK][ksq] & (pos->by_type[ROOK] | pos->by_type[QUEEN]))
       | (PseudoAttacks[BISHOP][ksq] & (pos->by_type[BISHOP] | pos->by_type[QUEEN])))
      & pieces_c(pos, them);
    const Bitboard occupancy = pieces(pos) ^ snipers;

    while (snipers) {
        const Square sniper_sq = pop_lsb(&snipers);
        const Bitboard between = BetweenBB[ksq][sniper_sq] ^ square_bb(sniper_sq);
        const Bitboard b = between & occupancy;

        // Exactly one piece in the way makes a pin; two or more block each other.
        if (b && !bb_more_than_one(b)) {
            *blockers |= b;
            if (b & pieces_c(pos, c))
                *pinners |= square_bb(sniper_sq);
        }
    }
}

// Derive the cached check state from the board. Duplicated from position.c for the
// same reason as put_piece; it belongs in a future `state_setup` module that both
// call.
static void set_check_info(Position *pos) {
    slider_blockers(pos, WHITE, &pos->st->blockers[WHITE], &pos->st->pinners[BLACK]);
    slider_blockers(pos, BLACK, &pos->st->blockers[BLACK], &pos->st->pinners[WHITE]);

    const Square ksq = king_square(pos, pos->side_to_move);
    pos->st->checkers =
      pos_attackers_to_occ(pos, ksq, pieces(pos)) & pieces_c(pos, flip_color(pos->side_to_move));
}

// Recompute every key from the board. pos_do_move maintains them incrementally;
// this is the from-scratch definition it must agree with.
//
// The pawn/non-pawn/minor classification here is upstream's, not "everything of
// that kind": the king is IN non_pawn_key and OUT of minor_piece_key. Getting it
// wrong mis-indexes a history table without ever failing a gate.
static Key compute_key(Position *pos) {
    Key k = 0;

    pos->st->pawn_key = Zobrist_no_pawns;
    pos->st->minor_piece_key = 0;
    pos->st->non_pawn_key[WHITE] = 0;
    pos->st->non_pawn_key[BLACK] = 0;
    pos->st->material_key = 0;

    // Hash the material by COUNT, not by square: slot `8 + cnt` of the piece's own
    // Zobrist row, for each of its cnt copies. Offsetting by 8 keeps the slots off
    // the squares a real board uses, so a material key never collides with a
    // position key (Stockfish/src/position.cpp:530).
    for (Piece pc = W_PAWN; pc <= B_KING; ++pc)
        for (int cnt = 0; cnt < pos->piece_count[pc]; ++cnt)
            pos->st->material_key ^= Zobrist_psq[pc][8 + cnt];

    for (Square s = SQ_A1; s <= SQ_H8; ++s) {
        const Piece pc = pos->board[s];
        if (pc == NO_PIECE)
            continue;

        k ^= Zobrist_psq[pc][s];
        const PieceType pt = type_of_piece(pc);

        if (pt == PAWN) {
            pos->st->pawn_key ^= Zobrist_psq[pc][s];
        } else {
            pos->st->non_pawn_key[color_of_piece(pc)] ^= Zobrist_psq[pc][s];
            if (pt != KING && pt <= BISHOP)
                pos->st->minor_piece_key ^= Zobrist_psq[pc][s];
        }
    }

    if (pos->st->ep_square != SQ_NONE)
        k ^= Zobrist_enpassant[file_of(pos->st->ep_square)];

    if (pos->side_to_move == BLACK)
        k ^= Zobrist_side;

    return k ^ Zobrist_castling[pos->st->castling_rights];
}

// Record the rook origin for CR and mark the squares whose vacation clears it, so
// pos_do_move can drop the right by masking on both the from- and to-square.
static void set_castling_right(Position *pos, Color c, Square rfrom) {
    const Square kfrom = king_square(pos, c);
    const CastlingRights cr =
      (CastlingRights) ((c == WHITE ? WHITE_OO : BLACK_OO) << (rfrom < kfrom));

    pos->st->castling_rights |= cr;
    pos->castling_rights_mask[kfrom] |= cr;
    pos->castling_rights_mask[rfrom] |= cr;
    pos->castling_rook_square[cr] = rfrom;
}

bool pos_set(Position *pos, const char *fen, bool chess960, StateInfo *si) {
    memset(pos, 0, sizeof(Position));
    memset(si, 0, sizeof(StateInfo));
    pos->st = si;
    si->ep_square = SQ_NONE;
    pos->chess960 = chess960;

    const char *p = fen;
    int f = 0, r = 7;

    // 1. Piece placement.
    for (; *p && *p != ' '; ++p) {
        if (*p == '/') {
            if (f != 8 || r == 0)
                return false;
            f = 0;
            --r;
        } else if (*p >= '1' && *p <= '8') {
            f += *p - '0';
            if (f > 8)
                return false;
        } else {
            const char *hit = strchr(PieceToChar, *p);
            if (!hit || *p == ' ' || f >= 8 || r < 0)
                return false;
            put_piece(pos, (Piece) (hit - PieceToChar), make_square(f, r));
            ++f;
        }
    }
    if (f != 8 || r != 0)
        return false;

    // Exactly one king per side, or every downstream king_square() reads garbage.
    if (count_p(pos, WHITE, KING) != 1 || count_p(pos, BLACK, KING) != 1)
        return false;

    // 2. Active color.
    while (*p == ' ')
        ++p;
    if (*p != 'w' && *p != 'b')
        return false;
    pos->side_to_move = (*p == 'w') ? WHITE : BLACK;
    ++p;

    // 3. Castling availability. Resolve K/Q against the outermost rook on the back
    // rank so a Chess960 record and a standard one take the same path.
    while (*p == ' ')
        ++p;
    for (; *p && *p != ' '; ++p) {
        if (*p == '-')
            continue;

        const Color c = (*p >= 'a' && *p <= 'z') ? BLACK : WHITE;
        const char t = (char) (*p >= 'a' ? *p - 32 : *p);
        const Bitboard rooks = pieces_cp(pos, c, ROOK) & rank_bb(c == WHITE ? 0 : 7);
        Square rsq;

        if (t == 'K')
            rsq = rooks ? msb(rooks) : SQ_NONE;
        else if (t == 'Q')
            rsq = rooks ? lsb(rooks) : SQ_NONE;
        else if (t >= 'A' && t <= 'H')
            rsq = make_square(t - 'A', c == WHITE ? 0 : 7);
        else
            return false;

        if (rsq == SQ_NONE || piece_on(pos, rsq) != make_piece(c, ROOK))
            return false;
        set_castling_right(pos, c, rsq);
    }

    // 4. En-passant square.
    while (*p == ' ')
        ++p;
    if (*p >= 'a' && *p <= 'h') {
        const int ef = *p - 'a';
        ++p;
        if (*p != '3' && *p != '6')
            return false;
        const Square ep = make_square(ef, *p - '1');
        ++p;

        // Keep the ep square only when the capture is actually available: a FEN
        // that states one unconditionally would otherwise perturb the key and
        // desynchronise every golden built on it.
        const Bitboard capturers = pieces_cp(pos, pos->side_to_move, PAWN)
                                 & PawnAttacksBB[flip_color(pos->side_to_move)][ep];
        if (capturers && piece_on(pos, ep) == NO_PIECE)
            si->ep_square = ep;
    } else if (*p == '-') {
        ++p;
    } else if (*p) {
        return false;
    }

    // 5-6. Halfmove clock and fullmove number. Both are optional: a record that
    // stops after the en-passant field is accepted with rule50 0 and fullmove 1.
    int rule50 = 0, fullmove = 1;
    if (sscanf(p, " %d %d", &rule50, &fullmove) < 1)
        rule50 = 0;
    si->rule50 = rule50;
    pos->game_ply = 2 * (fullmove > 0 ? fullmove - 1 : 0) + (pos->side_to_move == BLACK);

    si->key = compute_key(pos);
    set_check_info(pos);
    return true;
}
