#include "correction_bundle.h"

#include <string.h>

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
    bundle->pawn = 0;
    bundle->minor = 0;
    bundle->nonpawn_white = 0;
    bundle->nonpawn_black = 0;
}

void correction_bundle_page_clear(CorrectionBundle page[COLOR_NB]) {
    for (size_t c = 0; c < COLOR_NB; ++c)
        correction_bundle_clear(&page[c]);
}

void correction_bundle_clear_range(CorrectionBundle (*pages)[COLOR_NB], size_t count) {
    if (count == 0)
        return;
    memset(pages, 0, count * sizeof pages[0]);
}
