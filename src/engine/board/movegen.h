// Generate moves into a caller-owned buffer.
//
// The generators are PSEUDO-legal except `generate_legal`: they may leave our own
// king in check, and the caller must filter with pos_legal. Only `generate_legal`
// is safe to iterate without that filter.

#ifndef MCFISH_MOVEGEN_H
#define MCFISH_MOVEGEN_H

#include "position.h"
#include "types.h"

typedef struct {
    Move move;
    int value;  // move-ordering score, filled by the picker, not the generator
} ExtMove;

typedef enum {
    GEN_CAPTURES,  // captures and queen promotions
    GEN_QUIETS,    // everything else
    GEN_NON_EVASIONS,
    GEN_EVASIONS,  // legal-target subset when in check; still pseudo-legal
} GenType;

// Append the generated moves at LIST and return the new end pointer. LIST must
// have room for MAX_MOVES entries.
ExtMove *generate(const Position *pos, ExtMove *list, GenType type);

// Type-literal entries, upstream's explicit generate<Type> instantiations: each
// body is specialized per stage and per side to move, so the per-stage tests and
// the direction/piece dispatches are folded at compile time. `generate` above is
// the runtime-typed dispatcher over these.
ExtMove *generate_captures(const Position *pos, ExtMove *list);
ExtMove *generate_quiets(const Position *pos, ExtMove *list);
ExtMove *generate_evasions(const Position *pos, ExtMove *list);
ExtMove *generate_non_evasions(const Position *pos, ExtMove *list);

// Append only the moves that pass pos_legal. This is the perft and root generator.
ExtMove *generate_legal(const Position *pos, ExtMove *list);

#endif  // MCFISH_MOVEGEN_H
