#include "timeman.h"

#include "../../platform/clock.h"

#include <math.h>

// Mirror std::min / std::max exactly rather than calling fmin / fmax: the two
// disagree on NaN, and the budget formulas feed on log10 of a clock that can be
// zero. Keep the C++ tie-breaking (return the first argument on equality).
static double d_min(double a, double b) { return b < a ? b : a; }
static double d_max(double a, double b) { return a < b ? b : a; }

static int64_t i_min(int64_t a, int64_t b) { return b < a ? b : a; }
static int64_t i_max(int64_t a, int64_t b) { return a < b ? b : a; }

// Compute the bounds of time allowed for the current game ply. Support:
//      1) x basetime (+ z increment)
//      2) x moves in y seconds (+ z increment)
// Port of upstream TimeManagement::init (timeman.cpp:46).
TimemanOutput timeman_compute(TimemanInput input) {
    TimemanOutput output = {
        .time = input.time,
        .inc = input.inc,
        .start_time = input.start_time,
        .npmsec = input.npmsec,
        .available_nodes = input.available_nodes,
        .optimum_time = input.current_optimum_time,
        .maximum_time = input.current_maximum_time,
        .original_time_adjust = input.original_time_adjust,
        .use_nodes_time = input.npmsec != 0,
    };

    // With no time there is no budget to fully initialize. start_time is used by
    // movetime and use_nodes_time is used in the elapsed calls.
    if (input.time == 0)
        return output;

    TimePoint move_overhead = input.move_overhead;

    // In `nodes as time` mode, convert from time to nodes and use the resulting
    // values in the time management formulas.
    // WARNING: to avoid time losses, the given npmsec (nodes per millisecond)
    // must be much lower than the real engine speed.
    if (output.use_nodes_time) {
        if (output.available_nodes == -1)                        // Only once at game start
            output.available_nodes = input.npmsec * input.time;  // Time is in msec

        output.time = output.available_nodes;
        output.inc *= input.npmsec;
        move_overhead *= input.npmsec;
    }

    // These numbers are used where multiplications, divisions, or comparisons
    // with constants are involved. The division truncates toward zero.
    const int64_t scale_factor = output.use_nodes_time ? input.npmsec : 1;
    const TimePoint scaled_time = output.time / scale_factor;

    // Maximum move horizon
    int64_t mtg = input.movestogo != 0 ? i_min((int64_t) input.movestogo, 50) : 50;

    // If less than one second, gradually reduce mtg
    if (scaled_time < 1000)
        mtg = (int64_t) ((double) scaled_time * 0.05);

    // Make sure time_left is > 0 since we may use it as a divisor
    const TimePoint time_left =
      i_max((int64_t) 1, output.time + output.inc * (mtg - 1) - move_overhead * (2 + mtg));

    double opt_scale;
    double max_scale;
    double original_time_adjust = output.original_time_adjust;

    // x basetime (+ z increment)
    // If there is a healthy increment, time_left can exceed the actual available
    // game time for the current move, so also cap to a percentage of available
    // game time.
    if (input.movestogo == 0) {
        // Extra time according to time_left
        if (original_time_adjust < 0)
            original_time_adjust = 0.3272 * log10((double) time_left) - 0.4141;

        // Calculate time constants based on current time left.
        const double log_time_in_sec = log10((double) scaled_time / 1000.0);
        const double opt_constant = d_min(0.0029869 + 0.00033554 * log_time_in_sec, 0.004905);
        const double max_constant = d_max(3.3744 + 3.0608 * log_time_in_sec, 3.1441);

        opt_scale = d_min(0.012112 + pow((double) input.ply + 3.22713, 0.46866) * opt_constant,
                          0.19404 * (double) output.time / (double) time_left)
                  * original_time_adjust;

        max_scale = d_min(6.873, max_constant + (double) input.ply / 12.352);
    }

    // x moves in y seconds (+ z increment)
    else {
        opt_scale = d_min((0.88 + (double) input.ply / 116.4) / (double) mtg,
                          0.88 * (double) output.time / (double) time_left);
        max_scale = 1.3 + 0.11 * (double) mtg;
    }

    // Limit the maximum possible time for this move. Both casts truncate toward
    // zero, and both arguments are positive here.
    output.optimum_time = (TimePoint) d_max(1.0, opt_scale * (double) time_left);
    output.maximum_time = (TimePoint) d_max(
      (double) output.optimum_time, d_min(0.8097 * (double) output.time - (double) move_overhead,
                                          max_scale * (double) output.optimum_time));

    if (input.ponder)
        output.optimum_time += output.optimum_time / 4;

    output.original_time_adjust = original_time_adjust;
    return output;
}

TimemanLimits timeman_init(TimeManagement *tm,
                           const SearchLimits *limits,
                           Color us,
                           int ply,
                           TimemanOptions opts,
                           TimePoint start_time,
                           double *original_time_adjust) {
    const TimemanInput input = {
        .time = (TimePoint) limits->time_ms[us],
        .inc = (TimePoint) limits->inc_ms[us],
        .start_time = start_time,
        .npmsec = opts.npmsec,
        .move_overhead = opts.move_overhead,
        .available_nodes = tm->available_nodes,
        .current_optimum_time = tm->optimum_time,
        .current_maximum_time = tm->maximum_time,
        .movestogo = (int32_t) limits->moves_to_go,
        .ply = (int32_t) ply,
        .original_time_adjust = *original_time_adjust,
        .ponder = opts.ponder,
    };

    const TimemanOutput out = timeman_compute(input);

    tm->start_time = out.start_time;
    tm->optimum_time = out.optimum_time;
    tm->maximum_time = out.maximum_time;
    tm->available_nodes = out.available_nodes;
    tm->use_nodes_time = out.use_nodes_time;
    *original_time_adjust = out.original_time_adjust;

    return (TimemanLimits) { .time = out.time, .inc = out.inc, .npmsec = out.npmsec };
}

void timeman_clear(TimeManagement *tm) {
    tm->available_nodes = -1;  // When in `nodes as time` mode
}

void timeman_advance_nodes_time(TimeManagement *tm, int64_t nodes) {
    tm->available_nodes = i_max((int64_t) 0, tm->available_nodes - nodes);
}

TimePoint timeman_optimum(const TimeManagement *tm) { return tm->optimum_time; }
TimePoint timeman_maximum(const TimeManagement *tm) { return tm->maximum_time; }

TimePoint timeman_elapsed_time(const TimeManagement *tm) {
    return (TimePoint) now_ms() - tm->start_time;
}

TimePoint timeman_elapsed(const TimeManagement *tm, uint64_t nodes_searched) {
    return tm->use_nodes_time ? (TimePoint) nodes_searched : timeman_elapsed_time(tm);
}
