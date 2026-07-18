// Own `PositionSnapshot`: a flat, self-contained copy of everything a consumer
// needs to read off a `Position` without holding a pointer into the live one.
//
// The snapshot exists so the evaluation and the feature indexer never dereference
// `pos->st`. A `StateInfo` is a link in a chain the search rewrites as it
// recurses, so a reader that keeps the pointer reads a different ply than the one
// it asked about. A snapshot is a value: once filled it is stable, and the search
// may do whatever it likes to the position underneath.
//
// The snapshot is DERIVED, never authoritative. `position_query.c: pos_fill_snapshot`
// is its only writer, and nothing may write a snapshot field back into a Position.
//
// Ported from zfish `engine/board/position_snapshot.zig`. zfish routes `fill` and
// `moveIsLegal` through function-pointer hooks to break a Zig import cycle
// (position cannot be imported by its own importers); C has no such cycle, so the
// two entry points here call `position_query` and `legality` directly and no hook
// registration exists or is needed.

#ifndef CCFISH_POSITION_SNAPSHOT_H
#define CCFISH_POSITION_SNAPSHOT_H

#include "position_types.h"
#include "types.h"

typedef struct {
    Color side_to_move;
    Bitboard pieces_all;
    Bitboard pieces_by_color[COLOR_NB];
    Bitboard pieces_by_type[PIECE_TYPE_NB];
    Bitboard blockers_for_king[COLOR_NB];
    Bitboard pinners[COLOR_NB];
    Square king_square[COLOR_NB];
    Square ep_square;
    uint8_t castling_rights;
    // Index by the single-bit CastlingRights value (1, 2, 4, 8), not by 0..3 — the
    // right IS the index everywhere else in the tree, and a packed 4-entry array
    // here would be the one place that is not true.
    Square castling_rook_square[16];
    Bitboard checkers;
    Piece board[SQUARE_NB];
    Key pawn_key;
    Key key;
    Value material_value;
    int rule50_count;
    int game_ply;
    bool is_chess960;
} PositionSnapshot;

// Fill OUT from POS. Thin alias for pos_fill_snapshot; kept so the snapshot's own
// header is the whole surface a consumer needs.
void snapshot_fill(const Position *pos, PositionSnapshot *out);

// Test whether M — which must be pseudo-legal — is legal in POS. Thin alias for
// pos_legal, mirroring zfish's `moveIsLegal` hook.
bool snapshot_move_is_legal(const Position *pos, Move m);

#endif  // CCFISH_POSITION_SNAPSHOT_H
