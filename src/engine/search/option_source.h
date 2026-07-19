// Inject the UCI-option reads the search depends on.
//
// The options live in the shell's model, so the engine reads them through
// function pointers the shell registers at startup rather than importing a shell
// module. The defaults return neutral values (0 / false), which is the correct
// answer for a headless depth-only search with no option model attached.
//
// Treat all four as SEARCH-AFFECTING when unregistered: they answer rather than
// abort, so a missing registration is a wrong search, not a crash — the one
// failure shape the bench signature cannot catch.
//
// Ported from zfish `engine/search/option_source.zig`.

#ifndef MCFISH_OPTION_SOURCE_H
#define MCFISH_OPTION_SOURCE_H

// Read an integer UCI option by name; 0 when unset or no model is attached.
extern int (*OptionIntByName)(const char *name);

// Read the Syzygy probe settings. 0 / false read as "never probe", which is
// exactly true when no tablebases are loaded.
extern int (*OptionSyzygyProbeDepth)(void);
extern int (*OptionSyzygyProbeLimit)(void);
extern bool (*OptionSyzygy50MoveRule)(void);

#endif  // MCFISH_OPTION_SOURCE_H
