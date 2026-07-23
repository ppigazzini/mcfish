#include "syzygy_option.h"

#include "../engine/search/option_source.h"
#include "../engine/search/tb_source.h"
#include "../platform/tablebase.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// Hold the option values. The defaults are upstream's
// (`Stockfish/src/engine.cpp:125-134`): an empty path, which makes
// `tablebase_max_cardinality` 0 and every probe report unavailable, so a build
// that never sets SyzygyPath — `bench` included — takes no tablebase path at all.
static char Path[4096];
static int ProbeDepth = 1;
static int ProbeLimit = 7;
static bool Rule50 = true;

static int read_probe_depth(void) { return ProbeDepth; }
static int read_probe_limit(void) { return ProbeLimit; }
static bool read_rule50(void) { return Rule50; }

void syzygy_option_install(void) {
    OptionSyzygyProbeDepth = read_probe_depth;
    OptionSyzygyProbeLimit = read_probe_limit;
    OptionSyzygy50MoveRule = read_rule50;

    TbMaxCardinality = tablebase_max_cardinality;
    TbProbeFen = tablebase_probe_fen;
    TbProbeWdlPos = tablebase_probe_wdl_pos;
}

// Rescan the path and report what was found, as upstream does at the end of
// `Tablebases::init` (syzygy/tbprobe.cpp:1558). Print only for a non-empty path:
// upstream returns before the report when the path list is empty
// (tbprobe.cpp:1420), so an engine with no tablebases stays silent.
static void load_path(void) {
    tablebase_init(Path, strlen(Path));
    if (Path[0] == '\0')
        return;
    printf("info string Found %zu WDL and %zu DTZ tablebase files (up to %zu-man).\n",
           tablebase_found_wdl(), tablebase_found_dtz(), tablebase_max_cardinality());
    fflush(stdout);
}

// Accept a spin value only when the whole string parses and lands in [min, max],
// and otherwise leave the option unchanged — upstream's `value_in_range`
// (ucioption.cpp:148) plus the guard at :168, which returns the option untouched.
// Clamping instead would silently turn a typo into a different search.
static void set_spin(int *slot, const char *value, long min, long max) {
    char *end = nullptr;
    const long v = strtol(value, &end, 10);
    if (value[0] == '\0' || *end != '\0' || v < min || v > max)
        return;
    *slot = (int) v;
}

void syzygy_option_reinit(void) { load_path(); }

bool syzygy_option_set(const char *name, const char *value) {
    // A `setoption` line with no ` value ` field parses as the empty value, which
    // is what upstream's token reader also yields.
    const char *v = value ? value : "";

    if (strcmp(name, "SyzygyPath") == 0) {
        // `<empty>` is the wire spelling of the empty string for a string option,
        // both on the way out and on the way back in (ucioption.cpp:183, :211).
        snprintf(Path, sizeof Path, "%s", strcmp(v, "<empty>") == 0 ? "" : v);
        load_path();
        return true;
    }
    if (strcmp(name, "SyzygyProbeDepth") == 0) {
        set_spin(&ProbeDepth, v, 1, 100);
        return true;
    }
    if (strcmp(name, "SyzygyProbeLimit") == 0) {
        set_spin(&ProbeLimit, v, 0, 7);
        return true;
    }
    if (strcmp(name, "Syzygy50MoveRule") == 0) {
        if (strcmp(v, "true") == 0 || strcmp(v, "false") == 0)
            Rule50 = strcmp(v, "true") == 0;
        return true;
    }
    return false;
}

void syzygy_option_print(void) {
    printf("option name SyzygyPath type string default <empty>\n");
    printf("option name SyzygyProbeDepth type spin default 1 min 1 max 100\n");
    printf("option name Syzygy50MoveRule type check default true\n");
    printf("option name SyzygyProbeLimit type spin default 7 min 0 max 7\n");
}
