// The feature-transformer weight-row add/sub kernels: the SIMD primitives that apply
// removed/added weight rows to a raw accumulator tile. Split out of the accumulator
// because they carry NO arena knowledge -- they take raw pointers and share only
// simd.h -- so they are a clean unit with a six-function surface, unlike the arena
// accessors, which are per-TU byte-offset helpers with no independent API.
//
// Golden: upstream `nnue/nnue_feature_transformer.h` (the apply_combined tile loop).

#ifndef MCFISH_NNUE_ACC_ROWOPS_H
#define MCFISH_NNUE_ACC_ROWOPS_H

#include <stddef.h>
#include <stdint.h>

// psq-only i16 accumulation: (target - Σremoved + Σadded), one tile sweep.
void nnue_acc_apply_delta_i16(int16_t *target,
                              const uint32_t *removed,
                              size_t removed_len,
                              const uint32_t *added,
                              size_t added_len,
                              const int16_t *weights);

// int8 (threat) rows accumulated onto target, widened to i16.
void nnue_acc_accumulate_rows_i8(int16_t *target,
                                 const uint32_t *rows,
                                 size_t row_count,
                                 const int8_t *weights);

// psqt (8-bucket i32) delta and accumulation, held in one register.
void nnue_acc_apply_psqt_delta(int32_t *target,
                               const uint32_t *removed,
                               size_t removed_len,
                               const uint32_t *added,
                               size_t added_len,
                               const int32_t *weights);
void nnue_acc_accumulate_psqt_rows(int32_t *target,
                                   const uint32_t *rows,
                                   size_t row_count,
                                   const int32_t *weights);

// The fused combined step: source + Σpsq_added − Σpsq_removed + Σthr_added − Σthr_removed
// into target, one load/store per tile (psq i16 rows direct, threat i8 rows widened).
void nnue_acc_apply_combined_delta(int16_t *target,
                                   const int16_t *source,
                                   const uint32_t *psq_removed,
                                   size_t psq_removed_len,
                                   const uint32_t *psq_added,
                                   size_t psq_added_len,
                                   const uint32_t *thr_removed,
                                   size_t thr_removed_len,
                                   const uint32_t *thr_added,
                                   size_t thr_added_len,
                                   const int16_t *psq_weights,
                                   const int8_t *thr_weights);
void nnue_acc_apply_combined_psqt_delta(int32_t *target,
                                        const int32_t *source,
                                        const uint32_t *psq_removed,
                                        size_t psq_removed_len,
                                        const uint32_t *psq_added,
                                        size_t psq_added_len,
                                        const uint32_t *thr_removed,
                                        size_t thr_removed_len,
                                        const uint32_t *thr_added,
                                        size_t thr_added_len,
                                        const int32_t *psq_weights,
                                        const int32_t *thr_weights);

#endif  // MCFISH_NNUE_ACC_ROWOPS_H
