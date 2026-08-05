// Own the .nnue WRITE primitives: the byte sink, the little-endian scalars and the
// LEB128 sections `export_net` emits.
//
// This is the mirror of nnue_parse.h, and the mirroring is the hazard: a read order
// and a write order are two statements of ONE file format, forty lines apart, with
// nothing in the source relating them. Neither the bench anchor nor the eval gates
// can see a writer drift away from its reader -- the writer is not on the eval path
// at all -- so `./build.sh net-roundtrip` is what holds them together, by exporting
// the shipped net and comparing it byte for byte with the file on disk.
//
// A LEB128 section states its byte count BEFORE its bytes, so every caller measures
// the encoding first and emits it second. That is why the size and the emit are two
// entry points over the same encoder rather than one buffered call: the largest
// section here is 46 MB of int16 and buffering it whole to learn its length would
// cost more memory than the resident net.
//
// Golden: src/nnue/nnue_common.h (write_leb_128, write_little_endian),
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

// Write COUNT int32 values little-endian, as `write_little_endian<BiasType>` does for
// an affine layer's biases. The storage holds them in HOST order, so this is a
// conversion, not a copy.
void nnue_write_i32_le(NnueWriter *w, const int32_t *values, size_t count);

// Measure the signed-LEB128 encoding of COUNT values, in bytes. Sum these over every
// span of a section before opening it.
size_t nnue_leb_bytes_i16(const int16_t *values, size_t count);
size_t nnue_leb_bytes_i32(const int32_t *values, size_t count);

// Open a COMPRESSED_LEB128 section: the magic string, then the total byte count.
void nnue_write_leb_header(NnueWriter *w, uint32_t byte_count);

// Append COUNT values to the open section.
void nnue_write_leb_i16(NnueWriter *w, const int16_t *values, size_t count);
void nnue_write_leb_i32(NnueWriter *w, const int32_t *values, size_t count);

#endif  // MCFISH_NNUE_WRITE_H
