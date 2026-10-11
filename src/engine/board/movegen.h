// Generate moves into a caller-owned buffer.
//
// Every generator emits LEGAL moves only, as upstream's do since 400c23f97: the
// pins, the threat set and the castling path are all resolved here, so nothing a
// generator returns needs pos_legal.
//
// Upstream: movegen.h:31 (GenType), movegen.cpp, and perft.h (the perft walk).

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
    GEN_EVASIONS,  // every move when in check
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

// Append every legal move: the evasions in check, else the captures and then the
// quiets (upstream generate<ALL>). This is the perft and root generator.
ExtMove *generate_legal(const Position *pos, ExtMove *list);

#endif  // MCFISH_MOVEGEN_H
