// Own the NNUE network object: the load path, the UCI verify report, and the
// forward-pass entry the evaluation calls.
//
// The net is a runtime input, never embedded. A load walks the same three
// candidate directories upstream does — "<internal>", "", then the root
// directory — and stops at the first that produces a net whose header hash
// matches this build's architecture. A missing file, a truncated file, a version
// mismatch or a hash mismatch all leave the previously loaded net untouched and
// report failure; none of them may abort. Callers must therefore treat a loaded
// net as optional and consult network_verify before evaluating.
//
// Golden: src/nnue/network.cpp, src/nnue/network.h.

#ifndef MCFISH_NETWORK_H
#define MCFISH_NETWORK_H

#include "nnue_architecture.h"

#include "../../board/position.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// Name the default net (EvalFileDefaultName, evaluate.h), a build constant. A net
// bump edits this one line.
#define NETWORK_DEFAULT_EVAL_FILE_NAME "nn-0ee0657fb25e.nnue"

// Report the two halves of an NNUE score before the output scaling the caller
// applies: the PSQT (material) term and the layer-stack (positional) term.
typedef struct {
    int32_t psqt;
    int32_t positional;
} NnueEvalOutput;

// Report every bucket's score for the UCI `eval` breakdown, plus the bucket the
// position actually selects.
typedef struct {
    int32_t psqt[NNUE_LAYER_STACKS];
    int32_t positional[NNUE_LAYER_STACKS];
    size_t correct_bucket;
} NnueTraceOutput;

// Carry the outcome of network_verify. MESSAGE is heap-allocated and may be NULL
// when the allocation failed; release it with network_free_message.
typedef struct {
    bool should_exit;
    char *message;
} NetworkVerifyResult;

// Load the net named by EVALFILE_PATH, falling back to the default name when
// EVALFILE_PATH_LEN is 0. Search "<internal>", the working directory, then
// ROOT_DIRECTORY, concatenating the directory and the name with no separator
// inserted — ROOT_DIRECTORY must already end in one. Silent on failure: call
// network_verify to learn whether a usable net is resident.
void network_load(const char *root_directory,
                  size_t root_directory_len,
                  const char *evalfile_path,
                  size_t evalfile_path_len);

// Report whether the net EVALFILE_PATH names is the one that loaded, and render
// the message upstream prints for either outcome.
NetworkVerifyResult network_verify(const char *evalfile_path, size_t evalfile_path_len);

// Release a NetworkVerifyResult message.
void network_free_message(char *message);

// ---- forward pass ------------------------------------------------------------
//
// ACCUMULATOR_STACK and REFRESH_CACHE are the accumulator port's own types. They
// travel as void * so this header need not duplicate their definitions; pass the
// same pointers straight through. The two nnue_inference_* symbols below are the
// contract the feature-transformer / accumulator port implements.

NnueEvalOutput
nnue_inference_evaluate(const Position *pos, void *accumulator_stack, void *refresh_cache);
NnueTraceOutput
nnue_inference_trace_evaluate(const Position *pos, void *accumulator_stack, void *refresh_cache);

NnueEvalOutput network_evaluate(const Position *pos, void *accumulator_stack, void *refresh_cache);
NnueTraceOutput
network_trace_evaluate(const Position *pos, void *accumulator_stack, void *refresh_cache);

#endif  // MCFISH_NETWORK_H
