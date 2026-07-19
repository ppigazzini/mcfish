// Own the ordered view over the four correction entries stored per (key, color) slot.
//
// `CorrectionBundle` itself is history.h's -- this header does not redefine it. What it
// adds is the INDEX: upstream reaches the four tables by name (pawn, minor, non-pawn
// white, non-pawn black) while the correction update walks them in a fixed order, and
// that order is what decides which key indexes which entry. Naming it once here keeps a
// reorder from silently swapping two tables, which costs strength without failing a gate.
//
// Upstream: history.h (CorrectionHistory), search.cpp (update_correction_history,
// correction_value). Port source: zfish src/engine/state/correction_bundle.zig.

#ifndef CCFISH_CORRECTION_BUNDLE_H
#define CCFISH_CORRECTION_BUNDLE_H

#include "../board/types.h"
#include "../search/history.h"

#include <stddef.h>
#include <stdint.h>

// Order the four fields as the correction update walks them.
typedef enum {
    CORR_FIELD_PAWN = 0,
    CORR_FIELD_MINOR = 1,
    CORR_FIELD_NONPAWN_WHITE = 2,
    CORR_FIELD_NONPAWN_BLACK = 3,
    CORR_FIELD_NB = 4,
} CorrectionField;

static_assert(sizeof(CorrectionBundle) == CORR_FIELD_NB * sizeof(int16_t),
              "CorrectionBundle must stay four contiguous int16 entries");

// Return the entry FIELD names. FIELD must be below CORR_FIELD_NB.
int16_t *correction_bundle_field(CorrectionBundle *bundle, CorrectionField field);
const int16_t *correction_bundle_field_const(const CorrectionBundle *bundle, CorrectionField field);

// Return the non-pawn entry for C, so the caller indexes by color rather than by name.
int16_t *correction_bundle_nonpawn(CorrectionBundle *bundle, Color c);

// Reset every entry of one bundle, or of one [COLOR_NB] page, to CORRECTION_HISTORY_FILL.
void correction_bundle_clear(CorrectionBundle *bundle);
void correction_bundle_page_clear(CorrectionBundle page[COLOR_NB]);

// Reset COUNT consecutive [COLOR_NB] pages, the unit the shared correction table is
// cleared in.
void correction_bundle_clear_range(CorrectionBundle (*pages)[COLOR_NB], size_t count);

#endif  // CCFISH_CORRECTION_BUNDLE_H
