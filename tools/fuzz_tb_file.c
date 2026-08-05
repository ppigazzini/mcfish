// Fuzz a whole Syzygy table file through the path the engine actually takes.
//
// tools/fuzz_tb_parse.c reaches decode_set_sizes and decode_pairs directly, which
// is what makes it fast, but it can only reach them by REIMPLEMENTING the carve
// `registry.c set` performs -- and a reimplementation is a claim about the code
// under test, not a test of it. Everything between the file and the decoder is
// invisible to it: `set` itself, the piece loop, `set_groups`, `set_dtz_map`,
// `map_file`'s size and magic checks, and the lazy publication in
// registry_map_wdl. This driver covers exactly that gap, at the cost of a real
// file per iteration.
//
// One iteration is what a user does: write bytes to a `.rtbw`/`.rtbz`, point
// SyzygyPath at the directory holding them, probe a position of that material.
// The engine's own entry points do the rest -- tablebase_init scans and
// registers, the first probe maps and parses, and the probe walks the decoder
// through wdl.c's index arithmetic and the DTZ remap. Nothing here is a stand-in
// for engine code.
//
// An input is `[stem][wdl_len:3][wdl body][dtz body]`, and the driver supplies
// the magic and the length padding around each body: an iteration then reaches
// `set` instead of spending its budget rediscovering a 4-byte constant and a
// length congruence, both of which already have goldens behind them
// (`./build.sh tb`) where the parse below them has none.
//
// The explicit `wdl_len` is what makes the corpus SEEDABLE. Every 3-man table
// ships at a length that already satisfies the padding rule, so an entry built
// as `[stem][len][.rtbw minus magic][.rtbz minus magic]` reconstructs both files
// byte for byte, and the fuzzer then mutates around a table that parses. That is
// where a parser dies -- the same reason tools/uci_fuzz.py weights toward
// almost-valid commands rather than noise. `./build.sh fuzz-tb` builds those
// seeds when resources/syzygy/ holds the set, and says so when it does not.
//
// Determinism is load-bearing for a fuzzer -- the same input must do the same
// thing -- so each iteration removes every stem's files before writing the pair
// it chose, and releases the registry at the end rather than leaving a
// generation for the next input to inherit.
//
// An iteration then RANKS the root, because the probe is not the only consumer
// of a score the file decided. `tablebase_probe_fen` ends at the value; the root
// ranking indexes two five-entry tables with it (`WdlToRank[wdl + 2]`,
// `WdlToValue[score_wdl + 2]`), and that surface belonged to no tablebase lane
// while this driver stopped at the probe. Reach it through `root_moves_build`
// for the same reason the probe is reached through `tablebase_init` rather than
// through `set`: it is the engine's own entry point, and a corrupt file reaches
// it on every `go`. Extend this half whenever a new consumer reads an answer the
// file decided.
//
// Golden: none. This is test infrastructure, not a port.

#include "../src/engine/board/attacks.h"
#include "../src/engine/board/bitboard.h"
#include "../src/engine/board/movegen.h"
#include "../src/engine/board/position.h"
#include "../src/engine/board/threats.h"
#include "../src/engine/search/option_source.h"
#include "../src/engine/search/root_move_build.h"
#include "../src/engine/search/tb_source.h"
#include "../src/platform/syzygy/registry.h"
#include "../src/platform/syzygy/wdl.h"
#include "../src/platform/tablebase.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

// The libFuzzer runtime calls these two by name; no mcfish header declares them
// because no mcfish translation unit calls them. Declare them here, once, so
// -Wmissing-prototypes has the header-in-the-same-file every other symbol in
// this tree gets (docs/08-idiomatic-c.md's warning table).
int LLVMFuzzerInitialize(int *argc, char ***argv);
int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size);

// The magic each extension carries, which map_file checks before parsing.
static const uint8_t WdlMagic[4] = { 0x71, 0xE8, 0x23, 0x5D };
static const uint8_t DtzMagic[4] = { 0xD7, 0x66, 0x0C, 0xA5 };

