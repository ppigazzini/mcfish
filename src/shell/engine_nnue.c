// Implement the resident-net policy. See engine_nnue.h.

#include "engine_nnue.h"

#include "../engine/eval/evaluate.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// The directory the binary was launched from, with its trailing separator.
static char RootDirectory[512];

// The last file we tried to load, kept so engine_nnue_verify can name it.
static char WantedFile[256];

static bool NetOk = false;
static void (*EmitLine)(const char *line) = nullptr;
static void (*EmitInfo)(const char *message) = nullptr;

void engine_nnue_set_output(void (*emit_line)(const char *line)) { EmitLine = emit_line; }

void engine_nnue_set_info(void (*emit_info)(const char *message)) { EmitInfo = emit_info; }

void engine_nnue_set_root(const char *argv0) {
    RootDirectory[0] = '\0';
    if (argv0 == nullptr)
        return;
    const char *slash = strrchr(argv0, '/');
    if (slash == nullptr)
        return;
    const size_t len = (size_t) (slash - argv0) + 1;
    if (len >= sizeof RootDirectory)
        return;
    memcpy(RootDirectory, argv0, len);
    RootDirectory[len] = '\0';
}

void engine_nnue_reload(const char *eval_file) {
    snprintf(WantedFile, sizeof WantedFile, "%s", eval_file);
    NetOk = eval_nnue_load(RootDirectory, eval_file);
}

void engine_nnue_report(void) {
    if (!EmitLine)
        return;
    char line[512];
    snprintf(line, sizeof line, "info string %s", eval_nnue_status());
    EmitLine(line);
}

// Refuse to run without a usable net, as upstream does (nnue/network.cpp:165-187,
// reached from `go`, `perft` and `eval`). The message is upstream's five lines
// verbatim, including the file name and the download URL.
void engine_nnue_verify(void) {
    if (NetOk) {
        // One line per network replica, which upstream emits from the same function
        // right after the summary (engine.cpp:271-299) and mcfish did not emit at all
        // -- so the pre-`go` announcement block was one line shorter here than in
        // every other Stockfish, and a GUI parsing that block saw a short one.
        //
        // The COUNT and the SHAPE are upstream's. The STATUS is not, deliberately.
        // Upstream reports how each replica is backed, and on this box it answers
        // `Shared memory.` because it maps the net system-wide; mcfish holds ONE
        // network in ordinary process memory and has no such mapping, so the true
        // answer is `Local memory.` -- one of upstream's own four strings
        // (No allocation. / Local memory. / Shared memory. / Unknown status.).
        //
        // Saying `Shared memory.` would match the byte by asserting something false
        // about the allocation, and saying nothing -- which is what this did -- hides
        // a real difference behind an absent line. The count stays 1 until the network
        // registers for NUMA replication, which AGENTS.md still lists as open.
        if (EmitLine != nullptr)
            EmitLine("info string Network replica 1: Local memory.");
        return;
    }

    // Report through the INFO sink, not stderr. Upstream hands the whole block to
    // print_info_string (nnue/network.cpp:187 -> uci.cpp:55), which splits it and
    // prefixes each line with `info string` on stdout. A GUI reads stdout; writing a
    // fatal diagnostic to stderr unprefixed means it sees nothing at all before the
    // engine exits.
    char msg[1024];
    snprintf(msg, sizeof msg,
             "ERROR: Network evaluation parameters compatible with the engine must be "
             "available.\n"
             "ERROR: The network file %s was not loaded successfully.\n"
             "ERROR: The UCI option EvalFile might need to specify the full path, including "
             "the directory name, to the network file.\n"
             "ERROR: The default net can be downloaded from: "
             "https://tests.stockfishchess.org/api/nn/%s\n"
             "ERROR: The engine will be terminated now.",
             WantedFile, eval_nnue_default_file_name());
    if (EmitInfo != nullptr)
        EmitInfo(msg);
    exit(EXIT_FAILURE);
}
