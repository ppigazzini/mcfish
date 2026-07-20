#!/usr/bin/env bash
# Compile tools/perf_sample.c once (cached) and run it. See that file's header for what it
# does: a `perf record`-style CYCLE profiler over perf_event_open that attributes wall-clock
# cost to symbols on EVERY arch tier -- the tool for an IPC gap, where perf_callgrind.sh only
# counts instructions (and SIGILLs on avx512) and perf_counters.sh gives only aggregates.
#
# Usage (CWD must hold the net: mcfish -> resources/):
#   ../tools/perf_sample.sh <binary> <period_cycles> [bench-args...]
#   # one long single-position search (keeps ONE worker alive -> clean search attribution):
#   PS_UCI='uci\nposition fen ...\ngo movetime 20000' ../tools/perf_sample.sh ../build/mcfish 40000
#   # drill into a hot symbol by instruction offset (map with objdump -d):
#   PS_FOCUS=evaluate_side.75 PS_UCI='...' ../tools/perf_sample.sh ../build/mcfish 30000
set -u

SELF_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
SRC="$SELF_DIR/perf_sample.c"
BIN="${TMPDIR:-/tmp}/mcfish_perf_sample"

CC="${CC:-clang}"
command -v "$CC" >/dev/null || { echo "error: $CC not installed" >&2; exit 1; }
if [ ! -x "$BIN" ] || [ "$SRC" -nt "$BIN" ]; then
    "$CC" -O2 -std=c23 -o "$BIN" "$SRC" || { echo "error: failed to build $SRC" >&2; exit 1; }
fi
exec "$BIN" "$@"
