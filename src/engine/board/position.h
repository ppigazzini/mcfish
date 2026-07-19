// Own the board state: the piece placement, the incrementally-maintained derived
// state, and the make/unmake transition.
//
// The split matters. `Position` holds what a move rewrites in place (the boards,
// the side to move); `StateInfo` holds what a move cannot recompute cheaply on
// the way back (the captured piece, the castling rights, the Zobrist key). Undo
// restores by popping a StateInfo, never by recomputing — so any field added to
// StateInfo must be written by pos_do_move before the recursion, or unmake will
// restore a stale value.

#ifndef MCFISH_POSITION_H
#define MCFISH_POSITION_H

#include "attacks.h"
#include "bitboard.h"
#include "types.h"

typedef struct StateInfo {
    Key key;
    // Hash the MATERIAL only — every piece of each kind, counted, with no square
    // information: `Zobrist_psq[pc][8 + cnt]` for cnt in [0, count). Syzygy looks
    // its tables up by this, so it must be maintained incrementally by pos_do_move
    // and stay equal to the from-scratch definition in compute_key.
    // Upstream: Position::material_key (position.cpp:530 compute_material_key).
    Key material_key;
    // Index the history and correction tables. Semantics are upstream's
    // (position.cpp: set_state), NOT "everything of that kind":
    //   pawn_key        pawns ONLY, seeded with Zobrist_no_pawns so an empty
    //                   pawn structure still has a distinct key
    //   non_pawn_key    every non-pawn of that color, kings INCLUDED
    //   minor_piece_key knights and bishops only, kings EXCLUDED
    // Putting a king in or out of the wrong one silently mis-indexes a history
    // table, which costs strength without ever failing a gate.
    Key pawn_key;
    Key minor_piece_key;
    Key non_pawn_key[COLOR_NB];
    // Distance in plies back to the previous occurrence of this position:
    // positive for the first repetition, NEGATIVE when that earlier occurrence
    // was itself a repetition (i.e. this is the threefold), 0 when never
    // repeated. The sign is the whole encoding — upstream position.cpp:1046.
    int repetition;
    int rule50;  // halfmove clock, in plies
    int plies_from_null;
    Square ep_square;  // SQ_NONE when no en-passant capture is available
    uint8_t castling_rights;
    Piece captured_piece;
    Bitboard checkers;            // pieces of the side NOT to move giving check
    Bitboard blockers[COLOR_NB];  // own pieces whose move could expose our king
    Bitboard pinners[COLOR_NB];
    struct StateInfo *previous;
} StateInfo;

typedef struct Position {
    Bitboard by_type[PIECE_TYPE_NB];  // index 0 (ALL_PIECES) is the occupancy
    Bitboard by_color[COLOR_NB];
    Piece board[SQUARE_NB];
    int piece_count[PIECE_NB];
    Color side_to_move;
    int game_ply;
    // Track the rook origin per castling right so Chess960 castling is a data
    // lookup, not a special case in movegen.
    Square castling_rook_square[16];
    uint8_t castling_rights_mask[SQUARE_NB];
    bool chess960;
    StateInfo *st;
    // Hold the per-move NNUE deltas for callers that have no accumulator arena to
    // write into. They live only between pos_do_move and the read of them;
    // pos_undo_move does not restore them. Upstream Position::scratch_dp /
    // scratch_dts (position.h:439).
    DirtyPiece scratch_dp;
    DirtyThreats scratch_dts;
} Position;

// Build the Zobrist tables. Call once, after bitboards_init.
void position_init(void);

// Set POS from a FEN record, anchoring its state chain at SI.
// Return false and leave POS unspecified when the record is malformed.
bool pos_set(Position *pos, const char *fen, bool chess960, StateInfo *si);

// Set a position and, on failure, report why in upstream's words. `reason` may be
// nullptr, in which case this is exactly pos_set. The string is a literal with
// static lifetime; the caller does not own it.
bool pos_set_reason(
  Position *pos, const char *fen, bool chess960, StateInfo *si, const char **reason);

