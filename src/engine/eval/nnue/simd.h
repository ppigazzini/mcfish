// Own the vector vocabulary the NNUE kernels compute through: fixed-width integer
// lane vectors plus the element-wise operations over them.
//
// Provide ONE vocabulary in TWO implementations. `MCFISH_SIMD_VECTOR` (the default
// under GCC/clang) types a vector as the `vector_size` extension; the fallback types
// it as a struct wrapping an array and spells every operation as a lane loop.
//
// HOW BIT-IDENTITY IS GUARANTEED. Every operation in this header is element-wise and
// total: lane i of the result depends only on lane i of the operands, by the same
// per-lane C expression in both bodies, and the two bodies sit adjacent under a single
// `#if` so they are read together. Nothing here reduces, reassociates, rounds,
// saturates, or reorders lanes — the only horizontal step any kernel takes is written
// in ordinary scalar C outside this header and is therefore shared by both paths
// verbatim. A machine without the ISA runs the same arithmetic, so it must produce the
// same node count.
//
// Two integer hazards are settled here rather than at each call site:
//   - WRAPPING. The int16_t accumulator must wrap on overflow (upstream relies on it).
//     C signed overflow is UB and GCC leaves signed vector overflow undefined too, so
//     the accumulator family is uint16_t throughout and the int16_t view is reached by
//     a reinterpret. C23 pins the conversion back to two's complement.
//   - RIGHT SHIFT. C23 defines `>>` on a negative signed value as an arithmetic shift,
//     which both compilers' vector `>>` already do. Shifts here are therefore written
//     directly.
//
// Golden: the upstream `nnue/simd.h`.

#ifndef MCFISH_NNUE_SIMD_H
#define MCFISH_NNUE_SIMD_H

#include <stddef.h>
#include <stdint.h>

#if !defined(MCFISH_SIMD_SCALAR) && (defined(__GNUC__) || defined(__clang__))
    #define MCFISH_SIMD_VECTOR 1
#else
    #define MCFISH_SIMD_VECTOR 0
#endif

// Pull in the x86 intrinsic vocabulary once, so the nnz movemask can reach the native
// mask-register path (vptestmd/kmov, vmovmskps, or the SSE2 pack+pmovmskb) on the tiers
// that have it. The dot-product block below includes it again under its own guard; the
// header guard makes that a no-op.
#if MCFISH_SIMD_VECTOR && (defined(__AVX512F__) || defined(__AVX2__) || defined(__SSE2__))
    #include <immintrin.h>
#endif

// ---------------------------------------------------------------------------
// The family generator. Emits, for one (element type, lane count) pair, the whole
// element-wise op set. Unused members are `static inline` and cost nothing.
// ---------------------------------------------------------------------------

// Spell min/max as the elementwise builtin where the compiler has it. Both reduce
// to one instruction (pminsd/pmaxsd and friends); the fallback blend spells the same
// value in four vector ops, because C's `?:` does not apply to a vector type and
// there is nothing else portable to write. This sits in the feature transformer's
// inner loop, so the difference is not academic.
//
// clang has the builtin; gcc 13 -- the second compiler this repo builds under -- does
// not, hence the probe rather than a version test.
#if defined(__has_builtin)
    #if __has_builtin(__builtin_elementwise_min)
        #define NNUE_VEC_MIN(a, b) __builtin_elementwise_min((a), (b))
        #define NNUE_VEC_MAX(a, b) __builtin_elementwise_max((a), (b))
    #endif
#endif
#ifndef NNUE_VEC_MIN
    #define NNUE_VEC_MIN(a, b) \
        ((__typeof__(a)) (((a) & ((__typeof__(a)) ((a) < (b)))) \
                          | ((b) & ~((__typeof__(a)) ((a) < (b))))))
    #define NNUE_VEC_MAX(a, b) \
        ((__typeof__(a)) (((a) & ((__typeof__(a)) ((a) > (b)))) \
                          | ((b) & ~((__typeof__(a)) ((a) > (b))))))
#endif

#if MCFISH_SIMD_VECTOR

    #define NNUE_SIMD_TYPE(Type, Elem, Width) \
        typedef Elem Type __attribute__((vector_size((Width) * sizeof(Elem))))

    // Load/store through a typedef that CAPS the vector's natural alignment at the
    // arena's 64-byte guarantee (a vector_size type's natural alignment is its full
    // size — 128+ bytes here — which no arena offset provides). The _a forms exist
    // because a legacy-SSE instruction folds a memory operand only when the compiler
    // can prove 16-byte alignment: the memcpy load in _load lowers to movdqu, which
    // never folds, costing a separate load per psubw/paddw in the row kernels (the
    // same mechanism the SSSE3 nnue_dot_step fixed). Callers pass pointers at
    // NNUE_ALIGN-multiple offsets off 64-byte-aligned storage — rows narrower than
    // 64 bytes (the psqt's 32) claim only their own size.
    #define NNUE_SIMD_ALIGN_CAP(Type) (sizeof(Type) < 64 ? sizeof(Type) : 64)

    // clang-format off
