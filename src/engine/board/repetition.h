// Own the repetition and fifty-move queries, plus the cuckoo table the
// upcoming-repetition test hashes into.
//
// The invariant every query rests on is that `pos_do_move` records, on each
// StateInfo, the ply distance back to the previous occurrence of the same key —
// negative when that occurrence was itself a repetition, zero when the position is
// new. Nothing here recomputes that; the queries only read it and walk the
// StateInfo chain. So the chain must be live for `min(rule50, plies_from_null)`
// plies back, which is exactly what a root StateInfo with plies_from_null == 0
// guarantees. Every walk here dereferences `previous` unguarded on that basis, as
// upstream does.
//
// Ported from zfish `engine/board/repetition.zig` and the cuckoo build in
// `engine/board/zobrist.zig:66`. Golden: `Stockfish/src/position.cpp:1526`
// (is_draw / is_repetition / has_repeated / upcoming_repetition) and
// `Stockfish/src/position.cpp` Position::init (the cuckoo table).

#ifndef CCFISH_REPETITION_H
#define CCFISH_REPETITION_H

#include "position.h"
#include "types.h"

// Build the cuckoo table from the Zobrist keys the position module generated.
// Take them as arguments rather than re-deriving them: two independently seeded
// copies of the same PRNG is exactly the drift this table cannot survive. Call
// once, from position_init, after the psq keys are filled.
void repetition_init(const Key (*zobrist_psq)[SQUARE_NB], Key zobrist_side);

// Report whether POS is drawn by the fifty-move rule or by repetition. PLY bounds
// the "inside the current search line" window, where a single repetition already
// scores as a draw (zfish repetition.zig:61).
bool pos_is_draw(const Position *pos, int ply);

// Report a repetition strictly after the root, or a threefold at or before it.
bool pos_is_repetition(const Position *pos, int ply);

// Report whether any repetition occurred since the last capture or pawn move.
bool pos_has_repeated(const Position *pos);

// Report whether POS has a move that draws by repetition — the cuckoo
// no-progress-cycle test. Matches the outcome of pos_is_draw over all legal moves.
bool pos_upcoming_repetition(const Position *pos, int ply);

// Read StateInfo::repetition (maintained by pos_do_move, position.c).
// `StateInfo` carries `int repetition` and `pos_do_move` fills it — see
// PORT_NOTES_board.md. Until then this reads zero and every query above degrades
// to "never repeated": the module is faithful, its input is not yet wired.
static inline int state_repetition(const StateInfo *si) { return si->repetition; }

#endif  // CCFISH_REPETITION_H
