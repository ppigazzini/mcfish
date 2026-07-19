// Own the four Syzygy UCI options and bind the tablebase seams.
//
// Kept out of uci.c on purpose: the option state, the parse and the seam
// registration are one unit, and uci.c only dispatches to it. The engine reads
// these values through `option_source.h` and probes through `tb_source.h`;
// nothing here is reachable from the engine zone.
//
// Golden: `Stockfish/src/engine.cpp:125-134` (the four option declarations).

#ifndef MCFISH_SYZYGY_OPTION_H
#define MCFISH_SYZYGY_OPTION_H

#include <stdbool.h>

// Bind the option and probe seams to this module. Call once at startup, before
// the first `go`: until it runs the engine reads the neutral defaults (probe
// limit 0, no prober), which never probe.
void syzygy_option_install(void);

// Apply `setoption name NAME value VALUE`. Return true when NAME is one of the
// four Syzygy options, so the caller stops looking. A VALUE outside a spin's
// range leaves the option unchanged, as upstream's `Option::operator=`
// (ucioption.cpp:168) does.
bool syzygy_option_set(const char *name, const char *value);

// Print the four `option name ...` lines of the `uci` handshake.
void syzygy_option_print(void);

#endif  // MCFISH_SYZYGY_OPTION_H
