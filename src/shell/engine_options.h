// Own the option table: the wire-order registration, the on-change callbacks that
// make each subsystem reachable from a `setoption`, and the readers the session and
// the search reach it through. tools/handshake.golden pins the registration order.
//
// Golden: upstream `engine.cpp` (the option registration in the constructor).

#ifndef MCFISH_ENGINE_OPTIONS_H
#define MCFISH_ENGINE_OPTIONS_H

#include "ucioption.h"

#include <stddef.h>

// Fill the table in upstream's wire order with its wired callbacks. Call once.
void engine_options_register(void);

// Install the sink on-change messages emit through (one `info string` line per line).
void engine_options_set_info(void (*emit_info)(const char *message));

// The value readers. engine_options_get_int matches search_set_option_source's
// signature, so the search reads options from the same table the handshake renders.
OptionsMap *engine_options_map(void);
int engine_options_get_int(const char *name);
const char *engine_options_get_string(const char *name);

// Apply a `setoption` body. Returns OPTION_SET_UNKNOWN with *NAME filled when the
// option does not exist.
int engine_options_apply(const char *args, char name[OPTION_NAME_MAX]);

// Render the table for the `uci` handshake.
void engine_options_render(char *buf, size_t buf_len);

#endif  // MCFISH_ENGINE_OPTIONS_H
