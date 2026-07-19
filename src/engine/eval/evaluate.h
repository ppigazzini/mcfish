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
// See nnue/PORT_NOTES_accumulator.md.
//
// Golden: Stockfish/src/evaluate.cpp:41 (Eval::evaluate), :75 (Eval::trace).
// Port source: zfish `engine/eval/evaluate.zig`, `engine/search/search_acc.zig`.

#ifndef CCFISH_EVALUATE_H
#define CCFISH_EVALUATE_H

#include "../board/position.h"
#include "../board/types.h"

// Return the static score of POS in centipawns, positive when the side to move
// stands better. Never returns a mate-range value.
//
// OPTIMISM is the search's per-colour aspiration bias, read for the side to move.
// It scales against the network's own complexity and against material, so it is
// part of the evaluation rather than a correction applied afterwards — pass the
// search's value from inside a search. `evaluate` is the standalone form and
// passes zero, which is upstream's own value at `eval` and in the trace.
Value evaluate_with_optimism(const Position *pos, int optimism);
Value evaluate(const Position *pos);

// Render the evaluation breakdown for the UCI `eval` command into BUF.
void evaluate_trace(const Position *pos, char *buf, int buf_len);

// ---- NNUE runtime -----------------------------------------------------------

// Allocate the accumulator arenas and build the feature tables. Run this once at
// startup, in the same phase as bitboards_init and attacks_init: the feature
// tables are zero, not garbage, before it, so the failure mode is a silent
// all-zero feature set. Return false when an arena allocation failed, which
// leaves the evaluation on the classical fallback.
bool eval_nnue_init(void);

// Release the arenas and the loaded weights.
void eval_nnue_shutdown(void);

// Load the net EVALFILE_PATH names, searching "<internal>", the working
// directory, then ROOT_DIRECTORY — which must already end in a separator. Pass
// nullptr or "" for either to skip it, and nullptr for EVALFILE_PATH to take the
// build's default name. Return true when a usable net is resident afterwards; a
// missing, truncated or mismatched file leaves the classical fallback in place
// rather than terminating, which is where ccfish deliberately parts from
// upstream's `exit(EXIT_FAILURE)` in Network::verify.
bool eval_nnue_load(const char *root_directory, const char *evalfile_path);

// Report whether `evaluate` is currently running the NNUE forward pass.
bool eval_nnue_available(void);

// Return the one-line status upstream prints through `info string` before each
// go / perft / eval: the resident net's identity, or why the classical fallback
// is in use. Valid until the next load.
const char *eval_nnue_status(void);

// Name the net this build expects, so the shell can default the EvalFile option
// without reaching into the NNUE headers.
const char *eval_nnue_default_file_name(void);

// ---- accumulator bracketing --------------------------------------------------

// Drop back to a single uncomputed root slot. Call at the root of every search
// and before any standalone evaluate, so the first evaluation refreshes from the
// board rather than from a stale ply's diff.
// Re-seed the NNUE refresh cache from the resident net's feature-transformer
// biases. Upstream does this in Worker::clear (search.cpp:698), reached from
// `ucinewgame` -- not only on net load. A cache still holding entries seeded from
// a PREVIOUS net makes the incremental refresh path produce wrong accumulator
// values, and a forced full refresh cannot expose it because that path bypasses
// the cache entirely.
void eval_nnue_clear_refresh_cache(void);

void eval_acc_reset(void);

// Push one ply and hand back the two records pos_do_move must fill. The pointers
// address the accumulator's own arena, so the make-move writes its delta straight
// into the slot and nothing is copied. Both are always non-NULL: without a net
// they address private scratch, so a call site needs no branch.
void eval_acc_push(DirtyPiece **dp, DirtyThreats **dts);

// Drop the top ply. Pair with eval_acc_push around pos_undo_move.
void eval_acc_pop(void);

#endif  // CCFISH_EVALUATE_H
