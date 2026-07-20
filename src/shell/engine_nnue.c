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

void engine_nnue_set_output(void (*emit_line)(const char *line)) { EmitLine = emit_line; }

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
    if (NetOk)
        return;

    fprintf(stderr,
            "ERROR: Network evaluation parameters compatible with the engine must be "
            "available.\n"
            "ERROR: The network file %s was not loaded successfully.\n"
            "ERROR: The UCI option EvalFile might need to specify the full path, including "
            "the directory name, to the network file.\n"
            "ERROR: The default net can be downloaded from: "
            "https://tests.stockfishchess.org/api/nn/%s\n"
            "ERROR: The engine will be terminated now.\n",
            WantedFile, eval_nnue_default_file_name());
    exit(EXIT_FAILURE);
}
