// Own the .nnue WRITE primitives: the byte sink and the little-endian scalars
// `export_net` emits.
//
// This is the mirror of nnue_parse.h, and the mirroring is the hazard: a read order
// and a write order are two statements of ONE file format, forty lines apart, with
// nothing in the source relating them. Neither the bench anchor nor the eval gates
// can see a writer drift away from its reader -- the writer is not on the eval path
// at all -- so `./build.sh net-roundtrip` is what holds them together, by exporting
// the shipped net and comparing it byte for byte with the file on disk.
//
// Golden: src/nnue/nnue_common.h (write_little_endian),
// src/nnue/network.cpp (write_header, write_parameters).

#ifndef MCFISH_NNUE_WRITE_H
#define MCFISH_NNUE_WRITE_H

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

// Carry the output stream and a STICKY failure flag. Every call below is a no-op
// once `ok` is false, so a caller writes the whole file and tests once at the end,
// as upstream's stream state does.
typedef struct {
    FILE *out;
    bool ok;
} NnueWriter;

void nnue_write_bytes(NnueWriter *w, const void *data, size_t len);
void nnue_write_u32_le(NnueWriter *w, uint32_t value);

// Write COUNT values little-endian, as `write_little_endian` does for the transformer's
// int16 biases and an affine layer's int32 ones. The storage holds them in HOST order,
// so this is a conversion, not a copy.
void nnue_write_i16_le(NnueWriter *w, const int16_t *values, size_t count);
void nnue_write_i32_le(NnueWriter *w, const int32_t *values, size_t count);

#endif  // MCFISH_NNUE_WRITE_H
