// Own the per-move threat deltas the NNUE `full_threats` feature set consumes: the
// packed DirtyThreat word, the bounded list a single make/unmake fills, and the
// ray-pass geometry the discovered-threat scan needs.
//
// A DirtyThreat is one (attacker, attacked) pair that appeared or disappeared
// because a piece landed on or left a square. The invariant is that the recorded
// pairs are EXACTLY those the feature indexer encodes — never a superset. The
// filters below (can_slider_threat, and the per-piece-type masks on `threatened`)
// are not an optimisation: the combinations they drop are the ones the index
// mapping sends out of range and the accumulator then discards, so recording them
// would be pure work. Adding a pair here that upstream rejects moves the eval.
//
// The list is bounded at 96 and never checked. A non-castling move changes at most
// (8 + 16) * 3 + 8 = 80 features and a castling move at most (5 + 1 + 3 + 9) * 2 =
// 36, so 80 bounds it; the remaining 16 slots exist so an unmasked vector store
// near the end of the list stays in bounds (Stockfish types.h:334).
//
// Ported from zfish `engine/board/move_do_threats.zig` and the ray-pass table in
// `engine/board/bitboard.zig:143`. Golden: `Stockfish/src/position.cpp:1183`
// (Position::update_piece_threats), `Stockfish/src/types.h:309` (DirtyThreat), and
// `Stockfish/src/nnue/features/full_threats.cpp` (what the pairs must contain).
//
// `DirtyThreats` itself, its packed-word offsets and its accessors live in
// `types.h`: this header names `Position`, `position.h` names `DirtyThreats`, and
// the record depends on neither. That is upstream's split exactly.

#ifndef CCFISH_THREATS_H
#define CCFISH_THREATS_H

#include <stddef.h>

#include "position.h"
#include "types.h"

// Build RayPassBB. Call once, after attacks_init and before any position exists.
void threats_init(void);

// Return RayPassBB[s1][s2]: from s1's empty-board attacks along the s1-s2 line,
// the squares at or beyond s2, with s1 removed from the occupancy. Zero when the
// two squares are not aligned.
Bitboard ray_pass_bb(Square s1, Square s2);

// Append to DTS every threat pair that PC on S creates (PUT_PIECE) or destroys.
//
// Call this with POS already holding the OTHER side of the transition: for a
// removal, before the boards are updated; for a placement, after. That ordering is
// what makes `occupied` correct for both directions.
//
// COMPUTE_RAY selects the discovered-threat scan. Pass true when the mover leaves
// or fills a square on the board (remove/put/move), false for the in-place
// swap-piece path, where the occupancy does not change and no ray can be
// discovered — there `directSliders` are folded into the incoming list instead.
//
// NO_RAYS suppresses a discovered threat whose ray covers the whole given set:
// pass `from | to` for the two halves of a piece move, so the piece's own vacated
// and occupied squares do not register as a discovery, and all-ones otherwise.
void threats_update_piece(bool compute_ray,
                          const Position *pos,
                          Piece pc,
                          bool put_piece,
                          Square s,
                          DirtyThreats *dts,
                          Bitboard no_rays);

#endif  // CCFISH_THREATS_H
