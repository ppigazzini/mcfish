// Own the UCI protocol surface: the command loop, the option table, and every
// line the engine prints.
//
// This is the only zone that touches stdio. The engine zone emits through the
// sink `uci_loop` installs (search_set_output), which is what lets a gate drive a
// search and read its output without a subprocess.

#ifndef CCFISH_UCI_H
#define CCFISH_UCI_H

// Read commands from stdin until `quit` or EOF. Handle ARGC/ARGV as a single
// command first, so `ccfish bench` and `ccfish "go depth 5"` work non-interactively.
void uci_loop(int argc, char **argv);

#endif  // CCFISH_UCI_H
