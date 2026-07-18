// Own the UCI protocol surface: the command loop, the option table, and every
// line the engine prints.
//
// This is the only zone that touches stdio. The engine zone emits through the
// sink `uci_loop` installs (search_set_output), which is what lets a gate drive a
// search and read its output without a subprocess.

#ifndef CCFISH_UCI_H
#define CCFISH_UCI_H

#include <stddef.h>

// Read commands from stdin until `quit` or EOF. Handle ARGC/ARGV as a single
// command first, so `ccfish bench` and `ccfish "go depth 5"` work non-interactively.
void uci_loop(int argc, char **argv);

// Run one command through the same handler the loop uses, so bench drives the
// engine over the real UCI surface rather than a private path. A second entry
// point into the search would be free to drift from the one users exercise --
// which is exactly what makes a bench number stop meaning anything.
void uci_execute(const char *line);

// Write the current position's FEN into BUF (needs >= 128 bytes).
void uci_current_fen(char *buf, size_t buf_len);

#endif  // CCFISH_UCI_H
