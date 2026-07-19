#include "correction_bundle.h"


// Index the bundle through its declared fields rather than by casting it to an array:
// the four entries are contiguous by the static_assert in the header, but naming them
// keeps the mapping visible at the one place a reorder would break it.
int16_t *correction_bundle_field(CorrectionBundle *bundle, CorrectionField field) {
    switch (field) {
    case CORR_FIELD_PAWN :
        return &bundle->pawn;
    case CORR_FIELD_MINOR :
        return &bundle->minor;
    case CORR_FIELD_NONPAWN_WHITE :
        return &bundle->nonpawn_white;
    case CORR_FIELD_NONPAWN_BLACK :
    case CORR_FIELD_NB :
    default :
        return &bundle->nonpawn_black;
    }
}

const int16_t *correction_bundle_field_const(const CorrectionBundle *bundle,
                                             CorrectionField field) {
    return correction_bundle_field((CorrectionBundle *) bundle, field);
}

int16_t *correction_bundle_nonpawn(CorrectionBundle *bundle, Color c) {
    return c == WHITE ? &bundle->nonpawn_white : &bundle->nonpawn_black;
}

void correction_bundle_clear(CorrectionBundle *bundle) {
    bundle->pawn = CORRECTION_HISTORY_FILL;
    bundle->minor = CORRECTION_HISTORY_FILL;
    bundle->nonpawn_white = CORRECTION_HISTORY_FILL;
    bundle->nonpawn_black = CORRECTION_HISTORY_FILL;
}

void correction_bundle_page_clear(CorrectionBundle page[COLOR_NB]) {
    for (size_t c = 0; c < COLOR_NB; ++c)
        correction_bundle_clear(&page[c]);
}

void correction_bundle_clear_range(CorrectionBundle (*pages)[COLOR_NB], size_t count) {
    // Fill entry by entry rather than memset: the cleared value is -6, not 0, so a byte
    // fill would only be correct if the value happened to be zero -- and the search reads
    // this table on the very first node of the first search after a clear.
    int16_t *entries = (int16_t *) &pages[0][0];
    const size_t total = count * (size_t) COLOR_NB * (size_t) CORR_FIELD_NB;
    for (size_t i = 0; i < total; ++i)
        entries[i] = CORRECTION_HISTORY_FILL;
}
