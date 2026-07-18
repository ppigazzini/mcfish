#include "misc.h"

#include <stdio.h>
#include <string.h>

#include <unistd.h>

// Hold the returned strings. Each function owns one buffer and overwrites it on
// every call, which is the contract misc.h states: the caller prints the result
// before calling again.
static char VersionBuf[64];
static char InfoBuf[256];
static char CompilerBuf[256];

const char *engine_version_info(void) {
    snprintf(VersionBuf, sizeof VersionBuf, "%s %s", ENGINE_NAME, ENGINE_VERSION);
    return VersionBuf;
}

const char *engine_info(bool to_uci) {
    snprintf(InfoBuf, sizeof InfoBuf, "%s%s%s", engine_version_info(),
             to_uci ? "\nid author " : " by ", ENGINE_AUTHORS);
    return InfoBuf;
}

const char *compiler_info(void) {
#if defined(__clang__)
    snprintf(CompilerBuf, sizeof CompilerBuf, "Compiled by clang %d.%d.%d, C%ld", __clang_major__,
             __clang_minor__, __clang_patchlevel__, __STDC_VERSION__);
#elif defined(__GNUC__)
    snprintf(CompilerBuf, sizeof CompilerBuf, "Compiled by gcc %d.%d.%d, C%ld", __GNUC__,
             __GNUC_MINOR__, __GNUC_PATCHLEVEL__, __STDC_VERSION__);
#else
    snprintf(CompilerBuf, sizeof CompilerBuf, "Compiled by an unknown compiler, C%ld",
             __STDC_VERSION__);
#endif
    return CompilerBuf;
}

int hardware_concurrency(void) {
#ifdef _SC_NPROCESSORS_ONLN
    const long n = sysconf(_SC_NPROCESSORS_ONLN);
    return n > 0 ? (int) n : 0;
#else
    // glibc hides _SC_NPROCESSORS_ONLN behind __USE_GNU, and build.sh compiles
    // with _POSIX_C_SOURCE only. Report 0 — "unknown" — rather than guessing 1,
    // which would read as a real single-core answer. Nothing consumes this yet:
    // the `Threads` maximum is pinned at 1 until the thread pool lands.
    return 0;
#endif
}

bool str_to_size_t(const char *s, size_t *out) {
    const char *p = s;
    while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r')
        ++p;
    if (*p == '+')
        ++p;

    const char *digits = p;
    size_t value = 0;
    for (; *p >= '0' && *p <= '9'; ++p) {
        const size_t digit = (size_t) (*p - '0');
        // Check before multiplying: size_t arithmetic wraps silently, and a
        // wrapped result is indistinguishable from a legitimate small value.
        if (value > (SIZE_MAX - digit) / 10)
            return false;
        value = value * 10 + digit;
    }

    if (p == digits || *p != '\0')
        return false;

    *out = value;
    return true;
}

bool is_whitespace(const char *s) {
    for (; *s; ++s)
        if (*s != ' ' && *s != '\t' && *s != '\n' && *s != '\r' && *s != '\v' && *s != '\f')
            return false;
    return true;
}

uint64_t hash_bytes(const void *data, size_t len) {
    const uint8_t *bytes = (const uint8_t *) data;
    const uint64_t m = 0xc6a4a7935bd1e995ULL;
    const int r = 47;

    // Every multiply and add below is modulo 2^64 by definition of MurmurHash2.
    // uint64_t wraps, which is what makes this a faithful port; do not "fix" it
    // with a wider type or a saturating check.
    uint64_t hash = (uint64_t) len * m;
    const size_t aligned_end = len & ~(size_t) 7;

    for (size_t i = 0; i < aligned_end; i += 8) {
        // Assemble little-endian by hand so the digest does not depend on the
        // host's byte order.
        uint64_t k = 0;
        for (int b = 7; b >= 0; --b)
            k = (k << 8) | bytes[i + (size_t) b];
        k *= m;
        k ^= k >> r;
        k *= m;

        hash ^= k;
        hash *= m;
    }

    if ((len & 7) != 0) {
        uint64_t k = 0;
        for (size_t tail = len & 7; tail != 0; --tail)
            k = (k << 8) | bytes[aligned_end + tail - 1];
        hash ^= k;
        hash *= m;
    }

    hash ^= hash >> r;
    hash *= m;
    hash ^= hash >> r;

    return hash;
}
