// Own the think-time budget for one `go`: turn the clock on the UCI command into
// an optimum and a maximum time for this move, and answer how much of that budget
// has been spent.
//
// The budget arithmetic is a pure function of its input record — timeman_compute
// reads no clock, no option, and no position. The stateful wrapper timeman_init
// only stores its result and hands back the possibly-converted clock, so the
// formulas stay testable without a running search.
//
// Nothing here reads the clock per node. timeman_elapsed_time is the single clock
// read, and the caller decides how often to make it; in `nodes as time` mode
// timeman_elapsed reads no clock at all. That is what keeps a fixed-depth run
// reproducible regardless of machine speed, so callers must keep the elapsed
// queries on a node-count checkpoint rather than in the recursion.
//
// available_nodes is -1 until the first init of a `nodes as time` game and is
// carried across moves; timeman_clear resets it at the start of a new game.

#ifndef MCFISH_TIMEMAN_H
#define MCFISH_TIMEMAN_H

#include "search.h"

#include "../board/types.h"

#include <stdint.h>

// Hold a point in time, or a duration, in milliseconds. In `nodes as time` mode
// the same type carries a node count instead — the conversion is what that mode
// is. The unit is milliseconds, matching upstream's TimePoint.
typedef int64_t TimePoint;

typedef struct {
    TimePoint start_time;
    TimePoint optimum_time;
    TimePoint maximum_time;
    int64_t available_nodes;  // -1 when no `nodes as time` game has started
    bool use_nodes_time;      // true while in `nodes as time` mode
} TimeManagement;

// Carry the UCI options the budget depends on. The caller reads them from the
// options map; this module never does.
typedef struct {
    TimePoint npmsec;         // "nodestime": nodes per millisecond, 0 to disable
    TimePoint move_overhead;  // "Move Overhead", milliseconds
    bool ponder;              // "Ponder"
} TimemanOptions;

// Report the clock as it stands after a `nodes as time` conversion. Equal to the
// caller's own limits when that mode is off.
typedef struct {
    TimePoint time;
    TimePoint inc;
    TimePoint npmsec;
} TimemanLimits;

// Mirror the pure core of Stockfish's TimeManagement::init.
typedef struct {
    TimePoint time;
    TimePoint inc;
    TimePoint start_time;
    TimePoint npmsec;
    TimePoint move_overhead;
    int64_t available_nodes;
    TimePoint current_optimum_time;
    TimePoint current_maximum_time;
    int32_t movestogo;
    int32_t ply;
    double original_time_adjust;  // negative until the first move of the game
    bool ponder;
} TimemanInput;

typedef struct {
    TimePoint time;
    TimePoint inc;
    TimePoint start_time;
    TimePoint npmsec;
    int64_t available_nodes;
    TimePoint optimum_time;
    TimePoint maximum_time;
    double original_time_adjust;
    bool use_nodes_time;
} TimemanOutput;

// Compute the budget. With a zero clock, pass the current optimum and maximum
// straight through: only start_time and use_nodes_time are established, which is
// all a movetime or infinite search reads.
TimemanOutput timeman_compute(TimemanInput input);

// Set up TM for the search about to start and return the effective clock. Read
// the side-to-move clock out of LIMITS; ORIGINAL_TIME_ADJUST is the caller's
// per-game state and is updated in place on the first move that fixes it.
TimemanLimits timeman_init(TimeManagement *tm,
                           const SearchLimits *limits,
                           Color us,
                           int ply,
                           TimemanOptions opts,
                           TimePoint start_time,
                           double *original_time_adjust);

// Reset the carried-over node budget. Call at the start of a new game.
void timeman_clear(TimeManagement *tm);

// Spend NODES from the `nodes as time` budget, never below zero. Callers pass the
// nodes searched less this move's increment, matching upstream.
void timeman_advance_nodes_time(TimeManagement *tm, int64_t nodes);

TimePoint timeman_optimum(const TimeManagement *tm);
TimePoint timeman_maximum(const TimeManagement *tm);

// Return the milliseconds since the search started. This is the only clock read
// in the module.
TimePoint timeman_elapsed_time(const TimeManagement *tm);

// Return elapsed budget in whichever unit this search is spending: nodes in
// `nodes as time` mode, milliseconds otherwise. NODES_SEARCHED is evaluated by
// the caller either way, where upstream passes a closure to avoid it — a cost,
// not a behaviour, difference.
TimePoint timeman_elapsed(const TimeManagement *tm, uint64_t nodes_searched);

#endif  // MCFISH_TIMEMAN_H
