#include "nnue_write.h"

#include "nnue_common.h"

#include <string.h>

void nnue_write_bytes(NnueWriter *w, const void *data, size_t len) {
    if (!w->ok || len == 0)
        return;
    if (fwrite(data, 1, len, w->out) != len)
        w->ok = false;
}

void nnue_write_u32_le(NnueWriter *w, uint32_t value) {
    const uint8_t bytes[4] = {
        (uint8_t) (value & 0xffu),
        (uint8_t) ((value >> 8) & 0xffu),
        (uint8_t) ((value >> 16) & 0xffu),
        (uint8_t) ((value >> 24) & 0xffu),
    };
    nnue_write_bytes(w, bytes, sizeof bytes);
}

void nnue_write_i32_le(NnueWriter *w, const int32_t *values, size_t count) {
    for (size_t i = 0; i < count && w->ok; ++i)
        nnue_write_u32_le(w, (uint32_t) values[i]);
}

// Encode one value, mirroring write_leb_128's loop exactly (nnue_common.h:261-318).
//
// The termination test is the LEB128 sign rule and not an optimisation: a group whose
// bit 0x40 is clear ends when the remaining value is 0, and one whose 0x40 is set ends
// when it is -1, so a negative number's sign is carried by the last group rather than
// by a leading run of 0xff. `>>` on a negative int32_t is an arithmetic shift under
// clang and gcc, which is the shift upstream's `value >>= 7` performs on its own signed
// type; the encoding is wrong under a logical shift, so this is load-bearing rather
// than incidental.
//
// Return the byte count and, when BUF is non-null, write the bytes there. One function
// for both passes: a size pass that walked a different loop than the emit pass is the
// drift this file exists to prevent, in miniature.
static inline size_t encode_leb(int32_t value, uint8_t *buf) {
    size_t n = 0;
    for (;;) {
        const uint8_t byte = (uint8_t) (value & 0x7f);
        value >>= 7;
        const bool last = (byte & 0x40) == 0 ? value == 0 : value == -1;
        if (buf != nullptr)
            buf[n] = last ? byte : (uint8_t) (byte | 0x80);
        ++n;
        if (last)
            return n;
    }
}

size_t nnue_leb_bytes_i16(const int16_t *values, size_t count) {
    size_t bytes = 0;
    for (size_t i = 0; i < count; ++i)
        bytes += encode_leb(values[i], nullptr);
    return bytes;
}

size_t nnue_leb_bytes_i32(const int32_t *values, size_t count) {
    size_t bytes = 0;
    for (size_t i = 0; i < count; ++i)
        bytes += encode_leb(values[i], nullptr);
    return bytes;
}

void nnue_write_leb_header(NnueWriter *w, uint32_t byte_count) {
    nnue_write_bytes(w, NNUE_LEB128_MAGIC, NNUE_LEB128_MAGIC_SIZE);
    nnue_write_u32_le(w, byte_count);
}

// Buffer the encoded bytes as upstream does (its BUF_SIZE is 4096): a value encodes to
// at most five bytes, so a flush whenever fewer than five slots remain cannot overrun.
enum { LEB_BUF_SIZE = 4096, LEB_MAX_BYTES = 5 };

typedef struct {
    uint8_t bytes[LEB_BUF_SIZE];
    size_t used;
} LebBuffer;

static void leb_flush(NnueWriter *w, LebBuffer *buf) {
    nnue_write_bytes(w, buf->bytes, buf->used);
    buf->used = 0;
}

static void leb_append(NnueWriter *w, LebBuffer *buf, int32_t value) {
    if (buf->used + LEB_MAX_BYTES > LEB_BUF_SIZE)
        leb_flush(w, buf);
    buf->used += encode_leb(value, buf->bytes + buf->used);
}

void nnue_write_leb_i16(NnueWriter *w, const int16_t *values, size_t count) {
    LebBuffer buf = { .used = 0 };
    for (size_t i = 0; i < count && w->ok; ++i)
        leb_append(w, &buf, values[i]);
    leb_flush(w, &buf);
}

void nnue_write_leb_i32(NnueWriter *w, const int32_t *values, size_t count) {
    LebBuffer buf = { .used = 0 };
    for (size_t i = 0; i < count && w->ok; ++i)
        leb_append(w, &buf, values[i]);
    leb_flush(w, &buf);
}
