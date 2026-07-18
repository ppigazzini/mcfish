#include "position.h"

#include <stdio.h>
#include <string.h>

#include "movegen.h"
#include "repetition.h"
#include "threats.h"

static Key Zobrist_psq[PIECE_NB][SQUARE_NB];
static Key Zobrist_enpassant[FILE_NB];
static Key Zobrist_castling[16];
static Key Zobrist_side;
static Key Zobrist_no_pawns;

// Generate the Zobrist keys from a fixed-seed xorshift64*, never from the host
// PRNG: the bench signature and every golden are functions of these keys, so a
// key that varies per run or per platform silently invalidates all of them.
static Key next_key(Key *s) {
    *s ^= *s >> 12;
    *s ^= *s << 25;
    *s ^= *s >> 27;
    return *s * 2685821657736338717ULL;
}

void position_init(void) {
    Key s = 1070372ULL;

    // Draw for the 12 REAL pieces only, skipping the encoding gaps at 7 and 8.
    // Upstream iterates a 12-element Pieces[] (position.cpp:61,124); drawing for
    // the gaps too would consume 128 extra PRNG values and shift every key from
    // B_PAWN onward away from upstream's table.
    static const Piece Pieces[12] = { W_PAWN, W_KNIGHT, W_BISHOP, W_ROOK, W_QUEEN, W_KING,
                                      B_PAWN, B_KNIGHT, B_BISHOP, B_ROOK, B_QUEEN, B_KING };

    for (int i = 0; i < 12; ++i)
        for (Square sq = SQ_A1; sq <= SQ_H8; ++sq)
            Zobrist_psq[Pieces[i]][sq] = next_key(&s);

    // Zero the ranks a pawn can only reach by promoting. A pawn never rests
    // there, so the entry is unreachable in compute_key; upstream zeroes it so
    // the promotion XOR cancels implicitly (position.cpp:126).
    for (int f = 0; f < FILE_NB; ++f) {
        Zobrist_psq[W_PAWN][make_square(f, 7)] = 0;
        Zobrist_psq[B_PAWN][make_square(f, 0)] = 0;
    }

    for (int f = 0; f < FILE_NB; ++f)
        Zobrist_enpassant[f] = next_key(&s);

    for (int cr = 0; cr < 16; ++cr)
        Zobrist_castling[cr] = next_key(&s);

    Zobrist_side = next_key(&s);
    Zobrist_no_pawns = next_key(&s);

    // Build the cuckoo table from the keys just drawn, here and only here: it is a
    // pure function of the psq and side keys, and repetition.h requires it be
    // filled after them. Skipping it leaves the table all zeros, which makes
    // pos_upcoming_repetition answer "no" for every position it is asked about
    // (upstream position.cpp: Position::init).
    repetition_init(Zobrist_psq, Zobrist_side);
}

// Toggle PC on SQ in the auxiliary keys. XOR is its own inverse, so the same call
// adds and removes — do_move pairs one call per square a piece leaves or enters.
// Mirrors compute_key's classification exactly; if one changes, both must.
static void toggle_aux_keys(StateInfo *st, Piece pc, Square sq) {
    const PieceType pt = type_of_piece(pc);

    if (pt == PAWN) {
        st->pawn_key ^= Zobrist_psq[pc][sq];
    } else {
        st->non_pawn_key[color_of_piece(pc)] ^= Zobrist_psq[pc][sq];
        if (pt != KING && pt <= BISHOP)
            st->minor_piece_key ^= Zobrist_psq[pc][sq];
    }
}

// Mutate the boards, recording the threat delta into DTS when it is non-NULL.
//
// The threat call sits on the side of the board update that leaves `occupied`
// describing the OTHER half of the transition: before the boards change for a
// removal, after for a placement. That ordering is what makes the discovered-threat
// scan see the right occupancy in each direction, so it is not free to move.
// Golden: Stockfish/src/position.h:383-437.

