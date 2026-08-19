// The legal form: side-to-move, then file.
#include "platform/syzygy/registry.h"
PairsData *right(TBTable *t, TbStm stm, TbFile f);
PairsData *right(TBTable *t, TbStm stm, TbFile f) { return tbtable_get(t, true, stm, f); }