// Write POS's FEN into BUF (needs >= 128 bytes).
void pos_fen(const Position *pos, char *buf);

// Render POS as an ASCII board plus the FEN and key, as UCI `d` prints it.
void pos_pretty(const Position *pos, char *buf, int buf_len);

// Make M, pushing NEW_ST onto the state chain and writing the move's NNUE deltas
// into DP and DTS. Both out-parameters are required: they are the slots
// nnue_acc_stack_push hands out, so the make-move writes through into the
// accumulator's arena and nothing is copied. Callers with no accumulator pass
// `&pos->scratch_dp` and `&pos->scratch_dts`.
//
// GIVES_CHECK is upstream's parameter (position.cpp:815), where it selects the new
// checkers set. mcfish's set_check_info recomputes checkers from the board on every
// move, so the value is accepted for signature parity and is not yet read; when
// Position::gives_check is ported, the checkers assignment moves here and starts
// trusting it.
void pos_do_move(
  Position *pos, Move m, StateInfo *new_st, bool gives_check, DirtyPiece *dp, DirtyThreats *dts);
void pos_undo_move(Position *pos, Move m);

// Flip the side to move without touching a piece. DP and DTS are filled with the
// empty delta, so the incremental accumulator step is a no-op rather than a read of
// a stale ply's diff.
void pos_do_null_move(Position *pos, StateInfo *new_st, DirtyPiece *dp, DirtyThreats *dts);
void pos_undo_null_move(Position *pos);

// Test whether M — which must be pseudo-legal for POS — leaves our king safe.
bool pos_legal(const Position *pos, Move m);

// Test whether M could have been generated for POS. This is the guard a move from
// an untrusted source (a TT hit, a killer, a UCI string) must pass before
// pos_legal, which assumes pseudo-legality. Upstream Position::pseudo_legal.
bool pos_pseudo_legal(const Position *pos, Move m);

// Test whether the static exchange evaluation of M is at least THRESHOLD, without
// generating the exchange sequence. Non-NORMAL moves are assumed to pass a
// zero-threshold test, as upstream does. Upstream Position::see_ge.
bool pos_see_ge(const Position *pos, Move m, Value threshold);

// Return the set of pieces of either color attacking S, given OCCUPIED.
Bitboard pos_attackers_to_occ(const Position *pos, Square s, Bitboard occupied);

static inline Piece piece_on(const Position *pos, Square s) { return pos->board[s]; }
static inline bool is_empty(const Position *pos, Square s) { return pos->board[s] == NO_PIECE; }
static inline Bitboard pieces(const Position *pos) { return pos->by_type[ALL_PIECES]; }
static inline Bitboard pieces_p(const Position *pos, PieceType pt) { return pos->by_type[pt]; }
static inline Bitboard pieces_c(const Position *pos, Color c) { return pos->by_color[c]; }

static inline Bitboard pieces_cp(const Position *pos, Color c, PieceType pt) {
    return pos->by_color[c] & pos->by_type[pt];
}

static inline int count_p(const Position *pos, Color c, PieceType pt) {
    return pos->piece_count[make_piece(c, pt)];
}

static inline Square king_square(const Position *pos, Color c) {
    return lsb(pieces_cp(pos, c, KING));
}

static inline Bitboard checkers(const Position *pos) { return pos->st->checkers; }
static inline Key pos_key(const Position *pos) { return pos->st->key; }
static inline Key pos_material_key(const Position *pos) { return pos->st->material_key; }
static inline Piece captured_piece(const Position *pos) { return pos->st->captured_piece; }

static inline bool is_capture(const Position *pos, Move m) {
    return (!is_empty(pos, move_to(m)) && move_type(m) != CASTLING) || move_type(m) == EN_PASSANT;
}

// Count the non-pawn material of C in centipawn-equivalent units.
Value pos_non_pawn_material(const Position *pos, Color c);

#endif  // MCFISH_POSITION_H
