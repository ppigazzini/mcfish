#include "uci_output.h"

#include <string.h>

// Resolve stdout lazily. A file-scope initialiser cannot name `stdout` on every
// platform (it is not a constant expression), and a null here means "the process
// stdout", which keeps the default free of a startup hook.
static FILE *OutStream = nullptr;
static FILE *LogStream = nullptr;

static uint64_t LastNodesSearched = 0;
static bool QuietMode = false;

static FILE *resolve_out(void) { return OutStream ? OutStream : stdout; }

// Write the line and its newline to one stream. Two calls, not one, because the
// line arrives as (pointer, length) and may hold no NUL. Flush per line: a GUI
// reads the engine over a pipe, where stdio would otherwise buffer a whole
// search's worth of `info` before the first byte arrives.
static void write_line(FILE *stream, const char *line, size_t len) {
    if (len != 0)
        (void) fwrite(line, 1, len, stream);
    (void) fputc('\n', stream);
    (void) fflush(stream);
}

void uci_output_set_stream(FILE *stream) { OutStream = stream; }

void uci_output_print_line(const char *line, size_t len) {
    write_line(resolve_out(), line, len);
    if (LogStream)
        write_line(LogStream, line, len);
}

void uci_output_emit_line(const char *line) {
    if (!line)
        line = "";
    uci_output_print_line(line, strlen(line));
}

void uci_output_start_logger(const char *name) {
    if (LogStream) {
        (void) fclose(LogStream);
        LogStream = nullptr;
    }
    if (!name || name[0] == '\0')
        return;
    LogStream = fopen(name, "w");
}

void uci_output_shutdown(void) {
    if (LogStream) {
        (void) fclose(LogStream);
        LogStream = nullptr;
    }
    OutStream = nullptr;
}

void uci_output_set_last_nodes_searched(uint64_t nodes) { LastNodesSearched = nodes; }

uint64_t uci_output_last_nodes_searched(void) { return LastNodesSearched; }

void uci_output_reset_last_nodes_searched(void) { LastNodesSearched = 0; }

void uci_output_set_quiet(bool quiet) { QuietMode = quiet; }

bool uci_output_is_quiet(void) { return QuietMode; }
