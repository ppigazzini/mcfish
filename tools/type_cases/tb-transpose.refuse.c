// "The Syzygy side-to-move and the table file, transposed at the DTZ read." Both are
// small unsigned enums over the same range, so only their being DISTINCT TYPES stops
// this; an index computed one off there returns a confident wrong verdict.
// REQUIRES: -Werror=enum-conversion
#include "platform/syzygy/registry.h"
PairsData *wrong(TBTable *t, TbStm stm, TbFile f);
PairsData *wrong(TBTable *t, TbStm stm, TbFile f) { return tbtable_get(t, true, f, stm); }
