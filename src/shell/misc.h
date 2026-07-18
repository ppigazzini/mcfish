// Own the engine's identity strings and the small process-level utilities
// upstream keeps in misc.cpp.
//
// Every string here is returned from static storage owned by this module and is
// valid until the next call to the same function. Nothing here allocates, and
// nothing here reads engine state — this module sits below the engine object so
// `id name` can be printed before anything else is constructed.
//
// Port source: zfish `shell/misc.zig`. Golden: upstream `misc.cpp:141`
// (engine_version_info), `misc.cpp:177` (engine_info), `misc.cpp:190`
// (compiler_info), `misc.cpp` str_to_size_t.

#ifndef CCFISH_MISC_H
#define CCFISH_MISC_H

#include <stddef.h>
#include <stdint.h>

// Name this engine, not upstream. ccfish is a port in progress and must not
// answer `uci` as "Stockfish": a GUI, a tournament manager, and every bug report
// key off this string, and claiming upstream's identity while the search, NNUE
// and Syzygy are unported misattributes the results. The `id` lines are pinned
// byte-for-byte by tools/handshake.golden.
#define ENGINE_NAME "ccfish"
#define ENGINE_VERSION "dev"
#define ENGINE_AUTHORS "the Stockfish developers (see AUTHORS)"

// Return "<name> <version>". Upstream appends the git date and SHA to a `dev`
// build (misc.cpp:145); ccfish's build.sh injects no git metadata, so the
// version stands alone. Wiring that in means defining GIT_DATE and GIT_SHA at
// compile time and is a build.sh change, not a source one.
const char *engine_version_info(void);

// Return the `uci` identity block. With TO_UCI the author is introduced by
// "\nid author ", so the result drops straight after "id name "; without it the
// separator is " by ", which is the banner form. Port of upstream misc.cpp:177.
const char *engine_info(bool to_uci);

// Report the compiler that built this binary and the C standard it compiled to.
// The CI builds this tree with both clang and gcc, and a bug report naming the
// wrong one costs a round trip.
const char *compiler_info(void);

// Report the number of hardware threads, 0 when it cannot be determined. This is
// upstream's get_hardware_concurrency, which sizes the `Threads` maximum. It has
// no effect on the advertised maximum while the thread pool is unported.
int hardware_concurrency(void);

// Parse S as a decimal size_t. Return false on an empty string, a non-digit, or
// an overflow, leaving *OUT untouched. Upstream returns std::optional<usize>
// here and the engine treats a nullopt as "leave the option alone".
bool str_to_size_t(const char *s, size_t *out);

// Report whether S is empty or entirely ASCII whitespace.
bool is_whitespace(const char *s);

// Hash LEN bytes at DATA with the 64-bit MurmurHash2 upstream uses to fingerprint
// a loaded NNUE net. The multiplies and shifts wrap by construction — the
// unsigned arithmetic below is the definition, not an accident to be "fixed".
uint64_t hash_bytes(const void *data, size_t len);

#endif  // CCFISH_MISC_H