static void put_piece(Position *pos, Piece pc, Square s, DirtyThreats *dts) {
    pos->board[s] = pc;
    pos->by_type[ALL_PIECES] |= square_bb(s);
    pos->by_type[type_of_piece(pc)] |= square_bb(s);
    pos->by_color[color_of_piece(pc)] |= square_bb(s);
    pos->piece_count[pc]++;

    if (dts)
        threats_update_piece(true, pos, pc, true, s, dts, ALL_SQUARES_BB);
}

static void remove_piece(Position *pos, Square s, DirtyThreats *dts) {
    const Piece pc = pos->board[s];

    if (dts)
        threats_update_piece(true, pos, pc, false, s, dts, ALL_SQUARES_BB);

    pos->by_type[ALL_PIECES] ^= square_bb(s);
    pos->by_type[type_of_piece(pc)] ^= square_bb(s);
    pos->by_color[color_of_piece(pc)] ^= square_bb(s);
    pos->board[s] = NO_PIECE;
    pos->piece_count[pc]--;
}

static void move_piece(Position *pos, Square from, Square to, DirtyThreats *dts) {
    const Piece pc = pos->board[from];
    const Bitboard fromto = square_bb(from) | square_bb(to);

    // Pass `from | to` as the no-rays mask so the mover's own vacated and occupied
    // squares do not register as a discovery.
    if (dts)
        threats_update_piece(true, pos, pc, false, from, dts, fromto);

    pos->by_type[ALL_PIECES] ^= fromto;
    pos->by_type[type_of_piece(pc)] ^= fromto;
    pos->by_color[color_of_piece(pc)] ^= fromto;
    pos->board[from] = NO_PIECE;
    pos->board[to] = pc;

    if (dts)
        threats_update_piece(true, pos, pc, true, to, dts, fromto);
}

// Replace the piece on S in place — the capture-and-promote square. The occupancy
// is unchanged across the swap, so no ray can be discovered: pass compute_ray=false
// and let the direct sliders fold into the incoming list instead.
static void swap_piece(Position *pos, Square s, Piece pc, DirtyThreats *dts) {
    const Piece old = pos->board[s];

    remove_piece(pos, s, nullptr);

    if (dts)
        threats_update_piece(false, pos, old, false, s, dts, ALL_SQUARES_BB);

    put_piece(pos, pc, s, nullptr);

    if (dts)
        threats_update_piece(false, pos, pc, true, s, dts, ALL_SQUARES_BB);
}

Bitboard pos_attackers_to_occ(const Position *pos, Square s, Bitboard occupied) {
    return (PawnAttacksBB[BLACK][s] & pieces_cp(pos, WHITE, PAWN))
         | (PawnAttacksBB[WHITE][s] & pieces_cp(pos, BLACK, PAWN))
         | (PseudoAttacks[KNIGHT][s] & pos->by_type[KNIGHT])
         | (attacks_bb(ROOK, s, occupied) & (pos->by_type[ROOK] | pos->by_type[QUEEN]))
         | (attacks_bb(BISHOP, s, occupied) & (pos->by_type[BISHOP] | pos->by_type[QUEEN]))
         | (PseudoAttacks[KING][s] & pos->by_type[KING]);
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

static void set_check_info(Position *pos) {
    slider_blockers(pos, WHITE, &pos->st->blockers[WHITE], &pos->st->pinners[BLACK]);
    slider_blockers(pos, BLACK, &pos->st->blockers[BLACK], &pos->st->pinners[WHITE]);

    const Square ksq = king_square(pos, pos->side_to_move);
    pos->st->checkers =
      pos_attackers_to_occ(pos, ksq, pieces(pos)) & pieces_c(pos, flip_color(pos->side_to_move));
}

// Recompute every key from the board. do_move maintains them incrementally; this
// is the from-scratch definition both pos_set and the round-trip test compare against.
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
// do_move can drop the right by masking on both the from- and to-square.
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
            static const char *tokens = " PNBRQK  pnbrqk";
            const char *hit = strchr(tokens, *p);
            if (!hit || *p == ' ' || f >= 8 || r < 0)
                return false;
            put_piece(pos, (Piece) (hit - tokens), make_square(f, r), nullptr);
            ++f;
        }
    }
    if (f != 8 || r != 0)
        return false;

    // Exactly one king per side, or every downstream king_square() reads garbage.
    if (count_p(pos, WHITE, KING) != 1 || count_p(pos, BLACK, KING) != 1)
        return false;

    while (*p == ' ')
        ++p;
    if (*p != 'w' && *p != 'b')
        return false;
    pos->side_to_move = (*p == 'w') ? WHITE : BLACK;
    ++p;

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

    int rule50 = 0, fullmove = 1;
    if (sscanf(p, " %d %d", &rule50, &fullmove) < 1)
        rule50 = 0;
    si->rule50 = rule50;
    pos->game_ply = 2 * (fullmove > 0 ? fullmove - 1 : 0) + (pos->side_to_move == BLACK);

    si->key = compute_key(pos);
    set_check_info(pos);
    return true;
}

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
                n += sprintf(buf + n, "%c", " PNBRQK  pnbrqk"[piece_on(pos, make_square(f++, r))]);
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
                          " PNBRQK  pnbrqk"[piece_on(pos, make_square(f, r))]);
        n += snprintf(buf + n, (size_t) (buf_len - n),
                      " | %d\n +---+---+---+---+---+---+---+---+\n", r + 1);
    }
    n += snprintf(buf + n, (size_t) (buf_len - n), "   a   b   c   d   e   f   g   h\n");
    snprintf(buf + n, (size_t) (buf_len - n), "\nFen: %s\nKey: %016llX\n", fen,
             (unsigned long long) pos_key(pos));
}

