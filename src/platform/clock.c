#include "clock.h"

#include <time.h>

uint64_t now_ms(void) {
    struct timespec ts;

    // Read the monotonic clock, not the realtime one: a wall-clock adjustment
    // mid-search would otherwise make the time budget jump or go negative.
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t) ts.tv_sec * 1000 + (uint64_t) ts.tv_nsec / 1000000;
}
