// Provide the one clock the engine reads.
//
// Every time-dependent decision routes through now_ms so the search zone stays
// free of OS calls and the gates can reason about a single source of nondeterminism.

#ifndef CCFISH_CLOCK_H
#define CCFISH_CLOCK_H

#include <stdint.h>

// Return milliseconds from an unspecified monotonic origin. Only differences are
// meaningful; the origin is not the epoch and does not survive a reboot.
uint64_t now_ms(void);

#endif  // CCFISH_CLOCK_H