bool pos_legal(const Position *pos, Move m) {
    const Color us = pos->side_to_move;
    const Color them = flip_color(us);
    const Square from = move_from(m);
    const Square to = move_to(m);
    const Square ksq = king_square(pos, us);

    if (move_type(m) == EN_PASSANT) {
        // Two pieces leave the board at once, so no pin test covers this: rebuild
        // the occupancy and ask the question directly.
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
        const bool king_side = to > from;
        const Square kto = make_square(king_side ? 6 : 2, rank_of(from));
        const int step = king_side ? 1 : -1;

        for (Square s = from; s != kto; s = (Square) ((int) s + step))
            if (pos_attackers_to_occ(pos, (Square) ((int) s + step), pieces(pos))
                & pieces_c(pos, them))
                return false;
        return true;
    }

    if (type_of_piece(piece_on(pos, from)) == KING)
        // Step the king off the board before testing, or a slider checking it
        // through the from-square looks blocked.
        return !(pos_attackers_to_occ(pos, to, pieces(pos) ^ square_bb(from))
                 & pieces_c(pos, them));

    // A non-king mover is legal iff it is not a blocker, or it stays on the pin ray.
    return !(pos->st->blockers[us] & square_bb(from)) || aligned(from, to, ksq);
}

// Move the king and rook to their castled squares, reporting the rook's origin and
// destination through RFROM_OUT / RTO_OUT because `*to` is overwritten with the
// king's destination and no longer identifies the side castled — in Chess960 the
// king may move toward a lower file while castling king-side.
//
// Both pieces may already sit on their destination in Chess960, so lift both before
// placing either. The Do direction records into DP and DTS; Undo passes neither,
// because the accumulator pops its stack rather than replaying deltas backwards.
// Golden: Stockfish/src/position.cpp:1345.
static void do_castling(Position *pos,
                        Color us,
                        Square from,
                        Square *to,
                        bool undo,
                        Square *rfrom_out,
                        Square *rto_out,
                        DirtyPiece *dp,
                        DirtyThreats *dts) {
    const bool king_side = *to > from;
    const Square rfrom = *to;
    const Square rto = make_square(king_side ? 5 : 3, rank_of(from));
    const Square kto = make_square(king_side ? 6 : 2, rank_of(from));
    *to = kto;
    *rfrom_out = rfrom;
    *rto_out = rto;

    if (dp) {
        dp->to = (uint8_t) kto;
        dp->remove_pc = dp->add_pc = (uint8_t) make_piece(us, ROOK);
        dp->remove_sq = (uint8_t) rfrom;
        dp->add_sq = (uint8_t) rto;
    }

    remove_piece(pos, undo ? kto : from, dts);
    remove_piece(pos, undo ? rto : rfrom, dts);
    put_piece(pos, make_piece(us, KING), undo ? from : kto, dts);
    put_piece(pos, make_piece(us, ROOK), undo ? rfrom : rto, dts);
}

