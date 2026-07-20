// Implement the engine's stdio funnel and the debug-log tee. See uci_output.h.

#include "uci_output.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static FILE *LogFile = nullptr;

// Track the last byte written to the log so a prefix is emitted exactly at line
// starts, across the input/output boundary. Never reset on reopen; upstream shares
// one `last` between the cin and cout ties (misc.cpp, Tie::log).
static int LogLast = '\n';

static void log_bytes(const char *s, size_t n, const char *prefix) {
    if (!LogFile)
        return;
    for (size_t i = 0; i < n; ++i) {
        if (LogLast == '\n')
            fwrite(prefix, 1, 3, LogFile);
        fputc(s[i], LogFile);
        LogLast = (unsigned char) s[i];
    }
    fflush(LogFile);
}

void uci_output_write(const char *s) {
    const size_t n = strlen(s);
    fwrite(s, 1, n, stdout);
    fflush(stdout);
    log_bytes(s, n, "<< ");
}

void uci_output_printf(const char *fmt, ...) {
    char buf[4096];
    va_list ap;
    va_start(ap, fmt);
    const int n = vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    if (n <= 0)
        return;
    // vsnprintf reports what it WOULD have written; clamp to what it did.
    const size_t len = (size_t) n < sizeof buf ? (size_t) n : sizeof buf - 1;
    fwrite(buf, 1, len, stdout);
    fflush(stdout);
    log_bytes(buf, len, "<< ");
}

void uci_output_log_input(const char *s, size_t n) { log_bytes(s, n, ">> "); }

// Write the search's line and its terminator without going through uci_output_printf:
// a PV info line is built in a 5120-byte buffer (search_emit.c LINE_MAX) and would be
// silently truncated by the smaller printf staging buffer.
void uci_output_emit_line(const char *line) {
    uci_output_write(line);
    uci_output_write("\n");
}

// Route an on-change message, one "info string" line per line of text, as upstream's
// UCIEngine::print_info_string does (uci.cpp:55).
void uci_output_emit_info(const char *message) {
    const char *line = message;
    while (*line) {
        const char *end = strchr(line, '\n');
        const int len = end ? (int) (end - line) : (int) strlen(line);
        if (len > 0)
            uci_output_printf("info string %.*s\n", len, line);
        if (!end)
            break;
        line = end + 1;
    }
}

// Open FNAME as the session log, closing any previous one. Exit on a path that
// cannot be opened, as upstream does (misc.cpp, Logger::start): an operator who
// asked for a transcript and silently got none cannot tell the run apart from one
// where nothing happened.
void uci_output_start_logger(const char *fname) {
    if (LogFile) {
        fclose(LogFile);
        LogFile = nullptr;
    }
    if (!fname[0])
        return;

    LogFile = fopen(fname, "w");
    if (!LogFile) {
        fprintf(stderr, "Unable to open debug log file %s\n", fname);
        exit(EXIT_FAILURE);
    }
}
