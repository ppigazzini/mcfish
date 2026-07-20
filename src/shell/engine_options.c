// Implement the option table and its on-change callbacks. See engine_options.h.

#include "engine_options.h"

#include "../engine/board/movegen.h"  // MAX_MOVES
#include "../engine/eval/evaluate.h"  // eval_nnue_default_file_name
#include "../engine/search/search.h"
#include "../engine/search/tt.h"
#include "engine_nnue.h"
#include "syzygy_option.h"
#include "uci_output.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static OptionsMap Options;
static char MessageBuf[256];

// ---------------------------------------------------------------------------
// On-change callbacks
//
// Each returns bare text for the info listener, or nullptr for silence, matching
// upstream's std::optional<std::string> OnChange.
// ---------------------------------------------------------------------------

static const char *on_hash(const UciOption *o) {
    const size_t mb = (size_t) strtoul(o->current_value, nullptr, 10);
    if (!tt_resize(mb)) {
        snprintf(MessageBuf, sizeof MessageBuf, "failed to allocate %zu MB hash", mb);
        return MessageBuf;
    }
    return nullptr;
}

// Clear the table AND the per-game state, as upstream's Engine::search_clear does
// (engine.cpp:172): the button and `ucinewgame` reach the same function.
static const char *on_clear_hash(const UciOption *o) {
    (void) o;
    tt_clear();
    search_clear();
    return nullptr;
}

static const char *on_debug_log_file(const UciOption *o) {
    uci_output_start_logger(o->current_value);
    return nullptr;
}

// Rebuild the worker set. Upstream reaches ThreadPool::set from the same option and
// rebuilds rather than resizes, because a thread must be created on the NUMA node it
// will run on. Owner: upstream `thread.cpp` (ThreadPool::set).
static const char *on_threads(const UciOption *o) {
    const size_t n = (size_t) strtoul(o->current_value, nullptr, 10);
    if (!search_set_threads(n)) {
        snprintf(MessageBuf, sizeof MessageBuf, "failed to build %zu search thread(s)", n);
        return MessageBuf;
    }
    return nullptr;
}

// Install the NUMA topology the next pool binds under, then re-apply the thread count
// so the policy takes effect now. Owner: upstream `numa.h`, engine.cpp:227.
static const char *on_numa_policy(const UciOption *o) {
    if (!search_set_numa_policy(o->current_value)) {
        snprintf(MessageBuf, sizeof MessageBuf, "NumaPolicy \"%s\" names no usable node",
                 o->current_value);
        return MessageBuf;
    }
    (void) search_set_threads((size_t) options_get_int(&Options, "Threads"));
    return nullptr;
}

// Hand a Syzygy option to the module that owns the four of them and the tablebase
// seams. The table has range-checked the value already. Golden: engine.cpp:125-134.
static const char *on_syzygy(const UciOption *o) {
    (void) syzygy_option_set(o->name, o->current_value);
    return nullptr;
}

static const char *on_eval_file(const UciOption *o) {
    engine_nnue_reload(o->current_value);
    // Drop the search state the previous net produced, as upstream follows every
    // load with threads.clear() (engine.cpp:313). The resident net is announced by
    // engine_nnue_report on the next go/perft/eval, so report nothing here.
    search_clear();
    return nullptr;
}

// ---------------------------------------------------------------------------
// Registration
//
// The order below IS the wire order, upstream's registration order in engine.cpp:69
// onward. tools/handshake.golden diffs it byte for byte, so do not reorder.
// ---------------------------------------------------------------------------

enum { SKILL_LOWEST_ELO = 1320, SKILL_HIGHEST_ELO = 3190 };

// Upstream's max(1024, 4 * get_hardware_concurrency()) (engine.cpp:52), ported as
// the expression so the advertised maximum tracks upstream past 256 cores.
static int max_threads(void) {
    long online = 0;
#if defined(_SC_NPROCESSORS_ONLN)
    online = sysconf(_SC_NPROCESSORS_ONLN);
#endif
    const long scaled = online > 0 ? 4 * online : 0;
    return scaled > 1024 ? (int) scaled : 1024;
}

// Upstream's MaxHashMB = Is64Bit ? 33554432 : 2048 (engine.cpp:51).
static int max_hash_mb(void) { return sizeof(size_t) >= 8 ? 33554432 : 2048; }

void engine_options_register(void) {
    char elo[16];

    options_clear(&Options);

    options_add(&Options, "Debug Log File", OPTION_STRING, "", 0, 0, on_debug_log_file);
    options_add(&Options, "NumaPolicy", OPTION_STRING, "auto", 0, 0, on_numa_policy);
    options_add(&Options, "Threads", OPTION_SPIN, "1", 1, max_threads(), on_threads);
    options_add(&Options, "Hash", OPTION_SPIN, "16", 1, max_hash_mb(), on_hash);
    options_add(&Options, "Clear Hash", OPTION_BUTTON, "", 0, 0, on_clear_hash);
    options_add(&Options, "Ponder", OPTION_CHECK, "false", 0, 0, nullptr);
    options_add(&Options, "MultiPV", OPTION_SPIN, "1", 1, MAX_MOVES, nullptr);
    options_add(&Options, "Skill Level", OPTION_SPIN, "20", 0, 20, nullptr);
    options_add(&Options, "Move Overhead", OPTION_SPIN, "10", 0, 5000, nullptr);
    options_add(&Options, "nodestime", OPTION_SPIN, "0", 0, 10000, nullptr);
    options_add(&Options, "UCI_Chess960", OPTION_CHECK, "false", 0, 0, nullptr);
    options_add(&Options, "UCI_LimitStrength", OPTION_CHECK, "false", 0, 0, nullptr);

    snprintf(elo, sizeof elo, "%d", SKILL_LOWEST_ELO);
    options_add(&Options, "UCI_Elo", OPTION_SPIN, elo, SKILL_LOWEST_ELO, SKILL_HIGHEST_ELO,
                nullptr);

    options_add(&Options, "UCI_ShowWDL", OPTION_CHECK, "false", 0, 0, nullptr);
    options_add(&Options, "SyzygyPath", OPTION_STRING, "", 0, 0, on_syzygy);
    options_add(&Options, "SyzygyProbeDepth", OPTION_SPIN, "1", 1, 100, on_syzygy);
    options_add(&Options, "Syzygy50MoveRule", OPTION_CHECK, "true", 0, 0, on_syzygy);
    options_add(&Options, "SyzygyProbeLimit", OPTION_SPIN, "7", 0, 7, on_syzygy);
    options_add(&Options, "EvalFile", OPTION_STRING, eval_nnue_default_file_name(), 0, 0,
                on_eval_file);
}

void engine_options_set_info(void (*emit_info)(const char *message)) {
    options_set_info(&Options, emit_info);
}

OptionsMap *engine_options_map(void) { return &Options; }

int engine_options_get_int(const char *name) { return options_get_int(&Options, name); }

const char *engine_options_get_string(const char *name) {
    return options_get_string(&Options, name);
}

int engine_options_apply(const char *args, char name[OPTION_NAME_MAX]) {
    return options_setoption(&Options, args, name);
}

void engine_options_render(char *buf, size_t buf_len) {
    (void) buf_len;
    options_render(&Options, buf, buf_len);
}