void pos_do_move(
  Position *pos, Move m, StateInfo *new_st, bool gives_check, DirtyPiece *dp, DirtyThreats *dts) {
    (void) gives_check;  // set_check_info recomputes checkers; see position.h

    const Color us = pos->side_to_move;
    const Color them = flip_color(us);
    const Square from = move_from(m);
    Square to = move_to(m);
    const Piece pc = piece_on(pos, from);
    const MoveType mt = move_type(m);

    // Copy the whole record forward, then overwrite what this move changes. Undo
    // pops the chain, so anything left unwritten here is restored stale.
    Key key = pos->st->key ^ Zobrist_side;
    memcpy(new_st, pos->st, sizeof(StateInfo));
    new_st->previous = pos->st;
    pos->st = new_st;
    new_st->rule50++;
    new_st->plies_from_null++;
    pos->game_ply++;

    Piece captured = (mt == EN_PASSANT) ? make_piece(them, PAWN) : piece_on(pos, to);

    // Open the NNUE deltas before any piece moves: prev_ksq is the pre-move king
    // square, and the threat list must start empty.
    dp->pc = (uint8_t) pc;
    dp->from = (uint8_t) from;
    dp->to = (uint8_t) to;
    dp->add_sq = (uint8_t) SQ_NONE;
    dirty_threats_clear(dts);
    dts->us = us;
    dts->prev_ksq = king_square(pos, us);

    if (mt == CASTLING) {
        Square rfrom, rto;
        do_castling(pos, us, from, &to, false, &rfrom, &rto, dp, dts);
        const Piece rook = make_piece(us, ROOK);
        key ^= Zobrist_psq[rook][rfrom] ^ Zobrist_psq[rook][rto];
        toggle_aux_keys(new_st, rook, rfrom);
        toggle_aux_keys(new_st, rook, rto);
        captured = NO_PIECE;
    } else if (captured != NO_PIECE) {
        Square cap_sq = to;

        // Lift an en-passant victim here — it does not stand on `to`, so the
        // piece-mutation block below cannot reach it. Every other capture is
        // removed there, by the swap onto `to`.
        if (type_of_piece(captured) == PAWN && mt == EN_PASSANT) {
            cap_sq = sq_sub(to, us == WHITE ? NORTH : SOUTH);
            remove_piece(pos, cap_sq, dts);
        }

        key ^= Zobrist_psq[captured][cap_sq];
        toggle_aux_keys(new_st, captured, cap_sq);
        dp->remove_pc = (uint8_t) captured;
        dp->remove_sq = (uint8_t) cap_sq;

        // Retire the victim's LAST material slot. The count is still pre-capture
        // for a normal capture and already post-capture for en passant, which the
        // `- (mt != EN_PASSANT)` term reconciles (Stockfish/src/position.cpp:906).
        new_st->material_key ^=
          Zobrist_psq[captured][8 + pos->piece_count[captured] - (mt != EN_PASSANT ? 1 : 0)];
        new_st->rule50 = 0;
    } else {
        dp->remove_sq = (uint8_t) SQ_NONE;
    }

    // Key the mover from-to. For castling `to` is now the king's destination, so
    // this covers the king half of the move as well.
    key ^= Zobrist_psq[pc][from] ^ Zobrist_psq[pc][to];
    toggle_aux_keys(new_st, pc, from);
    toggle_aux_keys(new_st, pc, to);

    // Clear the previous ep square unconditionally: it is a one-ply right.
    if (pos->st->ep_square != SQ_NONE) {
        key ^= Zobrist_enpassant[file_of(pos->st->ep_square)];
        new_st->ep_square = SQ_NONE;
    }

    if (new_st->castling_rights
        && (pos->castling_rights_mask[from] | pos->castling_rights_mask[to])) {
        key ^= Zobrist_castling[new_st->castling_rights];
        new_st->castling_rights &=
          (uint8_t) ~(pos->castling_rights_mask[from] | pos->castling_rights_mask[to]);
        key ^= Zobrist_castling[new_st->castling_rights];
    }

    if (type_of_piece(pc) == PAWN) {
        new_st->rule50 = 0;

        // Set the ep square only when a capture is actually available, matching
        // pos_set: the key must not depend on whether the pawn merely could be taken.
        if ((to ^ from) == 16
            && (pawn_attacks_bb(us, square_bb(sq_sub(to, us == WHITE ? NORTH : SOUTH)))
                & pieces_cp(pos, them, PAWN))) {
            new_st->ep_square = sq_sub(to, us == WHITE ? NORTH : SOUTH);
            key ^= Zobrist_enpassant[file_of(new_st->ep_square)];
        } else if (mt == PROMOTION) {
            const Piece promoted = make_piece(us, move_promotion(m));

            dp->add_pc = (uint8_t) promoted;
            dp->add_sq = (uint8_t) to;
            dp->to = (uint8_t) SQ_NONE;  // the pawn never lands on `to`

            // Undo the pawn's arrival on `to` that the from-to key above assumed,
            // and place the promoted piece there instead. Upstream folds this into
            // one term because its Zobrist_psq[pawn][promotion rank] is zero;
            // ccfish's table has no such hole, so cancel it explicitly.
            key ^= Zobrist_psq[pc][to] ^ Zobrist_psq[promoted][to];
            toggle_aux_keys(new_st, pc, to);
            toggle_aux_keys(new_st, promoted, to);

            // Both counts are still pre-move here — the piece mutation is below —
            // so the new piece takes slot `count` and the pawn gives up slot
            // `count - 1` (Stockfish/src/position.cpp:973).
            new_st->material_key ^= Zobrist_psq[promoted][8 + pos->piece_count[promoted]]
                                  ^ Zobrist_psq[pc][8 + pos->piece_count[pc] - 1];
        }
    }

    // Mutate the boards LAST, and in upstream's shape: a promoting pawn goes
    // straight from `from` to the promoted piece on `to` and is never routed
    // through `to` as a pawn, and a capture replaces the victim in place rather
    // than moving onto a vacated square. The final position is the same either
    // way; the dirty-threat list is not, and it is the accumulator's input rather
    // than a derived fact (Stockfish/src/position.cpp:1013).
    if (mt != CASTLING) {
        const Piece to_pc = (mt == PROMOTION) ? make_piece(us, move_promotion(m)) : pc;

        if (captured != NO_PIECE && mt != EN_PASSANT) {
            remove_piece(pos, from, dts);
            swap_piece(pos, to, to_pc, dts);
        } else if (pc == to_pc) {
            move_piece(pos, from, to, dts);
        } else {
            remove_piece(pos, from, dts);
            put_piece(pos, to_pc, to, dts);
        }
    }

    new_st->captured_piece = captured;
    new_st->key = key;
    pos->side_to_move = them;
    set_check_info(pos);

    // Record the repetition distance. Walk back two plies at a time — only
    // same-side-to-move positions can repeat — bounded by the last irreversible
    // move, beyond which no position can recur. Mirrors position.cpp:1046.
    new_st->repetition = 0;
    const int end =
      new_st->rule50 < new_st->plies_from_null ? new_st->rule50 : new_st->plies_from_null;
    if (end >= 4) {
        const StateInfo *stp = new_st->previous->previous;
        for (int i = 4; i <= end; i += 2) {
            stp = stp->previous->previous;
            if (stp->key == new_st->key) {
                new_st->repetition = stp->repetition ? -i : i;
                break;
            }
        }
    }

    // Close the threat delta with the post-move king square. The accumulator
    // compares it against prev_ksq to decide whether the block must be rebuilt.
    dts->ksq = king_square(pos, us);
}

