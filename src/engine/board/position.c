#include "position.h"

#include <stdio.h>
#include <string.h>

#include "movegen.h"
#include "repetition.h"
#include "threats.h"
#include "zobrist.h"

void position_init(void) {
    // The key tables and their draw order belong to zobrist.c, which owns the one
    // seeded PRNG in the engine. Everything downstream of a key -- the TT, the
    // repetition cuckoo table, the bench signature, every golden -- is a function
    // of that draw order, so it lives in exactly one place.
    zobrist_init();

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

// Report WHY a FEN was rejected, in upstream's words.
//
// Upstream does not fall back or ignore: it returns a reason from set() and the UCI
// layer prints it and exits (uci.cpp:684). mcfish silently reset to the start
// position instead, which is worse than a wrong answer -- the engine went on
// answering `go` about a board the operator never asked for, and two goldens were
// generated over it and pinned it.
//
// The messages are upstream's verbatim (position.cpp), because a diagnostic that
// only approximates the reference is not comparable against it.
#define FEN_FAIL(msg) \
    do { \
        if (reason) \
            *reason = (msg); \
        return false; \
    } while (0)

// Three of upstream's messages name the offending character. Format into a static
// buffer: the reason outlives this frame (the shell prints it after the return) and
// a caller never owns it, matching the plain-literal cases above. One position is
// set at a time, so a single buffer cannot be contended.
static char FenFailBuf[64];

#define FEN_FAIL_CH(fmt, ch) \
    do { \
        snprintf(FenFailBuf, sizeof FenFailBuf, (fmt), (ch)); \
        if (reason) \
            *reason = FenFailBuf; \
        return false; \
    } while (0)

bool pos_set(Position *pos, const char *fen, bool chess960, StateInfo *si) {
    return pos_set_reason(pos, fen, chess960, si, nullptr);
}

bool pos_set_reason(
  Position *pos, const char *fen, bool chess960, StateInfo *si, const char **reason) {
    memset(pos, 0, sizeof(Position));
    memset(si, 0, sizeof(StateInfo));
    pos->st = si;
    si->ep_square = SQ_NONE;
    pos->chess960 = chess960;

    const char *p = fen;
    int f = 0, r = 7;

    for (; *p && *p != ' '; ++p) {
        if (*p == '/') {
            if (f != 8)
                FEN_FAIL("Invalid FEN. Trying to end rank when not at the end of it.");
            if (r == 0)
                FEN_FAIL("Invalid FEN. Invalid rank reached.");
            f = 0;
            --r;
        } else if (*p >= '0' && *p <= '9') {
            if (*p == '0' || *p == '9')
                FEN_FAIL("Invalid FEN. Invalid number of squares to skip.");
            f += *p - '0';
            if (f > 8)
                FEN_FAIL("Invalid FEN. Invalid number of squares to skip.");
        } else {
            static const char *tokens = " PNBRQK  pnbrqk";
            const char *hit = strchr(tokens, *p);
            if (!hit || *p == ' ')
                FEN_FAIL_CH("Invalid FEN. Invalid piece: %c", *p);
            if (f >= 8)
                FEN_FAIL("Invalid FEN. Invalid file reached.");
            if (r < 0)
                FEN_FAIL("Invalid FEN. Invalid rank reached.");
            put_piece(pos, (Piece) (hit - tokens), make_square(f, r), nullptr);
            ++f;
        }
    }
    if (f != 8 || r != 0)
        FEN_FAIL("Invalid FEN. Board state encoding ended but cursor not at end.");

    // A pawn cannot stand on the first or eighth rank -- it would have promoted.
    // Upstream rejects such a FEN before the king check (position.cpp:271-273), and
    // mcfish had no equivalent, so it accepted the position and then hashed it with
    // the promotion-rank Zobrist entries that position_init deliberately ZEROES.
    // Two distinct boards would collide on one key.
    if (pos->by_type[PAWN] & (rank_bb(0) | rank_bb(7)))
        FEN_FAIL("Unsupported position. Pawns on the first or eighth rank.");

    // Exactly one king per side, or every downstream king_square() reads garbage.
    if (count_p(pos, WHITE, KING) != 1 || count_p(pos, BLACK, KING) != 1)
        FEN_FAIL("Unsupported position. Incorrect number of kings.");

    while (*p == ' ')
        ++p;
    if (*p != 'w' && *p != 'b')
        FEN_FAIL_CH("Invalid FEN. Invalid side to move: %c", *p);
    pos->side_to_move = (*p == 'w') ? WHITE : BLACK;
    ++p;

    while (*p == ' ')
        ++p;
    // Resolve each castling right to a ROOK SQUARE, upstream's way (position.cpp).
    //
    // Two things here are not obvious and mcfish had both wrong.
    //
    // First, K/Q do not mean h-file/a-file. They mean "scan inward from that corner
    // and take the FIRST rook, stopping at the king" -- the king must come later
    // than the rook, or the right is not real. Taking msb/lsb of every back-rank
    // rook instead, as mcfish did, grants a kingside right to a rook sitting on the
    // far side of the king: with Kh1 and Ra1, upstream drops the right and mcfish
    // granted it with the a1 rook. That is a legal-move difference, not a message.
    //
    // Second, a right whose king or rook is missing is DROPPED, not an error
    // ("Only apply castling rights if they can be valid"). Only an unrecognised
    // character is an error. mcfish rejected the whole FEN, so a position upstream
    // accepts was refused outright.
    int cr_seen = 0;
    for (; *p && *p != ' '; ++p) {
        if (*p == '-')
            continue;
        if (++cr_seen > 4)
            FEN_FAIL("Invalid FEN. Maximum of 4 castling rights can be specified.");

        const Color c = (*p >= 'a' && *p <= 'z') ? BLACK : WHITE;
        const char t = (char) (*p >= 'a' ? *p - 32 : *p);
        const int back = c == WHITE ? 0 : 7;
        const Piece rook = make_piece(c, ROOK);
        const Piece king = make_piece(c, KING);
        Square rsq = SQ_NONE, ksq = SQ_NONE;

        if (t == 'K' || t == 'Q') {
            const int dir = t == 'K' ? -1 : 1;
            int file = t == 'K' ? 7 : 0;
            // Seven steps: with a castling right available the king is always on
            // files 2..7, so the last square never needs testing.
            for (int i = 0; i < 7; ++i, file += dir) {
                const Piece pc = piece_on(pos, make_square(file, back));
                if (pc == king) {
                    ksq = make_square(file, back);
                    break;
                }
                if (pc == rook && rsq == SQ_NONE)
                    rsq = make_square(file, back);
            }
        } else if (t >= 'A' && t <= 'H') {
            const Square cand = make_square(t - 'A', back);
            if (piece_on(pos, cand) == rook)
                rsq = cand;
            for (int file = 1; file < 7; ++file)
                if (piece_on(pos, make_square(file, back)) == king)
                    ksq = make_square(file, back);
        } else {
            FEN_FAIL_CH("Invalid FEN. Expected castling rights. Got: %c", *p);
        }

        if (ksq != SQ_NONE && rsq != SQ_NONE)
            set_castling_right(pos, c, rsq);
    }

    while (*p == ' ')
        ++p;
    if (*p >= 'a' && *p <= 'h') {
        const int ef = *p - 'a';
        ++p;
        if (*p != '3' && *p != '6')
            FEN_FAIL("Invalid FEN. Invalid en-passant square.");
        const Square ep = make_square(ef, *p - '1');
        ++p;

        // Keep the ep square only when the capture is actually LEGAL. A FEN that
        // states one unconditionally perturbs the key and desynchronises every
        // golden built on it, and so does one that stops at "a pawn attacks the
        // square": the capturer can be pinned to its own king, in which case no
        // capture exists and upstream drops the square. pos_do_move applies the
        // same rule, so a position keys identically whether it was parsed or
        // played into (Stockfish/src/position.cpp:388-418).
        const Color stm = pos->side_to_move;
        const Color other = flip_color(stm);
        const Direction up = stm == WHITE ? NORTH : SOUTH;

        Bitboard capturers = pieces_cp(pos, stm, PAWN) & PawnAttacksBB[other][ep];

        // The double-pushed pawn that would be taken, and the occupancy the
        // capture leaves: that pawn goes, and the ep square becomes occupied.
        const Bitboard target = pieces_cp(pos, other, PAWN) & square_bb(sq_sub(ep, up));
        const Bitboard occ = pieces(pos) ^ target ^ square_bb(ep);

        // a) a pawn threatens the ep square, b) the pushed pawn stands in front of
        // it, c) neither the ep square nor the one behind it is occupied.
        const bool available = capturers != 0 && target != 0
                            && (pieces(pos) & (square_bb(ep) | square_bb(sq_add(ep, up)))) == 0;

        // Record it only if some capturer can execute the capture without leaving
        // its own king in check.
        bool legal = false;
        while (capturers != 0) {
            const Bitboard after = occ ^ square_bb(pop_lsb(&capturers));
            legal |= (pos_attackers_to_occ(pos, king_square(pos, stm), after) & pieces_c(pos, other)
                      & ~target)
                  == 0;
        }

        if (available && legal)
            si->ep_square = ep;
    } else if (*p == '-') {
        ++p;
    } else if (*p) {
        FEN_FAIL("Invalid FEN. Invalid en-passant square.");
    }

    int rule50 = 0, fullmove = 1;
    if (sscanf(p, " %d %d", &rule50, &fullmove) < 1)
        rule50 = 0;
    si->rule50 = rule50;
    pos->game_ply = 2 * (fullmove > 0 ? fullmove - 1 : 0) + (pos->side_to_move == BLACK);

    si->key = compute_key(pos);
    set_check_info(pos);

    // Refuse a position where the side NOT to move is in check. It could only be
    // reached by a move that left its own king en prise, so it is unreachable in a
    // real game. Upstream rejects it after set_state (position.cpp:438); mcfish
    // accepted it, and the search would then happily generate a king capture --
    // nothing downstream prevents that, because every generator assumes this
    // invariant already holds.
    if (pos_attackers_to_occ(pos, king_square(pos, flip_color(pos->side_to_move)), pieces(pos))
        & pieces_c(pos, pos->side_to_move))
        FEN_FAIL("Unsupported position. King can be captured.");

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

// Swap the case of one ASCII letter and pass anything else through, so digits and
// `/` survive the board field untouched.
static char flip_case(char c) {
    if (c >= 'a' && c <= 'z')
        return (char) (c - 'a' + 'A');
    if (c >= 'A' && c <= 'Z')
        return (char) (c - 'A' + 'a');
    return c;
}

// Read one whitespace-delimited field starting at *P, advancing *P past it.
// Report its length, which is zero once the record is exhausted.
static int fen_field(const char **p, const char **start) {
    while (**p == ' ')
        ++*p;
    *start = *p;
    while (**p != '\0' && **p != ' ')
        ++*p;
    return (int) (*p - *start);
}

// Golden: `Stockfish/src/position.cpp: Position::flip` (1603-1634).
//
// Upstream builds the flipped record by inserting each rank at the FRONT of the
// string, which reverses their order, appends the side to move and the castling
// rights, and only then swaps the case of everything written so far. Doing the
// swap per character on the way out is the same transformation -- the fields it
// covers are exactly the board, the colour and the rights -- and it keeps the
// en-passant square and the counters out of it, which is what upstream's ordering
// achieves by appending them afterwards.
bool pos_flip_fen(const char *fen, char *out) {
    const char *p = fen;
    const char *field = nullptr;
    const int board_len = fen_field(&p, &field);
    if (board_len == 0)
        return false;

    // Split the board into ranks first: they have to be emitted back to front.
    const char *rank[8];
    int rank_len[8];
    int ranks = 0;
    for (const char *s = field; s < field + board_len && ranks < 8;) {
        const char *e = s;
        while (e < field + board_len && *e != '/')
            ++e;
        rank[ranks] = s;
        rank_len[ranks] = (int) (e - s);
        ++ranks;
        s = e < field + board_len ? e + 1 : e;
    }

    int n = 0;
    for (int i = ranks - 1; i >= 0; --i) {
        for (int k = 0; k < rank_len[i]; ++k)
            out[n++] = flip_case(rank[i][k]);
        if (i > 0)
            out[n++] = '/';
    }
    out[n++] = ' ';

    // Side to move. Upstream writes the OPPOSITE letter uppercase and lets the
    // case swap lowercase it, so `w` becomes `b` and anything else becomes `w`.
    const int stm_len = fen_field(&p, &field);
    out[n++] = stm_len == 1 && *field == 'w' ? 'b' : 'w';
    out[n++] = ' ';

    // Castling rights, case-swapped: `KQkq` names Black's rights after the flip,
    // and under Chess960 the same swap carries a rook file between the two cases.
    const int cr_len = fen_field(&p, &field);
    if (cr_len == 0)
        out[n++] = '-';
    else
        for (int i = 0; i < cr_len; ++i)
            out[n++] = flip_case(field[i]);
    out[n++] = ' ';

    // En passant. Mirror the rank between the two sides' third ranks; the file
    // letter is lowercase in both records, so it must NOT be swapped.
    const int ep_len = fen_field(&p, &field);
    if (ep_len == 2) {
        out[n++] = field[0];
        out[n++] = field[1] == '3' ? '6' : '3';
    } else
        out[n++] = '-';

    // The halfmove clock and the fullmove number describe the game, not a side,
    // and cross unchanged.
    while (*p == ' ')
        ++p;
    if (*p != '\0') {
        out[n++] = ' ';
        while (*p != '\0')
            out[n++] = *p++;
    }

    out[n] = '\0';
    return true;
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
    n += snprintf(buf + n, (size_t) (buf_len - n), "\nFen: %s\nKey: %016llX\nCheckers: ", fen,
                  (unsigned long long) pos_key(pos));

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

        // Set the ep square only when the capture is actually LEGAL, not merely
        // pseudo-legal. A pawn that stands to take en passant can still be pinned
        // to its own king, and the double push itself can be the piece that
        // discovers the check; in either case no capture exists and upstream
        // leaves the square unset. Recording it anyway keys the position
        // differently from upstream, which moves TT slots and correction-history
        // buckets and desynchronises the search
        // (Stockfish/src/position.cpp:936-956).
        if ((to ^ from) == 16) {
            const Square ep_sq = sq_sub(to, us == WHITE ? NORTH : SOUTH);
            const Bitboard capturers =
              pawn_attacks_bb(us, square_bb(ep_sq)) & pieces_cp(pos, them, PAWN);

            if (capturers) {
                const Square ksq = king_square(pos, them);
                const Bitboard not_blockers = ~new_st->previous->blockers[them];

                // The pushed pawn discovers a check unless it was not a blocker
                // for the enemy king, or it stays on the king's file by pushing.
                const bool no_discovery =
                  (square_bb(from) & not_blockers) != 0 || file_of(from) == file_of(ksq);

                // A capture exists when some capturer is not itself a blocker, or
                // one lies on the king's line through the ep square, so that
                // taking keeps it on its pin ray.
                if (no_discovery && (capturers & (not_blockers | LineBB[ep_sq][ksq]))) {
                    new_st->ep_square = ep_sq;
                    key ^= Zobrist_enpassant[file_of(ep_sq)];
                }
            }
        } else if (mt == PROMOTION) {
            const Piece promoted = make_piece(us, move_promotion(m));

            dp->add_pc = (uint8_t) promoted;
            dp->add_sq = (uint8_t) to;
            dp->to = (uint8_t) SQ_NONE;  // the pawn never lands on `to`

            // Undo the pawn's arrival on `to` that the from-to key above assumed,
            // and place the promoted piece there instead. Upstream folds this into
            // one term because its Zobrist_psq[pawn][promotion rank] is zero;
            // mcfish's table has no such hole, so cancel it explicitly.
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
    // Do NOT touch rule50. Upstream's do_null_move (position.cpp) advances
    // pliesFromNull only; a null move is not a real ply for the fifty-move
    // counter. Incrementing it here inflates the counter for the whole subtree
    // below the null and compounds with every null on the path, which feeds the
    // rule50 > 99 draw test, the eval's rule50 damping and is_shuffling -- so
    // subtrees get declared drawn early, worst in the low-material positions
    // where null-move pruning fires most.
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


Value pos_non_pawn_material(const Position *pos, Color c) {
    return (Value) (count_p(pos, c, KNIGHT) * KNIGHT_VALUE + count_p(pos, c, BISHOP) * BISHOP_VALUE
                    + count_p(pos, c, ROOK) * ROOK_VALUE + count_p(pos, c, QUEEN) * QUEEN_VALUE);
}