// Three material configurations, chosen for the branches they reach in `set`
// rather than for variety: no pawns at all (one file, max_file 0), pawns on one
// side (the four-file loop), and pawns on BOTH sides (`pp`, which adds the
// second order nibble and the leading-pawn group). A fourth stem would add
// iterations, not code.
typedef struct {
    const char *stem;
    const char *fen;
} Config;

static const Config Configs[] = {
    { "KQvK", "8/8/8/8/8/2K5/3Q4/7k w - - 0 1" },
    { "KPvK", "8/8/8/8/8/2K5/3P4/7k w - - 0 1" },
    { "KPvKP", "8/8/8/3p4/8/2K5/3P4/7k w - - 0 1" },
};

enum { CONFIG_COUNT = (int) (sizeof Configs / sizeof Configs[0]) };

static char TbDir[64];

// Count what each iteration REACHED, not that it ran.
//
// libFuzzer's executed-input total is the only floor this lane had, and it cannot
// tell a decoder exercised thousands of times from a `set` that refused every file:
// the driver supplies the magic, so an input that dies on a length check still
// counts as an execution. Three counts split that -- rounds, the files `set` parsed,
// and the probes that answered -- and each gets its own floor in build.sh.
//
// ../rfish dd0df54 records the same defect in its own sweep, from ../zfish 741f8ffc,
// which found its floor "met almost entirely by answers carrying scores no file can
// hold". PARSED is read from the registry rather than inferred from the probe,
// because a probe reports one FAIL whether `set` refused the file or the decoder
// declined to answer, and those are the two rates that must not be added together.
static uint64_t Rounds = 0;
static uint64_t Parsed = 0;
static uint64_t Answered = 0;

// Report the counts as libFuzzer reports its own totals: one line, on exit, for
// build.sh to hold to a floor. A crash aborts and prints nothing, which is correct --
// a crashed run is a finding, not a rate.
static void report_counts(void) {
    fprintf(stderr, "fuzz-tb-file: rounds %llu parsed %llu answered %llu\n",
            (unsigned long long) Rounds, (unsigned long long) Parsed,
            (unsigned long long) Answered);
    fflush(stderr);
}

// Point the engine's option seam at upstream's Syzygy defaults. `search_common.c`
// leaves these reading zero for a zone linked without a shell, and a zero
// cardinality short-circuits the ranking before it probes anything -- so without
// this the second half of each iteration would run no engine code at all. These
// are the four values `src/shell/syzygy_option.c` installs.
static int fuzz_probe_depth(void) { return 1; }
static int fuzz_probe_limit(void) { return 7; }
static bool fuzz_rule50(void) { return true; }

// Build "<TbDir>/<stem><ext>" into OUT.
static void tb_path(char *out, size_t out_len, const char *stem, const char *ext) {
    snprintf(out, out_len, "%s/%s%s", TbDir, stem, ext);
}

// Write BODY as a table file, prefixed with MAGIC and zero-padded to the length
// map_file accepts. Report failure so an iteration that cannot write does not go
// on to draw conclusions from a file that is not there.
static bool write_table(const char *stem, const char *ext, const uint8_t magic[4],
                        const uint8_t *body, size_t body_len) {
    char path[128];
    tb_path(path, sizeof path, stem, ext);

    FILE *f = fopen(path, "wb");
    if (f == nullptr) {
        return false;
    }
    bool ok = fwrite(magic, 1, 4, f) == 4;
    if (body_len != 0) {
        ok = ok && fwrite(body, 1, body_len, f) == body_len;
    }

    // Pad to `size % 64 == 16`, the shape upstream's header layout guarantees and
    // map_file refuses without. Never shorten: the fuzzer's own bytes stay whole.
    size_t total = 4 + body_len;
    while (ok && (total < 80 || total % 64 != 16)) {
        ok = fputc(0, f) != EOF;
        total += 1;
    }
    return fclose(f) == 0 && ok;
}