void pos_undo_move(Position *pos, Move m) {
    pos->side_to_move = flip_color(pos->side_to_move);

    const Color us = pos->side_to_move;
    const Square from = move_from(m);
    Square to = move_to(m);
    const MoveType mt = move_type(m);

    // Record nothing on the way back: the accumulator pops its stack rather than
    // replaying the deltas in reverse, so every mutator here takes a null DTS.
    if (mt == PROMOTION)
        swap_piece(pos, to, make_piece(us, PAWN), nullptr);

    if (mt == CASTLING) {
        Square rfrom, rto;
        do_castling(pos, us, from, &to, true, &rfrom, &rto, nullptr, nullptr);
    } else {
        move_piece(pos, to, from, nullptr);

        if (pos->st->captured_piece != NO_PIECE) {
            Square cap_sq = to;
            if (mt == EN_PASSANT)
                cap_sq = sq_sub(to, us == WHITE ? NORTH : SOUTH);
            put_piece(pos, pos->st->captured_piece, cap_sq, nullptr);
        }
    }

    pos->st = pos->st->previous;
    pos->game_ply--;
}

void pos_do_null_move(Position *pos, StateInfo *new_st, DirtyPiece *dp, DirtyThreats *dts) {
    // Fill both deltas with the empty move. A null move touches no piece, so the
    // incremental accumulator step must be a no-op — leaving these unwritten would
    // make it replay the previous ply's diff instead.
    dp->pc = (uint8_t) NO_PIECE;
    dp->from = (uint8_t) SQ_NONE;
    dp->to = (uint8_t) SQ_NONE;
    dp->remove_sq = (uint8_t) SQ_NONE;
    dp->add_sq = (uint8_t) SQ_NONE;
    dp->remove_pc = (uint8_t) NO_PIECE;
    dp->add_pc = (uint8_t) NO_PIECE;

    dirty_threats_clear(dts);
    dts->us = pos->side_to_move;
    dts->prev_ksq = dts->ksq = king_square(pos, pos->side_to_move);

    memcpy(new_st, pos->st, sizeof(StateInfo));
    new_st->previous = pos->st;
    pos->st = new_st;

    new_st->key ^= Zobrist_side;
    if (new_st->ep_square != SQ_NONE) {
        new_st->key ^= Zobrist_enpassant[file_of(new_st->ep_square)];
        new_st->ep_square = SQ_NONE;
    }
    new_st->rule50++;
    new_st->plies_from_null = 0;
    new_st->captured_piece = NO_PIECE;

    pos->side_to_move = flip_color(pos->side_to_move);
    set_check_info(pos);

    // A null move cannot repeat a position: it is not reachable by legal play.
    new_st->repetition = 0;
}

