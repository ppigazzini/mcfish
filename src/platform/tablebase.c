#include "tablebase.h"

#include "syzygy/probe.h"
#include "syzygy/registry.h"

void tablebase_init(const char *path, size_t path_len) { registry_init(path, path_len); }

size_t tablebase_max_cardinality(void) { return registry_max_cardinality(); }

size_t tablebase_found_wdl(void) { return registry_found_wdl(); }

size_t tablebase_found_dtz(void) { return registry_found_dtz(); }

TbProbeResult tablebase_probe_fen(const char *fen, size_t len, bool chess960) {
    return syzygy_probe_fen(fen, len, chess960);
}

TbProbeResult tablebase_probe_wdl_pos(Position *pos) { return syzygy_probe_wdl_pos(pos); }
