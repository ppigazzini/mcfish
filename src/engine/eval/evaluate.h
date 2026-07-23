// Score a position from the side-to-move's point of view, and own the NNUE
// runtime state the score needs.
//
// THE INVARIANT IS THAT `evaluate` RETURNS THE NNUE SCORE WHENEVER A NET IS
// RESIDENT. The classical material + piece-square term below it is a fallback for
// a netless run only — it is scaffolding to be deleted, not a second evaluation to
// tune (docs/03-engine-eval.md, AGENTS.md).
//
// The NNUE accumulator is incremental, so this module also owns the per-ply stack
// and the refresh cache, and the search MUST bracket every make/unmake with
// `eval_acc_push` / `eval_acc_pop` and reset at each root. A missing bracket does
// not crash: it silently evaluates a different position than the board holds.
//
// Golden: Stockfish/src/evaluate.cpp:41 (Eval::evaluate), :75 (Eval::trace).

#ifndef MCFISH_EVALUATE_H
#define MCFISH_EVALUATE_H

#include "../board/position.h"
#include "../board/types.h"

// Own the two NNUE arenas ONE search worker evaluates through: the incremental
// accumulator stack and the refresh cache. The accumulator is a running diff of the
// board a single recursion is walking, so two workers sharing one arena evaluate a
// position neither of them is on — this is per-worker state, not a cache.
//
// Opaque: the arena's size is a runtime function of the resident net's architecture.
typedef struct EvalArena EvalArena;

// Allocate one arena, or null. The refresh cache is left unseeded; call
// eval_arena_clear_refresh_cache once a net is resident.
EvalArena *eval_arena_create(void);
void eval_arena_destroy(EvalArena *arena);

// Return the process-wide arena, allocating it on first use. It is what a caller with
// no worker of its own evaluates through: the `eval` command, the trace, and any direct
// `evaluate`. Null when the allocation failed, which leaves the classical fallback.
EvalArena *eval_default_arena(void);

// Return the static score of POS in centipawns, positive when the side to move
// stands better. Never returns a mate-range value.
//
// OPTIMISM is the search's per-colour aspiration bias, read for the side to move.
// It scales against the network's own complexity and against material, so it is
// part of the evaluation rather than a correction applied afterwards — pass the
// search's value from inside a search. `evaluate` is the standalone form: it passes
// zero, which is upstream's own value at `eval` and in the trace, and evaluates
// through the default arena.
Value evaluate_with_optimism(EvalArena *arena, const Position *pos, int optimism);
Value evaluate(const Position *pos);

// The no-fallback form: evaluate through ARENA, which must be non-null, with a
// network resident. The search calls this behind its own per-go readiness flag
// (SearchCtx.eval_nnue_ready), so the per-node path re-tests neither the global
// load state nor the arena pointer; every other caller goes through
// evaluate_with_optimism, which still degrades to the classical placeholder.
Value evaluate_nnue_with_optimism(EvalArena *arena, const Position *pos, int optimism);

// Render the evaluation breakdown for the UCI `eval` command into BUF.
void evaluate_trace(const Position *pos, char *buf, int buf_len);

// ---- NNUE runtime -----------------------------------------------------------

// Build the feature tables and the default arena. Run this once at startup, in the
// same phase as bitboards_init and attacks_init: the feature tables are zero, not
// garbage, before it, so the failure mode is a silent all-zero feature set. Return
// false when the default arena could not be allocated, which leaves the evaluation on
// the classical fallback.
bool eval_nnue_init(void);

// Release the arenas and the loaded weights.
void eval_nnue_shutdown(void);

// Load the net EVALFILE_PATH names, searching "<internal>", the working
// directory, then ROOT_DIRECTORY — which must already end in a separator. Pass
// nullptr or "" for either to skip it, and nullptr for EVALFILE_PATH to take the
// build's default name. Return true when a usable net is resident afterwards; a
// missing, truncated or mismatched file leaves the classical fallback in place
// rather than terminating, which is where mcfish deliberately parts from
// upstream's `exit(EXIT_FAILURE)` in Network::verify.
bool eval_nnue_load(const char *root_directory, const char *evalfile_path);

// Report whether `evaluate` is currently running the NNUE forward pass.
bool eval_nnue_available(void);

// Count the successful net loads this process has seen. A worker records the value its
// refresh cache was seeded at and re-seeds when the two differ: a cache still holding
// entries seeded from a PREVIOUS net makes the incremental refresh path produce wrong
// accumulator values, and a forced full refresh cannot expose it because that path
// bypasses the cache entirely. This is upstream's `ensure_network_replicated`
// (search.h:328) with the replication reduced to the one net mcfish keeps.
//
// Starts at 0, which no seeded worker can hold, so a worker built before the first load
// always re-seeds.
uint64_t eval_network_generation(void);

// Return the one-line status upstream prints through `info string` before each
// go / perft / eval: the resident net's identity, or why the classical fallback
// is in use. Valid until the next load.
const char *eval_nnue_status(void);

// Name the net this build expects, so the shell can default the EvalFile option
// without reaching into the NNUE headers.
const char *eval_nnue_default_file_name(void);

// ---- accumulator bracketing --------------------------------------------------

// Re-seed ARENA's refresh cache from the resident net's feature-transformer
// biases. Upstream does this in Worker::clear (search.cpp:699), reached from
// `ucinewgame` -- not only on net load. A cache still holding entries seeded from
// a PREVIOUS net makes the incremental refresh path produce wrong accumulator
// values, and a forced full refresh cannot expose it because that path bypasses
// the cache entirely.
void eval_arena_clear_refresh_cache(EvalArena *arena);

// Drop ARENA back to a single uncomputed root slot. Call at the root of every search
// and before any standalone evaluate, so the first evaluation refreshes from the
// board rather than from a stale ply's diff.
void eval_acc_reset(EvalArena *arena);

// Push one ply and hand back the two records pos_do_move must fill. The pointers
// address ARENA's own storage, so the make-move writes its delta straight into the
// slot and nothing is copied. Both are always non-NULL: a ply the stack cannot hold
// lands in the arena's private scratch, so a call site needs no branch.
//
// ARENA must not be null. A search that could not get one must not start: without a
// place to absorb the diff there is no evaluation to fall back TO, and a shared
// scratch record would be exactly the cross-worker aliasing the arena exists to end.
void eval_acc_push(EvalArena *arena, DirtyPiece **dp, DirtyThreats **dts);

// Drop the top ply. Pair with eval_acc_push around pos_undo_move.
void eval_acc_pop(EvalArena *arena);

#endif  // MCFISH_EVALUATE_H