void pos_undo_null_move(Position *pos) {
    pos->st = pos->st->previous;
    pos->side_to_move = flip_color(pos->side_to_move);
}


bool pos_pseudo_legal(const Position *pos, Move m) {
    const Color us = pos->side_to_move;
    const Square from = move_from(m);
    const Square to = move_to(m);
    const Piece pc = piece_on(pos, from);

    // Answer the uncommon move types by membership in the generator: castling,
    // promotion and en passant each carry enough context that reproducing their
    // conditions here would be a second implementation to keep in step. The
    // generators are pseudo-legal, so this skips no legality check.
    if (move_type(m) != NORMAL) {
        ExtMove list[MAX_MOVES];
        const ExtMove *const end =
          generate(pos, list, checkers(pos) ? GEN_EVASIONS : GEN_NON_EVASIONS);
        for (const ExtMove *it = list; it != end; ++it)
            if (it->move == m)
                return true;
        return false;
    }

    if (pc == NO_PIECE || color_of_piece(pc) != us)
        return false;

    if (pieces_c(pos, us) & square_bb(to))
        return false;

    if (type_of_piece(pc) == PAWN) {
        // A NORMAL move never promotes, so a pawn reaching the last rank is a
        // mis-encoded move, not a legal one.
        if ((rank_bb(0) | rank_bb(7)) & square_bb(to))
            return false;

        const Direction push = (us == WHITE) ? NORTH : SOUTH;
        const bool is_capture =
          (PawnAttacksBB[us][from] & pieces_c(pos, flip_color(us)) & square_bb(to)) != 0;
        const bool is_single_push = sq_add(from, push) == to && is_empty(pos, to);
        const bool is_double_push = (int) from + 2 * (int) push == (int) to
                                 && relative_rank(us, from) == 1 && is_empty(pos, to)
                                 && is_empty(pos, sq_sub(to, push));

        if (!(is_capture || is_single_push || is_double_push))
            return false;
    } else if (!(attacks_bb(type_of_piece(pc), from, pieces(pos)) & square_bb(to))) {
        return false;
    }

    if (checkers(pos) && type_of_piece(pc) != KING) {
        // Only a king move evades a double check.
        if (bb_more_than_one(checkers(pos)))
            return false;

        // Otherwise the move must block the checker or capture it — BetweenBB
        // includes the checker's own square, so both are one test.
        if (!(BetweenBB[king_square(pos, us)][lsb(checkers(pos))] & square_bb(to)))
            return false;
    }

    return true;
}

