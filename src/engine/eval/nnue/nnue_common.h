// Own the NNUE quantization vocabulary: the evaluation-file version, the scaling
// constants, and the fixed-width weight types every layer reads.
//
// Each width here is the width the .nnue file stores and the width the quantized
// arithmetic that consumes it relies on. Widening one silently changes a rounding
// boundary, so the widths are load-bearing, not a style choice. Read every
// multi-byte quantity out of the file byte by byte: the file's byte order is
// fixed little-endian, the host's is not.
//
// Mirrors src/nnue/nnue_common.h:56-102 (types, Version, scaling constants,
// CacheLineSize, the LEB128 magic, ceil_to_multiple).

#ifndef MCFISH_NNUE_COMMON_H
#define MCFISH_NNUE_COMMON_H

#include <stddef.h>
#include <stdint.h>

typedef int16_t BiasType;
typedef int8_t ThreatWeightType;
typedef int16_t WeightType;
typedef int32_t PSQTWeightType;
typedef uint32_t IndexType;

// Type of input feature after conversion (nnue_common.h:97).
typedef uint8_t TransformedFeatureType;

// Version of the evaluation file (nnue_common.h:64, the post-merge format).
#define NNUE_VERSION 0x6A448AFAu

enum {
    // Constants used in evaluation value calculation (nnue_common.h:67-70).
    NNUE_OUTPUT_SCALE = 16,
    NNUE_WEIGHT_SCALE_BITS = 6,
    NNUE_FT_MAX_VAL = 255,
    NNUE_HIDDEN_ONE_VAL = 128,

    // Size of cache line, in bytes (nnue_common.h:73).
    NNUE_CACHE_LINE_SIZE = 64,
};

#define NNUE_LEB128_MAGIC "COMPRESSED_LEB128"
#define NNUE_LEB128_MAGIC_SIZE (sizeof(NNUE_LEB128_MAGIC) - 1)

// Round N up to a multiple of BASE (ceil_to_multiple, nnue_common.h:101). Both
// arguments must already be `size_t`; the layout constants below depend on it
// staying an integer-constant expression.
#define NNUE_CEIL_TO_MULTIPLE(n, base) (((n) + (base) - 1) / (base) * (base))

// Read a little-endian uint32_t from P. Assemble it from bytes rather than
// casting a pointer: the file layout is fixed regardless of host endianness, and
// the source need not be aligned.
static inline uint32_t nnue_read_u32_le(const uint8_t *p) {
    return (uint32_t) p[0] | ((uint32_t) p[1] << 8) | ((uint32_t) p[2] << 16)
         | ((uint32_t) p[3] << 24);
}

// Read a little-endian int32_t from P. The uint32_t -> int32_t conversion is a
// two's-complement reinterpretation in C23, which is what the file stores.
static inline int32_t nnue_read_i32_le(const uint8_t *p) { return (int32_t) nnue_read_u32_le(p); }

#endif  // MCFISH_NNUE_COMMON_H
