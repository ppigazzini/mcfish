// Own the vector vocabulary the NNUE kernels compute through: fixed-width integer
// lane vectors plus the element-wise operations over them.
//
// Provide ONE vocabulary in TWO implementations. `CCFISH_SIMD_VECTOR` (the default
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
//   - WRAPPING. The int16_t accumulator must wrap on overflow (upstream relies on it;
//     zfish spells it with Zig's element-wise vector `+`). C signed overflow is UB and
//     GCC leaves signed vector overflow undefined too, so the accumulator family is
//     uint16_t throughout and the int16_t view is reached by a reinterpret. C23 pins
//     the conversion back to two's complement.
//   - RIGHT SHIFT. C23 defines `>>` on a negative signed value as an arithmetic shift,
//     which is what Zig's `>>` and both compilers' vector `>>` already do. Shifts here
//     are therefore written directly.
//
// Ported from zfish `engine/eval/nnue_acc_rowops.zig` (portable `@Vector`) against the
// upstream golden `nnue/simd.h`.

#ifndef CCFISH_NNUE_SIMD_H
#define CCFISH_NNUE_SIMD_H

#include <stddef.h>
#include <stdint.h>

#if !defined(CCFISH_SIMD_SCALAR) && (defined(__GNUC__) || defined(__clang__))
    #define CCFISH_SIMD_VECTOR 1
#else
    #define CCFISH_SIMD_VECTOR 0
#endif

// ---------------------------------------------------------------------------
// The family generator. Emits, for one (element type, lane count) pair, the whole
// element-wise op set. Unused members are `static inline` and cost nothing.
// ---------------------------------------------------------------------------

#if CCFISH_SIMD_VECTOR

    #define NNUE_SIMD_TYPE(Type, Elem, Width) \
        typedef Elem Type __attribute__((vector_size((Width) * sizeof(Elem))))

    // clang-format off
#  define NNUE_SIMD_FAMILY(Pfx, Type, Elem, Width)                                          \
      static inline Type Pfx##_load(const Elem *p) {                                        \
          Type v;                                                                           \
          __builtin_memcpy(&v, p, sizeof v);                                                \
          return v;                                                                         \
      }                                                                                     \
      static inline void Pfx##_store(Elem *p, Type v) { __builtin_memcpy(p, &v, sizeof v); } \
      static inline Type Pfx##_splat(Elem x) {                                              \
          Type z = { 0 };                                                                   \
          return z + x;                                                                     \
      }                                                                                     \
      static inline Type Pfx##_add(Type a, Type b) { return a + b; }                        \
      static inline Type Pfx##_sub(Type a, Type b) { return a - b; }                        \
      static inline Type Pfx##_mul(Type a, Type b) { return a * b; }                        \
      static inline Type Pfx##_shl(Type a, int s) { return a << s; }                        \
      static inline Type Pfx##_shr(Type a, int s) { return a >> s; }                        \
      static inline Type Pfx##_min(Type a, Type b) {                                        \
          Type m = (Type) (a < b);                                                          \
          return (Type) ((a & m) | (b & ~m));                                               \
      }                                                                                     \
      static inline Type Pfx##_max(Type a, Type b) {                                        \
          Type m = (Type) (a > b);                                                          \
          return (Type) ((a & m) | (b & ~m));                                               \
      }                                                                                     \
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

#endif  // CCFISH_SIMD_VECTOR

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

// Feature-transformer output tile: 32 lanes through int16 -> uint16 -> uint32 -> uint8.
NNUE_SIMD_TYPE(NnueV32i16, int16_t, 32);
NNUE_SIMD_TYPE(NnueV32u16, uint16_t, 32);
NNUE_SIMD_TYPE(NnueV32u32, uint32_t, 32);
NNUE_SIMD_TYPE(NnueV32u8, uint8_t, 32);
NNUE_SIMD_TYPE(NnueV8u32, uint32_t, 8);
NNUE_SIMD_FAMILY(nnue_v32i16, NnueV32i16, int16_t, 32);
NNUE_SIMD_FAMILY(nnue_v32u16, NnueV32u16, uint16_t, 32);
NNUE_SIMD_FAMILY(nnue_v32u32, NnueV32u32, uint32_t, 32);
NNUE_SIMD_FAMILY(nnue_v32u8, NnueV32u8, uint8_t, 32);
NNUE_SIMD_FAMILY(nnue_v8u32, NnueV8u32, uint32_t, 8);
NNUE_SIMD_CONVERT(nnue_v32_i16_to_u16, NnueV32u16, NnueV32i16)
NNUE_SIMD_CONVERT(nnue_v32_u16_to_u32, NnueV32u32, NnueV32u16)
NNUE_SIMD_CONVERT(nnue_v32_u32_to_u16, NnueV32u16, NnueV32u32)
NNUE_SIMD_CONVERT(nnue_v32_u16_to_u8, NnueV32u8, NnueV32u16)
NNUE_SIMD_REINTERPRET(nnue_v32_u8_as_u32x8, NnueV8u32, NnueV32u8)

// Affine post-activation tile: 8 int32_t lanes.
NNUE_SIMD_TYPE(NnueV8i32, int32_t, 8);
NNUE_SIMD_FAMILY(nnue_v8i32, NnueV8i32, int32_t, 8);

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
NNUE_SIMD_REINTERPRET(nnue_v128_u32x32_as_u8, NnueV128u8, NnueV32u32)

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
// weights are int8. The largest pair sum is 127 * 128 * 2 = 32512 < 32767, and the
// smallest is -32512 > -32768. The bound is strict with 255 to spare, and it does
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

#if CCFISH_SIMD_VECTOR && defined(__SSSE3__)

    #include <tmmintrin.h>

static inline NnueV4i32 nnue_dot4_i32(NnueV16u8 in, NnueV16i8 w) {
    const __m128i pairs = _mm_maddubs_epi16((__m128i) in, (__m128i) w);
    return (NnueV4i32) _mm_madd_epi16(pairs, _mm_set1_epi16(1));
}

#else

static inline NnueV4i32 nnue_dot4_i32(NnueV16u8 in, NnueV16i8 w) {
    int32_t r[4];
    for (size_t q = 0; q < 4; q++) {
        int32_t acc = 0;
        for (size_t s = 0; s < 4; s++) {
            acc +=
              (int32_t) nnue_v16u8_lane(in, q * 4 + s) * (int32_t) nnue_v16i8_lane(w, q * 4 + s);
        }
        r[q] = acc;
    }
    return nnue_v4i32_load(r);
}

#endif

#if defined(__GNUC__) && !defined(__clang__)
    #pragma GCC diagnostic pop
#endif

#endif  // CCFISH_NNUE_SIMD_H
