// Construct the root move list and its Syzygy root ranking.
//
// Build the `go`-path root moves: rank the legal moves (or the caller's
// searchmoves) by DTZ, falling back to WDL, when tablebases are loaded, then hand
// back the RootMove array the search binds to.
//
// The invariant that keeps a default build's node count untouched: with no
// SyzygyPath the probe seam reports zero cardinality, no ranking runs, every
// tb_rank and tb_score stays 0, and the returned list is the move list in
// generator order. Everything tablebase-shaped here is behind that gate.
//
// Pure over position / movegen plus the injected tb_source and option_source
// seams — no thread or worker dependency, so the allocation-failure paths are
// testable in isolation.
//
// Golden: `Stockfish/src/syzygy/tbprobe.cpp: rank_root_moves` and
// `Stockfish/src/search.cpp: Search::Worker::start_searching`.

#ifndef MCFISH_ROOT_MOVE_BUILD_H
#define MCFISH_ROOT_MOVE_BUILD_H

#include "search_types.h"

#include "../board/position.h"
#include "../board/types.h"

#include <stddef.h>

typedef struct {
    RootMove *moves;
    size_t count;
    TbConfig tb_config;
} RootMoveList;

// Build the ranked root move list for POS from MOVES. ROOT_FEN must be POS's own
// FEN — the ranking replays each move from it on a scratch board. Return false on
// allocation failure, leaving OUT untouched.
[[nodiscard]] bool root_moves_build(const Position *pos,
                                    const char *root_fen,
                                    bool chess960,
                                    const Move *moves,
                                    size_t count,
                                    RootMoveList *out);

void root_moves_free(RootMoveList *list);

// Carry one move through the Syzygy ranking. This is upstream's `RootMove` seen
// only through the three fields `rank_root_moves` touches, so a caller ranking a
// list that is not the root move list — the PV corrector — needs no RootMove.
typedef struct {
    Move raw_move;
    int32_t tb_rank;
    int32_t tb_score;
} RankedRootMove;

// Report whether the ranking has spent its time budget. Nullable: nullptr reads
// as "never abort".
typedef bool (*TbTimeAbort)(void *ctx);

// The two flags the root ranking carries, as TYPES rather than as two adjacent
// bools. They were `bool rule50, bool rank_dtz` in argument position, which is the
// hole docs/09-type-design.md names as the largest one here -- "two arguments of the
// same type, transposed" -- and both inversions are silent AND meaningful: rule50
// changes the verdict a table gives, rank_dtz changes whether DTZ ranking happens at
// all. Callers passed bare `true`/`false` literals at three sites.
//
// Two distinct `enum : uint8_t`, which is this tree's newtype tier for a tag, so the
// transposition is a hard error under the -Werror=enum-conversion promotion
// build.sh probes for -- and `./build.sh type-check` is what keeps that promotion
// from quietly going missing.
//
// The values are 0/1 so the enum is still usable where upstream's bool was, and the
// ranking is a ROOT operation -- once per `go`, never per node -- so the cost rule's
// concern (a scalar threaded through a hot function's control flow) does not apply.
typedef enum : uint8_t { RULE50_IGNORE = 0, RULE50_APPLY = 1 } Rule50;
typedef enum : uint8_t { RANK_DTZ_NO = 0, RANK_DTZ_YES = 1 } RankDtz;

// Rank MOVES for POS by DTZ, falling back to WDL, and return the resolved config.
//
// POS is walked with do/undo and restored exactly on every return, including the
// probe-failure and time-abort paths — the caller keeps using it. ST is the one
// StateInfo slot each trial move pushes onto POS's chain; it must outlive the
// call and must not alias a record already in that chain.
//
// CNT50 and HAS_REPEATED describe POS. They are parameters rather than reads of
// POS because the root ranking replays its moves on a FEN-built scratch board,
// whose chain carries no game history, while the counters that must feed the DTZ
// bound are the real root's.
//
// RANK_DTZ is upstream's `rankDTZ`: RANK_DTZ_NO ranks all certain wins equally,
// RANK_DTZ_YES orders them by exact DTZ. It is forced YES where DTZ ranks as DTM.
//
// Upstream `Stockfish/src/syzygy/tbprobe.cpp:1780` (Tablebases::rank_root_moves).
TbConfig tb_rank_moves(Position *pos,
                       StateInfo *st,
                       RankedRootMove *ranked,
                       size_t count,
                       RankDtz rank_dtz,
                       int32_t cnt50,
                       bool has_repeated,
                       TbTimeAbort time_abort,
                       void *abort_ctx);

// Sort descending by tb_rank, stably: equal ranks keep their incoming order,
// which is the tie-break upstream relies on when every winning move ties at the
// DTZ ceiling.
void tb_stable_sort_by_rank(RankedRootMove *ranked, size_t count);

#endif  // MCFISH_ROOT_MOVE_BUILD_H
