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
// Golden: none. This is test infrastructure, not a port.

#include "../src/engine/board/attacks.h"
#include "../src/engine/board/bitboard.h"
#include "../src/engine/board/position.h"
#include "../src/engine/board/threats.h"
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
    (void) tablebase_probe_fen(cfg->fen, strlen(cfg->fen), false);

    // Release this generation before the next input inherits it: the mappings and
    // the parse arena both live until the following init, and a fuzzer that hands
    // input N+1 the state input N built is not testing either one.
    tablebase_init("", 0);
    return 0;
}
