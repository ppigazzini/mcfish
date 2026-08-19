// "A raw narrowing integer entering a domain type without a visible cast: a board
// file assigned straight to a `TbFile`." edge_distance folds eight board files onto
// four table files, so an unconverted board file is a WRONG file, not a wide one.
// REQUIRES: -Werror=implicit-int-conversion
#include "platform/syzygy/encode.h"
TbFile wrong(unsigned board_file);
TbFile wrong(unsigned board_file) {
    TbFile f = board_file;
    return f;
}
