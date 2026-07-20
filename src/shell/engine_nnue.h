// Own the resident net: the directory it loads from, the load itself, the status
// line, and the refuse-to-run check. The evaluation arenas belong to the engine
// zone; this module only decides which net file fills them and reports the outcome.
//
// Golden: upstream `engine.cpp` (load_networks / verify_networks) and
// `nnue/network.cpp:165-187`.

#ifndef MCFISH_ENGINE_NNUE_H
#define MCFISH_ENGINE_NNUE_H

// Install the line sink the status is announced through (the search/status sink).
void engine_nnue_set_output(void (*emit_line)(const char *line));

// Record the directory ARGV0 was launched from, with its trailing separator, as the
// third candidate network_load searches (after "<internal>" and the cwd).
void engine_nnue_set_root(const char *argv0);

// Load EVAL_FILE from the recorded root. Silent: engine_nnue_report announces the
// outcome, so a load and a re-load read the same on the wire.
void engine_nnue_reload(const char *eval_file);

// Announce the resident net (or the classical fallback) through the sink.
void engine_nnue_report(void);

// Terminate the process unless a usable net is loaded, printing upstream's five error
// lines to stderr, naming the file that failed.
void engine_nnue_verify(void);

#endif  // MCFISH_ENGINE_NNUE_H