static void remove_all_tables(void) {
    char path[128];
    for (int i = 0; i < CONFIG_COUNT; ++i) {
        tb_path(path, sizeof path, Configs[i].stem, ".rtbw");
        (void) remove(path);
        tb_path(path, sizeof path, Configs[i].stem, ".rtbz");
        (void) remove(path);
    }
}

int LLVMFuzzerInitialize(int *argc, char ***argv) {
    (void) argc;
    (void) argv;

    // Same order main.c uses, and for the same reason: a Position built before
    // attacks_init reads zeroed attack tables. See 00-architecture.md.
    bitboards_init();
    attacks_init();
    threats_init();

    OptionSyzygyProbeDepth = fuzz_probe_depth;
    OptionSyzygyProbeLimit = fuzz_probe_limit;
    OptionSyzygy50MoveRule = fuzz_rule50;
    TbMaxCardinality = tablebase_max_cardinality;
    TbProbeFen = tablebase_probe_fen;
    TbProbeWdlPos = tablebase_probe_wdl_pos;

    if (atexit(report_counts) != 0) {
        abort();  // a lane whose counts are never printed has no floor
    }

    memcpy(TbDir, "/tmp/mcfish-fuzz-tb-XXXXXX", sizeof "/tmp/mcfish-fuzz-tb-XXXXXX");
    if (mkdtemp(TbDir) == nullptr) {
        abort();  // no directory, no target: fail loudly rather than fuzz nothing
    }

    // Reject a FEN that does not parse HERE rather than let every iteration probe
    // a position the engine refused: syzygy_probe_fen reports that case as
    // "unavailable", which is indistinguishable from a table this driver failed
    // to write, and the target would silently test nothing.
    for (int i = 0; i < CONFIG_COUNT; ++i) {
        Position pos;
        StateInfo si;
        if (!pos_set(&pos, Configs[i].fen, false, &si)) {
            abort();
        }
    }
    return 0;
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    if (size < 5) {
        return 0;
    }
    const Config *cfg = &Configs[data[0] % CONFIG_COUNT];

    // Take the WDL body's length from the input rather than splitting in half: a
    // fixed split cannot express a real pair, whose two files differ in length,
    // and expressing one is what lets the corpus be seeded. See the header.
    const size_t wdl_len = ((size_t) data[1] << 16) | ((size_t) data[2] << 8) | (size_t) data[3];
    data += 4;
    size -= 4;
    const size_t cut = wdl_len < size ? wdl_len : size;

    remove_all_tables();
    if (!write_table(cfg->stem, ".rtbw", WdlMagic, data, cut)
        || !write_table(cfg->stem, ".rtbz", DtzMagic, data + cut, size - cut)) {
        return 0;
    }

    tablebase_init(TbDir, strlen(TbDir));
    ++Rounds;
    const TbProbeResult probe = tablebase_probe_fen(cfg->fen, strlen(cfg->fen), false);
    if (probe.available != 0) {
        ++Answered;
    }

    // Rank the root as a `go` does. The probe above ends at the score; this is
    // what INDEXES with it. See the header.
    Position pos;
    StateInfo si;
    if (pos_set(&pos, cfg->fen, false, &si)) {
        // Ask the REGISTRY whether `set` accepted the WDL file, which the probe's
        // own answer cannot tell apart from a decoder that declined. `base` is left
        // null unless map_file and `set` both succeeded (registry.c:585).
        const TBTable *const t = registry_get(syzygy_position_key(&pos));
        if (t != nullptr && t->base != nullptr) {
            ++Parsed;
        }

        ExtMove list[MAX_MOVES];
        const size_t n = (size_t) (generate_legal(&pos, list) - list);
        Move moves[MAX_MOVES];
        for (size_t i = 0; i < n; ++i) {
            moves[i] = list[i].move;
        }
        RootMoveList built;
        if (root_moves_build(&pos, cfg->fen, false, moves, n, &built)) {
            root_moves_free(&built);
        }
    }

    // Release this generation before the next input inherits it: the mappings and
    // the parse arena both live until the following init, and a fuzzer that hands
    // input N+1 the state input N built is not testing either one.
    tablebase_init("", 0);
    return 0;
}
