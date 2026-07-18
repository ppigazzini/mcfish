// Inject the monotonic clock time management reads.
//
// Reading the OS clock is a platform service, so the engine reaches it through a
// function pointer the platform registers at startup. The default is a real
// per-call monotonic counter, not a stub: every property time management requires
// of it holds, and a headless run stays deterministic. Unregistered, only the
// UNIT is wrong (ticks, not milliseconds), which no headless root reads because
// none of them is time-limited.
//
// Ported from zfish `engine/search/time_source.zig`.

#ifndef CCFISH_TIME_SOURCE_H
#define CCFISH_TIME_SOURCE_H

#include <stdint.h>

// Return monotonic time in milliseconds.
extern int64_t (*TimeNowMs)(void);

#endif  // CCFISH_TIME_SOURCE_H
