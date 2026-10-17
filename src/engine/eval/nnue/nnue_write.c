#include "nnue_write.h"

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

void nnue_write_i16_le(NnueWriter *w, const int16_t *values, size_t count) {
    for (size_t i = 0; i < count && w->ok; ++i) {
        const uint16_t value = (uint16_t) values[i];
        const uint8_t bytes[2] = { (uint8_t) (value & 0xffu), (uint8_t) (value >> 8) };
        nnue_write_bytes(w, bytes, sizeof bytes);
    }
}

void nnue_write_i32_le(NnueWriter *w, const int32_t *values, size_t count) {
    for (size_t i = 0; i < count && w->ok; ++i)
        nnue_write_u32_le(w, (uint32_t) values[i]);
}
