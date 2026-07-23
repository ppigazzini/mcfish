#!/usr/bin/env bash
# Compile tools/perf_counters.c once (cached) and run it. See that file's header for the
# protocol; it is the only tool here that measures instructions on EVERY arch tier -- avx2 and
# native/vnni512 included -- where perf_callgrind.sh (valgrind) SIGILLs on the avx512 EVEX
# prefix. Uses perf_event_open directly, so the absent `perf` CLI does not matter.
#
# Record results under the CONCRETE tier name, never "native": on this class of host
# `-march=native` is x86-64-vnni512 (`./build.sh perf-budget` prints the resolved
# label). A standing filed under "native" stops meaning anything when the host changes.
#
# Usage (CWD must hold the net: mcfish -> resources/):
#   ../tools/perf_counters.sh <binA> <binB> <rounds> [bench-args...]
#   ../tools/perf_counters.sh ../build/mcfish "$ORACLE"/src/stockfish 8 bench 16 1 13
#   MAX_INSTR_RATIO=1.36 ../tools/perf_counters.sh ../build/mcfish "$ORACLE"/src/stockfish 8 bench 16 1 13
set -u

SELF_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
SRC="$SELF_DIR/perf_counters.c"
BIN="${TMPDIR:-/tmp}/mcfish_perf_counters"

CC="${CC:-clang}"
command -v "$CC" >/dev/null || { echo "error: $CC not installed" >&2; exit 1; }

# Rebuild only when the source is newer than the cached binary.
if [ ! -x "$BIN" ] || [ "$SRC" -nt "$BIN" ]; then
    "$CC" -O2 -std=c23 -o "$BIN" "$SRC" || { echo "error: failed to build $SRC" >&2; exit 1; }
fi

exec "$BIN" "$@"