#  define NNUE_SIMD_FAMILY(Pfx, Type, Elem, Width)                                          \
      typedef Type Pfx##_aligned_t __attribute__((aligned(NNUE_SIMD_ALIGN_CAP(Type))));     \
      static inline Type Pfx##_load(const Elem *p) {                                        \
          Type v;                                                                           \
          __builtin_memcpy(&v, p, sizeof v);                                                \
          return v;                                                                         \
      }                                                                                     \
      static inline void Pfx##_store(Elem *p, Type v) { __builtin_memcpy(p, &v, sizeof v); } \
      static inline Type Pfx##_load_a(const Elem *p) {                                      \
          return *(const Pfx##_aligned_t *) p;                                              \
      }                                                                                     \
      static inline void Pfx##_store_a(Elem *p, Type v) { *(Pfx##_aligned_t *) p = v; }     \
      static inline Type Pfx##_splat(Elem x) {                                              \
          Type z = { 0 };                                                                   \
          return z + x;                                                                     \
      }                                                                                     \
      static inline Type Pfx##_add(Type a, Type b) { return a + b; }                        \
      static inline Type Pfx##_sub(Type a, Type b) { return a - b; }                        \
      static inline Type Pfx##_mul(Type a, Type b) { return a * b; }                        \
      static inline Type Pfx##_shl(Type a, int s) { return a << s; }                        \
      static inline Type Pfx##_shr(Type a, int s) { return a >> s; }                        \
      static inline Type Pfx##_min(Type a, Type b) { return NNUE_VEC_MIN(a, b); }            \
      static inline Type Pfx##_max(Type a, Type b) { return NNUE_VEC_MAX(a, b); }            \
      static inline Elem Pfx##_lane(Type a, size_t i) { return a[i]; }                      \
      _Static_assert(sizeof(Type) == (Width) * sizeof(Elem), #Type " is " #Width " lanes")
    // clang-format on

    // Convert lane-wise between two families of the same lane COUNT. Widening
    // zero/sign-extends by the source element's signedness; narrowing truncates —
    // exactly the C conversion the scalar body writes out.
    #define NNUE_SIMD_CONVERT(Name, Dst, Src) \
        static inline Dst Name(Src v) { return __builtin_convertvector(v, Dst); }

    // Reinterpret the same bytes as another family. Same total size, no lane math.
    #define NNUE_SIMD_REINTERPRET(Name, Dst, Src) \
        static inline Dst Name(Src v) { return (Dst) v; }

#else  // scalar fallback

    #define NNUE_SIMD_TYPE(Type, Elem, Width) \
        typedef struct { \
            Elem l[Width]; \
        } Type

    // clang-format off
#  define NNUE_SIMD_FAMILY(Pfx, Type, Elem, Width)                                       \
      static inline Type Pfx##_load(const Elem *p) {                                     \
          Type v;                                                                        \
          __builtin_memcpy(&v, p, sizeof v);                                             \
          return v;                                                                      \
      }                                                                                  \
      static inline void Pfx##_store(Elem *p, Type v) { __builtin_memcpy(p, &v, sizeof v); } \
      static inline Type Pfx##_load_a(const Elem *p) { return Pfx##_load(p); }           \
      static inline void Pfx##_store_a(Elem *p, Type v) { Pfx##_store(p, v); }           \
      static inline Type Pfx##_splat(Elem x) {                                           \
          Type r;                                                                        \
          for (size_t i = 0; i < (Width); i++)                                           \
              r.l[i] = x;                                                                \
          return r;                                                                      \
      }                                                                                  \
      static inline Type Pfx##_add(Type a, Type b) {                                     \
          Type r;                                                                        \
          for (size_t i = 0; i < (Width); i++)                                           \
              r.l[i] = (Elem) (a.l[i] + b.l[i]);                                         \
          return r;                                                                      \
      }                                                                                  \
      static inline Type Pfx##_sub(Type a, Type b) {                                     \
          Type r;                                                                        \
          for (size_t i = 0; i < (Width); i++)                                           \
              r.l[i] = (Elem) (a.l[i] - b.l[i]);                                         \
          return r;                                                                      \
      }                                                                                  \
      static inline Type Pfx##_mul(Type a, Type b) {                                     \
          Type r;                                                                        \
          for (size_t i = 0; i < (Width); i++)                                           \
              r.l[i] = (Elem) (a.l[i] * b.l[i]);                                         \
          return r;                                                                      \
      }                                                                                  \
      static inline Type Pfx##_shl(Type a, int s) {                                      \
          Type r;                                                                        \
          for (size_t i = 0; i < (Width); i++)                                           \
              r.l[i] = (Elem) (a.l[i] << s);                                             \
          return r;                                                                      \
      }                                                                                  \
      static inline Type Pfx##_shr(Type a, int s) {                                      \
          Type r;                                                                        \
          for (size_t i = 0; i < (Width); i++)                                           \
              r.l[i] = (Elem) (a.l[i] >> s);                                             \
          return r;                                                                      \
      }                                                                                  \
      static inline Type Pfx##_min(Type a, Type b) {                                     \
          Type r;                                                                        \
          for (size_t i = 0; i < (Width); i++)                                           \
              r.l[i] = a.l[i] < b.l[i] ? a.l[i] : b.l[i];                                \
          return r;                                                                      \
      }                                                                                  \
      static inline Type Pfx##_max(Type a, Type b) {                                     \
          Type r;                                                                        \
          for (size_t i = 0; i < (Width); i++)                                           \
              r.l[i] = a.l[i] > b.l[i] ? a.l[i] : b.l[i];                                \
          return r;                                                                      \
      }                                                                                  \
      static inline Elem Pfx##_lane(Type a, size_t i) { return a.l[i]; }                 \
      _Static_assert(sizeof(Type) == (Width) * sizeof(Elem), #Type " is " #Width " lanes")
    // clang-format on

    #define NNUE_SIMD_CONVERT(Name, Dst, Src) \
        static inline Dst Name(Src v) { \
            Dst r; \
            for (size_t i = 0; i < sizeof(r.l) / sizeof(r.l[0]); i++) \
                r.l[i] = (__typeof__(r.l[0])) v.l[i]; \
            return r; \
        }

    #define NNUE_SIMD_REINTERPRET(Name, Dst, Src) \
        static inline Dst Name(Src v) { \
            Dst r; \
            __builtin_memcpy(&r, &v, sizeof r); \
            return r; \
        }

#endif  // MCFISH_SIMD_VECTOR

// ---------------------------------------------------------------------------
// The families the NNUE kernels use. Widths are the kernels' tile widths, pinned
// where the kernel that owns them is documented.
// ---------------------------------------------------------------------------

// Silence GCC's -Wpsabi for the wide families. It reports that passing a vector wider
// than the enabled ISA's registers "changes the ABI" — true, and irrelevant: every
// function here is `static inline` and never crosses a translation unit, so no caller
// ever sees the ABI it is warning about.
#if defined(__GNUC__) && !defined(__clang__)
    #pragma GCC diagnostic push
    #pragma GCC diagnostic ignored "-Wpsabi"
#endif

// Accumulator row tile: 64 int16_t lanes, carried as uint16_t so the add/sub wrap.
NNUE_SIMD_TYPE(NnueV64u16, uint16_t, 64);
NNUE_SIMD_TYPE(NnueV64i8, int8_t, 64);
NNUE_SIMD_TYPE(NnueV64i16, int16_t, 64);
NNUE_SIMD_FAMILY(nnue_v64u16, NnueV64u16, uint16_t, 64);
NNUE_SIMD_FAMILY(nnue_v64i8, NnueV64i8, int8_t, 64);
NNUE_SIMD_FAMILY(nnue_v64i16, NnueV64i16, int16_t, 64);
NNUE_SIMD_CONVERT(nnue_v64_i8_to_i16, NnueV64i16, NnueV64i8)
NNUE_SIMD_REINTERPRET(nnue_v64_i16_as_u16, NnueV64u16, NnueV64i16)

// Feature-transformer output tile: 64 lanes through int16 -> uint16 -> uint32 -> uint8.
// Reuses NnueV64i16/NnueV64u16 from the row-tile block above. Wider than the accumulator
// row tile buys nothing there but does here: a sweep of TRANSFORM_VEC_WIDTH finds 64.
NNUE_SIMD_TYPE(NnueV64u32, uint32_t, 64);
NNUE_SIMD_TYPE(NnueV64u8, uint8_t, 64);
NNUE_SIMD_TYPE(NnueV16u32, uint32_t, 16);
NNUE_SIMD_FAMILY(nnue_v64u32, NnueV64u32, uint32_t, 64);
NNUE_SIMD_FAMILY(nnue_v64u8, NnueV64u8, uint8_t, 64);
NNUE_SIMD_FAMILY(nnue_v16u32, NnueV16u32, uint32_t, 16);
NNUE_SIMD_CONVERT(nnue_v64_i16_to_u16, NnueV64u16, NnueV64i16)
NNUE_SIMD_CONVERT(nnue_v64_u16_to_u32, NnueV64u32, NnueV64u16)
NNUE_SIMD_CONVERT(nnue_v64_u32_to_u16, NnueV64u16, NnueV64u32)
NNUE_SIMD_CONVERT(nnue_v64_u16_to_u8, NnueV64u8, NnueV64u16)
NNUE_SIMD_REINTERPRET(nnue_v64_u8_as_u32x16, NnueV16u32, NnueV64u8)

// Build the 16-bit movemask (bit g set iff 4-byte group g is non-zero) from DEFINED ops:
// (v != 0) yields all-ones per non-zero lane, AND with the per-lane bit weight, then a
// horizontal OR. A reinterpret of a bool-vector would be shorter but assumes the
// target-defined <N x i1> layout -- a backend using one byte per lane reads a few lanes as
// the whole mask and corrupts the nnz set into a wrong positional eval. Port of zfish
// d27ab1438. The scalar fallback spells the same per-lane reduction, so scalar==vector.
static inline uint32_t nnue_v16u32_movemask(NnueV16u32 v) {
#if MCFISH_SIMD_VECTOR && defined(__AVX2__)
    // Native path: sign-bit-per-dword movemask. vpcmpeqd against zero sets a lane to
    // all-ones exactly when it is zero, and vmovmskps harvests the eight sign bits into
    // eight scalar bits; invert to get the non-zero mask. Bit g == (lane g != 0), the same
    // value the reduce-OR spelling below produces, and no dependence on the <N x i1> layout.
    const uint32_t lo = (uint32_t) _mm256_movemask_ps((__m256) _mm256_cmpeq_epi32(
      (__m256i) __builtin_shufflevector(v, v, 0, 1, 2, 3, 4, 5, 6, 7), _mm256_setzero_si256()));
    const uint32_t hi = (uint32_t) _mm256_movemask_ps((__m256) _mm256_cmpeq_epi32(
      (__m256i) __builtin_shufflevector(v, v, 8, 9, 10, 11, 12, 13, 14, 15),
      _mm256_setzero_si256()));
    return (~lo & 0xffu) | ((~hi & 0xffu) << 8);
#elif MCFISH_SIMD_VECTOR && defined(__SSE2__)
    // Native SSE path: compare each dword group to zero, then narrow the four -1/0
    // lane masks 32->16->8 bits with the SIGNED saturating packs (which carry -1 and 0
    // through exactly) and harvest the sixteen sign bits with one pmovmskb — upstream's
    // NNZCursor::record2 shape (nnz_helper.h). The reduce-OR spelling below lowered to
    // a ~14-op value-domain horizontal reduce plus per-group bit arithmetic; this is
    // four compares, three packs and one movemask for the same 16 bits. Bit g ==
    // (lane g != 0), identical to the other bodies.
    typedef uint32_t NnueV4u32Chunk __attribute__((vector_size(16)));
    const NnueV4u32Chunk c0 = __builtin_shufflevector(v, v, 0, 1, 2, 3);
    const NnueV4u32Chunk c1 = __builtin_shufflevector(v, v, 4, 5, 6, 7);
    const NnueV4u32Chunk c2 = __builtin_shufflevector(v, v, 8, 9, 10, 11);
    const NnueV4u32Chunk c3 = __builtin_shufflevector(v, v, 12, 13, 14, 15);
    const __m128i zero = _mm_setzero_si128();
    const __m128i z01 =
      _mm_packs_epi32(_mm_cmpeq_epi32((__m128i) c0, zero), _mm_cmpeq_epi32((__m128i) c1, zero));
    const __m128i z23 =
      _mm_packs_epi32(_mm_cmpeq_epi32((__m128i) c2, zero), _mm_cmpeq_epi32((__m128i) c3, zero));
    const uint32_t zeros = (uint32_t) _mm_movemask_epi8(_mm_packs_epi16(z01, z23));
    return ~zeros & 0xffffu;
#elif MCFISH_SIMD_VECTOR && defined(__has_builtin) && __has_builtin(__builtin_reduce_or)
    const NnueV16u32 lane_bits = { 1u,   2u,   4u,    8u,    16u,   32u,   64u,    128u,
                                   256u, 512u, 1024u, 2048u, 4096u, 8192u, 16384u, 32768u };
    const NnueV16u32 nonzero = v != nnue_v16u32_splat(0);
    return (uint32_t) __builtin_reduce_or(nonzero & lane_bits);
#else
    uint32_t mask = 0;
    for (size_t g = 0; g < 16; g++)
        mask |= (uint32_t) (nnue_v16u32_lane(v, g) != 0) << g;
    return mask;
#endif
}

// Affine post-activation tile: 16 int32_t lanes for the clipped-ReLU sweep, plus an
// 8-lane tile the psqt refresh path pins to NNUE_PSQT_BUCKETS.
NNUE_SIMD_TYPE(NnueV8i32, int32_t, 8);
NNUE_SIMD_FAMILY(nnue_v8i32, NnueV8i32, int32_t, 8);
NNUE_SIMD_TYPE(NnueV16i32, int32_t, 16);
NNUE_SIMD_FAMILY(nnue_v16i32, NnueV16i32, int32_t, 16);

// Wide accumulator row tile: 128 int16_t lanes, carried as uint16_t so the add/sub wrap.
// Only the avx512 build widens ROW_TILE_WIDTH to 128 (nnue_accumulator.c); the narrower
// tiers stay at 64, where a 128-lane tile spills too many vector registers.
NNUE_SIMD_TYPE(NnueV128u16, uint16_t, 128);
NNUE_SIMD_TYPE(NnueV128i16, int16_t, 128);
NNUE_SIMD_FAMILY(nnue_v128u16, NnueV128u16, uint16_t, 128);
NNUE_SIMD_FAMILY(nnue_v128i16, NnueV128i16, int16_t, 128);

// Extra-wide accumulator row tile: 256 int16_t lanes, carried as uint16_t so the add/sub
// wrap. The avx512 build's ROW_TILE_WIDTH (nnue_acc_rowops.c) reaches it: 8 zmm hold the
// tile live across all four changed-row lists, upstream SIMDTiling's register budget.
NNUE_SIMD_TYPE(NnueV256u16, uint16_t, 256);
NNUE_SIMD_TYPE(NnueV256i16, int16_t, 256);
NNUE_SIMD_TYPE(NnueV256i8, int8_t, 256);
NNUE_SIMD_FAMILY(nnue_v256u16, NnueV256u16, uint16_t, 256);
NNUE_SIMD_FAMILY(nnue_v256i16, NnueV256i16, int16_t, 256);
NNUE_SIMD_FAMILY(nnue_v256i8, NnueV256i8, int8_t, 256);
NNUE_SIMD_CONVERT(nnue_v256_i8_to_i16, NnueV256i16, NnueV256i8)
NNUE_SIMD_REINTERPRET(nnue_v256_i16_as_u16, NnueV256u16, NnueV256i16)

// Affine dot-product tiles. The lane count is the layer's OUT*4, because the
// int8 weight layout interleaves each output's four sublanes; see nnue_affine.h.
NNUE_SIMD_TYPE(NnueV128i32, int32_t, 128);
NNUE_SIMD_TYPE(NnueV128i8, int8_t, 128);
NNUE_SIMD_TYPE(NnueV128u8, uint8_t, 128);
NNUE_SIMD_FAMILY(nnue_v128i32, NnueV128i32, int32_t, 128);
NNUE_SIMD_FAMILY(nnue_v128i8, NnueV128i8, int8_t, 128);
NNUE_SIMD_FAMILY(nnue_v128u8, NnueV128u8, uint8_t, 128);
NNUE_SIMD_CONVERT(nnue_v128_i8_to_i32, NnueV128i32, NnueV128i8)
NNUE_SIMD_CONVERT(nnue_v128_u8_to_i32, NnueV128i32, NnueV128u8)
NNUE_SIMD_CONVERT(nnue_v128_i8_to_i16, NnueV128i16, NnueV128i8)
NNUE_SIMD_REINTERPRET(nnue_v128_i16_as_u16, NnueV128u16, NnueV128i16)

// Wide feature-transformer output tile: 128 lanes, for TRANSFORM_VEC_WIDTH 128 on avx512.
// Reuses NnueV128i16/NnueV128u16 (row-tile block) and NnueV128u8 (affine block); adds the
// u32 product stage and the 32-group nnz movemask.
NNUE_SIMD_TYPE(NnueV128u32, uint32_t, 128);
NNUE_SIMD_TYPE(NnueV32u32, uint32_t, 32);
NNUE_SIMD_FAMILY(nnue_v128u32, NnueV128u32, uint32_t, 128);
NNUE_SIMD_FAMILY(nnue_v32u32, NnueV32u32, uint32_t, 32);
NNUE_SIMD_CONVERT(nnue_v128_i16_to_u16, NnueV128u16, NnueV128i16)
NNUE_SIMD_CONVERT(nnue_v128_u16_to_u32, NnueV128u32, NnueV128u16)
NNUE_SIMD_CONVERT(nnue_v128_u32_to_u16, NnueV128u16, NnueV128u32)
NNUE_SIMD_CONVERT(nnue_v128_u16_to_u8, NnueV128u8, NnueV128u16)
NNUE_SIMD_REINTERPRET(nnue_v128_u8_as_u32x32, NnueV32u32, NnueV128u8)

// The 32-bit movemask over 4-byte groups (see nnue_v16u32_movemask for the DEFINED-ops
// rationale). Port of zfish d27ab1438 at the 128-lane width.
static inline uint32_t nnue_v32u32_movemask(NnueV32u32 v) {
#if MCFISH_SIMD_VECTOR && defined(__AVX512F__)
    // Native path: vptestmd tests each dword non-zero straight into a mask register, and
    // kmov drops the 16-bit mask into a scalar. Two of them cover the 32 groups. Bit g ==
    // (lane g != 0), the same value the reduce-OR spelling below produces, without rebuilding
    // a value vector or the <N x i1> layout assumption.
    const NnueV16u32 lo =
      __builtin_shufflevector(v, v, 0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15);
    const NnueV16u32 hi =
      __builtin_shufflevector(v, v, 16, 17, 18, 19, 20, 21, 22, 23, 24, 25, 26, 27, 28, 29, 30, 31);
    const uint32_t m0 = _mm512_test_epi32_mask((__m512i) lo, (__m512i) lo);
    const uint32_t m1 = _mm512_test_epi32_mask((__m512i) hi, (__m512i) hi);
    return m0 | (m1 << 16);
#elif MCFISH_SIMD_VECTOR && defined(__has_builtin) && __has_builtin(__builtin_reduce_or)
    const NnueV32u32 lane_bits = {
        1u << 0,  1u << 1,  1u << 2,  1u << 3,  1u << 4,  1u << 5,  1u << 6,  1u << 7,
        1u << 8,  1u << 9,  1u << 10, 1u << 11, 1u << 12, 1u << 13, 1u << 14, 1u << 15,
        1u << 16, 1u << 17, 1u << 18, 1u << 19, 1u << 20, 1u << 21, 1u << 22, 1u << 23,
        1u << 24, 1u << 25, 1u << 26, 1u << 27, 1u << 28, 1u << 29, 1u << 30, 1u << 31,
    };
    const NnueV32u32 nonzero = v != nnue_v32u32_splat(0);
    return (uint32_t) __builtin_reduce_or(nonzero & lane_bits);
#else
    uint32_t mask = 0;
    for (size_t g = 0; g < 32; g++)
        mask |= (uint32_t) (nnue_v32u32_lane(v, g) != 0) << g;
    return mask;
#endif
}

NNUE_SIMD_TYPE(NnueV4i32, int32_t, 4);
NNUE_SIMD_TYPE(NnueV4i8, int8_t, 4);
NNUE_SIMD_TYPE(NnueV4u8, uint8_t, 4);
NNUE_SIMD_TYPE(NnueV1u32, uint32_t, 1);
NNUE_SIMD_FAMILY(nnue_v4i32, NnueV4i32, int32_t, 4);
NNUE_SIMD_FAMILY(nnue_v4i8, NnueV4i8, int8_t, 4);
NNUE_SIMD_FAMILY(nnue_v4u8, NnueV4u8, uint8_t, 4);
NNUE_SIMD_FAMILY(nnue_v1u32, NnueV1u32, uint32_t, 1);
NNUE_SIMD_CONVERT(nnue_v4_i8_to_i32, NnueV4i32, NnueV4i8)
NNUE_SIMD_CONVERT(nnue_v4_u8_to_i32, NnueV4i32, NnueV4u8)
NNUE_SIMD_REINTERPRET(nnue_v4_u32x1_as_u8, NnueV4u8, NnueV1u32)

// ---------------------------------------------------------------------------
// THE ONE REDUCING PRIMITIVE. Read the header comment's bit-identity argument
// first: everything above this line is element-wise, and this is not. It gets its
// own proof.
//
// nnue_dot4_i32 computes, for each of four output lanes q:
//
//     result[q] = sum over s in 0..3 of  in[4q + s] * w[4q + s]
//
// It exists because it is the whole affine kernel's cost. Accumulating in the
// interleaved OUT*4 domain needs 128 int32 lanes for the 32-output layer -- 512
// bytes against a 256-byte SSE register file -- so the accumulator spills and
// reloads every group. Folding the four sublanes INSIDE the multiply makes the
// accumulator 8 vectors of 4 int32 and it stays in registers.
//
// WHY THE TWO BODIES AGREE. The vector body is pmaddubsw followed by pmaddwd.
// pmaddubsw multiplies uint8 by int8 and adds ADJACENT PAIRS with SIGNED
// SATURATION to int16 -- so it is only equal to the scalar body if that
// intermediate cannot saturate. It cannot: affine inputs are activation outputs,
// which both nnue_clipped_relu_32 and nnue_sqr_clipped_relu_32 cap at 127, and
// weights are int8. The most positive pair sum is 127 * 127 * 2 = 32258 < 32767, and
// the most negative is 127 * -128 * 2 = -32512 > -32768. The bound holds, and it does
// not depend on the net: it holds for every representable weight. pmaddwd then
// adds adjacent int16 pairs into int32, which cannot overflow at that magnitude.
// So both bodies compute the same exact integer, and the node count cannot move.
//
// tests/test_main.c drives both bodies over the saturation boundary rather than
// taking this paragraph on trust.
// ---------------------------------------------------------------------------

NNUE_SIMD_TYPE(NnueV16u8, uint8_t, 16);
NNUE_SIMD_TYPE(NnueV16i8, int8_t, 16);
NNUE_SIMD_FAMILY(nnue_v16u8, NnueV16u8, uint8_t, 16);
NNUE_SIMD_FAMILY(nnue_v16i8, NnueV16i8, int8_t, 16);
NNUE_SIMD_TYPE(NnueV4u32, uint32_t, 4);
NNUE_SIMD_FAMILY(nnue_v4u32, NnueV4u32, uint32_t, 4);
NNUE_SIMD_REINTERPRET(nnue_v16_u32x4_as_u8, NnueV16u8, NnueV4u32)

// TIER THE WIDTH. One vocabulary, four lowerings, and the ONLY difference between
// them is how many outputs one step produces:
//
//   AVX-512 VNNI  16 outputs/step  vpdpbusd -- the whole dot4 is ONE instruction
//   AVX2           8 outputs/step  vpmaddubsw + vpmaddwd
//   SSSE3          4 outputs/step  pmaddubsw + pmaddwd
//   portable       4 outputs/step  a lane loop
//
// A tier nobody compiles rots silently, so `./build.sh arch-determinism` builds
// every tier the host can execute and requires one node count from all of them,
// and `./build.sh simd-scalar` builds the portable body alone. The four are
// value-identical for the reason argued above: no intermediate can saturate, and
// vpdpbusd accumulates in exact int32 with no intermediate at all.

#if MCFISH_SIMD_VECTOR && defined(__AVX512VNNI__) && defined(__AVX512F__)

    #include <immintrin.h>
    #define NNUE_DOT_LANES 16
typedef __m512i NnueDotAcc;
static inline NnueDotAcc nnue_dot_zero(void) { return _mm512_setzero_si512(); }
static inline NnueDotAcc nnue_dot_step(NnueDotAcc acc, uint32_t packed, const int8_t *w) {
    // vpdpbusd IS dot4: it multiplies u8 by i8 and adds the four products into each
    // 32-bit lane, in exact int32. No widening, no int16 intermediate, no saturation
    // question to answer.
    return _mm512_dpbusd_epi32(acc, _mm512_set1_epi32((int) packed),
                               _mm512_loadu_si512((const void *) w));
}
static inline int32_t nnue_dot_lane(NnueDotAcc a, size_t i) {
    int32_t v[16];
    _mm512_storeu_si512((void *) v, a);
    return v[i];
}
static inline NnueDotAcc nnue_dot_add(NnueDotAcc a, NnueDotAcc b) { return _mm512_add_epi32(a, b); }
static inline NnueDotAcc nnue_dot_load(const int32_t *p) {
    return _mm512_loadu_si512((const void *) p);
}
static inline void nnue_dot_store(int32_t *p, NnueDotAcc a) { _mm512_storeu_si512((void *) p, a); }

#elif MCFISH_SIMD_VECTOR && defined(__AVX2__)

    #include <immintrin.h>
    #define NNUE_DOT_LANES 8
typedef __m256i NnueDotAcc;
static inline NnueDotAcc nnue_dot_zero(void) { return _mm256_setzero_si256(); }
static inline NnueDotAcc nnue_dot_step(NnueDotAcc acc, uint32_t packed, const int8_t *w) {
    const __m256i pairs = _mm256_maddubs_epi16(_mm256_set1_epi32((int) packed),
                                               _mm256_loadu_si256((const __m256i *) w));
    return _mm256_add_epi32(acc, _mm256_madd_epi16(pairs, _mm256_set1_epi16(1)));
}
static inline int32_t nnue_dot_lane(NnueDotAcc a, size_t i) {
    int32_t v[8];
    _mm256_storeu_si256((__m256i *) v, a);
    return v[i];
}
static inline NnueDotAcc nnue_dot_add(NnueDotAcc a, NnueDotAcc b) { return _mm256_add_epi32(a, b); }
static inline NnueDotAcc nnue_dot_load(const int32_t *p) {
    return _mm256_loadu_si256((const __m256i *) p);
}
static inline void nnue_dot_store(int32_t *p, NnueDotAcc a) {
    _mm256_storeu_si256((__m256i *) p, a);
}

#elif MCFISH_SIMD_VECTOR && defined(__SSSE3__)

    #include <tmmintrin.h>
    #define NNUE_DOT_LANES 4
typedef __m128i NnueDotAcc;
static inline NnueDotAcc nnue_dot_zero(void) { return _mm_setzero_si128(); }
static inline NnueDotAcc nnue_dot_step(NnueDotAcc acc, uint32_t packed, const int8_t *w) {
    // Load the weight row ALIGNED: a legacy-SSE instruction can fold a memory operand
    // only when it is 16-byte aligned, so _mm_loadu_si128 here forced a separate movdqu
    // per chunk where upstream's aligned load disappears into pmaddubsw's operand
    // (affine_transform.h m128_add_dpbusd_epi32 on an alignas(CacheLineSize) weights
    // array). Every caller passes rows at 16-byte strides off nnue_layer_storage's
    // 64-byte-aligned blocks, and the header states the contract.
    const __m128i pairs =
      _mm_maddubs_epi16(_mm_set1_epi32((int) packed), _mm_load_si128((const __m128i *) w));
    return _mm_add_epi32(acc, _mm_madd_epi16(pairs, _mm_set1_epi16(1)));
}
static inline int32_t nnue_dot_lane(NnueDotAcc a, size_t i) {
    int32_t v[4];
    _mm_storeu_si128((__m128i *) v, a);
    return v[i];
}
static inline NnueDotAcc nnue_dot_add(NnueDotAcc a, NnueDotAcc b) { return _mm_add_epi32(a, b); }
static inline NnueDotAcc nnue_dot_load(const int32_t *p) {
    return _mm_loadu_si128((const __m128i *) p);
}
static inline void nnue_dot_store(int32_t *p, NnueDotAcc a) { _mm_storeu_si128((__m128i *) p, a); }

#else

    #define NNUE_DOT_LANES 4
typedef struct {
    int32_t l[4];
} NnueDotAcc;
static inline NnueDotAcc nnue_dot_zero(void) { return (NnueDotAcc) { { 0, 0, 0, 0 } }; }
static inline NnueDotAcc nnue_dot_step(NnueDotAcc acc, uint32_t packed, const int8_t *w) {
    // Reassemble the group's four bytes from the uint32 the same way the vector
    // broadcasts do, so this body is endianness-neutral with them.
    uint8_t in[4];
    for (size_t b = 0; b < 4; b++)
        in[b] = (uint8_t) (packed >> (8 * b));
    for (size_t q = 0; q < 4; q++)
        for (size_t s = 0; s < 4; s++)
            acc.l[q] += (int32_t) in[s] * (int32_t) w[q * 4 + s];
    return acc;
}
static inline int32_t nnue_dot_lane(NnueDotAcc a, size_t i) { return a.l[i]; }
static inline NnueDotAcc nnue_dot_add(NnueDotAcc a, NnueDotAcc b) {
    for (size_t i = 0; i < 4; i++)
        a.l[i] += b.l[i];
    return a;
}
static inline NnueDotAcc nnue_dot_load(const int32_t *p) {
    NnueDotAcc a;
    for (size_t i = 0; i < 4; i++)
        a.l[i] = p[i];
    return a;
}
static inline void nnue_dot_store(int32_t *p, NnueDotAcc a) {
    for (size_t i = 0; i < 4; i++)
        p[i] = a.l[i];
}

#endif

// ---------------------------------------------------------------------------
// THE CONTIGUOUS DOT. The OUT==1 layer (fc_2, 128->1) is special: its N=4 weight
// layout is the IDENTITY permutation -- group g's four sublanes are output 0 over
// inputs [4g, 4g+4), so out = sum over i in 0..127 of in[i] * w[i] with input AND
// weight laid out contiguously. That is a plain u8*i8 dot, and clang does not widen
// it: it loads four bytes at a time, sign/zero-extends each quad to int32, repacks
// to int16 and reduces through vpdpwssd -- 64 vpmov widen loads and a repack tax for
// what one vpdpbusd does 64 inputs at a stride. Give the layer its own wide dot.
//
// Bit-identity holds by the dot4 argument verbatim: pmaddubsw's int16 pair sum over
// activation outputs (u8 in [0,127]) times int8 weights stays in [-32512, 32258], so
// the int16 tiers never saturate; vpdpbusd carries no intermediate at all; and the
// portable body sums in order. Integer addition commutes, so every tier's reduction
// order lands on the same int32. n is a multiple of the tier width here (128), and
// the scalar tail closes any that is not.
#if MCFISH_SIMD_VECTOR && defined(__AVX512VNNI__) && defined(__AVX512F__)

static inline int32_t nnue_affine1_dot(const uint8_t *in, const int8_t *w, size_t n) {
    __m512i acc = _mm512_setzero_si512();
    size_t i = 0;
    for (; i + 64 <= n; i += 64)
        acc = _mm512_dpbusd_epi32(acc, _mm512_loadu_si512((const void *) (in + i)),
                                  _mm512_loadu_si512((const void *) (w + i)));
    int32_t sum = _mm512_reduce_add_epi32(acc);
    for (; i < n; i++)
        sum += (int32_t) in[i] * (int32_t) w[i];
    return sum;
}

#elif MCFISH_SIMD_VECTOR && defined(__AVX2__)

static inline int32_t nnue_affine1_dot(const uint8_t *in, const int8_t *w, size_t n) {
    __m256i acc = _mm256_setzero_si256();
    const __m256i ones = _mm256_set1_epi16(1);
    size_t i = 0;
    for (; i + 32 <= n; i += 32) {
        const __m256i pairs = _mm256_maddubs_epi16(_mm256_loadu_si256((const __m256i *) (in + i)),
                                                   _mm256_loadu_si256((const __m256i *) (w + i)));
        acc = _mm256_add_epi32(acc, _mm256_madd_epi16(pairs, ones));
    }
    __m128i s = _mm_add_epi32(_mm256_castsi256_si128(acc), _mm256_extracti128_si256(acc, 1));
    s = _mm_add_epi32(s, _mm_shuffle_epi32(s, 0x4E));
    s = _mm_add_epi32(s, _mm_shuffle_epi32(s, 0xB1));
    int32_t sum = _mm_cvtsi128_si32(s);
    for (; i < n; i++)
        sum += (int32_t) in[i] * (int32_t) w[i];
    return sum;
}

#elif MCFISH_SIMD_VECTOR && defined(__SSSE3__)

static inline int32_t nnue_affine1_dot(const uint8_t *in, const int8_t *w, size_t n) {
    __m128i acc = _mm_setzero_si128();
    const __m128i ones = _mm_set1_epi16(1);
    size_t i = 0;
    // Aligned loads for the same folding reason as nnue_dot_step: the sole caller
    // feeds a 64-byte-aligned concat buffer and 64-byte-aligned layer weights, and
    // legacy SSE folds only an aligned memory operand into pmaddubsw.
    for (; i + 16 <= n; i += 16) {
        const __m128i pairs = _mm_maddubs_epi16(_mm_load_si128((const __m128i *) (in + i)),
                                                _mm_load_si128((const __m128i *) (w + i)));
        acc = _mm_add_epi32(acc, _mm_madd_epi16(pairs, ones));
    }
    acc = _mm_add_epi32(acc, _mm_shuffle_epi32(acc, 0x4E));
    acc = _mm_add_epi32(acc, _mm_shuffle_epi32(acc, 0xB1));
    int32_t sum = _mm_cvtsi128_si32(acc);
    for (; i < n; i++)
        sum += (int32_t) in[i] * (int32_t) w[i];
    return sum;
}

#else

static inline int32_t nnue_affine1_dot(const uint8_t *in, const int8_t *w, size_t n) {
    int32_t sum = 0;
    for (size_t i = 0; i < n; i++)
        sum += (int32_t) in[i] * (int32_t) w[i];
    return sum;
}

#endif

#if defined(__GNUC__) && !defined(__clang__)
    #pragma GCC diagnostic pop
#endif

#endif  // MCFISH_NNUE_SIMD_H
