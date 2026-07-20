// Own the engine's stdio: the single funnel every outgoing byte leaves through, the
// two engine line-sinks, and the `Debug Log File` tee. This is the only module in
// the tree that writes to a stream, which is what makes the session log a copy of
// the whole conversation rather than a subset.
//
// Golden: upstream `misc.cpp` (struct Tie / Logger) and `uci.cpp` (print_info_string).

#ifndef MCFISH_UCI_OUTPUT_H
#define MCFISH_UCI_OUTPUT_H

#include <stddef.h>

// Write S verbatim to stdout and tee it to the log — no newline appended, so a
// caller composing a multi-line block controls its own breaks.
void uci_output_write(const char *s);

// printf to stdout, teed to the log.
[[gnu::format(printf, 1, 2)]]
void uci_output_printf(const char *fmt, ...);

// Tee one input line to the log with the ">> " prefix, before it is executed, so the
// log interleaves commands and replies in the order they happened.
void uci_output_log_input(const char *s, size_t n);

// The search/status line sink: write LINE and a newline. Installed as the search's
// emitter, so a PV line built in a large engine-side buffer is not staged through a
// smaller one.
void uci_output_emit_line(const char *line);

// The option on-change sink: one `info string` line per line of MESSAGE. The
// callbacks return bare text; the `info string ` prefix belongs to the transport.
void uci_output_emit_info(const char *message);

// Open FNAME as the session log, closing any previous one, or close the log when
// FNAME is empty. Exits on a path it cannot open, as upstream does.
void uci_output_start_logger(const char *fname);

#endif  // MCFISH_UCI_OUTPUT_H