bool pos_see_ge(const Position *pos, Move m, Value threshold) {
    // Assume the uncommon move types pass a simple exchange test, as upstream does.
    if (move_type(m) != NORMAL)
        return VALUE_ZERO >= threshold;

    const Square from = move_from(m);
    const Square to = move_to(m);

    // Track the exchange as a single running balance: `swap` is what the side to
    // recapture stands to gain, and `res` its parity. Both flip on every capture.
    Value swap = piece_value(type_of_piece(piece_on(pos, to))) - threshold;
    if (swap < 0)
        return false;

    swap = piece_value(type_of_piece(piece_on(pos, from))) - swap;
    if (swap <= 0)
        return true;

    // XOR `to` out of the occupancy as well as `from`: a piece pinned against the
    // capture square must not be treated as still blocking the ray it stands on.
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

        // Spend the least valuable attacker, then re-add whatever x-ray it uncovers.
        Bitboard bb;
        if ((bb = stm_attackers & pos->by_type[PAWN]) != 0) {
            swap = PAWN_VALUE - swap;
            if (swap < res)
                break;
            occupied ^= bb & (~bb + 1);
            attackers |= attacks_bb(BISHOP, to, occupied) & bishops_queens;
        } else if ((bb = stm_attackers & pos->by_type[KNIGHT]) != 0) {
            swap = KNIGHT_VALUE - swap;
            if (swap < res)
                break;
            occupied ^= bb & (~bb + 1);
        } else if ((bb = stm_attackers & pos->by_type[BISHOP]) != 0) {
            swap = BISHOP_VALUE - swap;
            if (swap < res)
                break;
            occupied ^= bb & (~bb + 1);
            attackers |= attacks_bb(BISHOP, to, occupied) & bishops_queens;
        } else if ((bb = stm_attackers & pos->by_type[ROOK]) != 0) {
            swap = ROOK_VALUE - swap;
            if (swap < res)
                break;
            occupied ^= bb & (~bb + 1);
            attackers |= attacks_bb(ROOK, to, occupied) & rooks_queens;
        } else if ((bb = stm_attackers & pos->by_type[QUEEN]) != 0) {
            swap = QUEEN_VALUE - swap;
            occupied ^= bb & (~bb + 1);
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

Value pos_non_pawn_material(const Position *pos, Color c) {
    return (Value) (count_p(pos, c, KNIGHT) * KNIGHT_VALUE + count_p(pos, c, BISHOP) * BISHOP_VALUE
                    + count_p(pos, c, ROOK) * ROOK_VALUE + count_p(pos, c, QUEEN) * QUEEN_VALUE);
}
