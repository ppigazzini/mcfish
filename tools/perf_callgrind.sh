#!/usr/bin/env bash
# Deterministic cost measurement for one engine binary.
#
# Runs callgrind over a bench and prints instructions, data refs and D1/LL misses. Needs no
# instrumentation: it measures the shipped release artifact, so mcfish and an upstream binary
# are directly comparable when handed the same bench (same node count => same tree => same
# workload).
#
# WHY THIS AND NOT nps, for anything under ~5%: callgrind counts are DETERMINISTIC. Wall-clock
# on this class of hardware swings by double-digit percent across interleaved rounds -- enough
# to falsely confirm and falsely refute real changes. If a hypothesis is worth less than a few
# percent, nps CANNOT settle it and callgrind can (it resolves 0.01%). Use nps only for the
# headline ratio, and only via tools/nps_ab.sh.
#
# Instruction counts alone do not predict time, so D refs and misses are printed alongside: a
# gap in time with no gap in Ir is a memory-traffic or IPC gap, not extra work. mcfish has
# measured 3.15x upstream's I1 misses while running FEWER total instructions -- reading Ir
# alone would have missed that entirely.
#
# ARCH MUST MATCH ACROSS THE BINARIES BEING COMPARED, and callgrind SIGILLs above the tier it
# understands -- build every side at x86-64-sse41-popcnt (mcfish: MCFISH_ARCH=sse41, the
# default). Comparing a native build against an SSE4.1 one measures the ISA, not the code.
#
# SUBTRACT STARTUP BEFORE QUOTING A SEARCH RATIO. On a shallow bench the net load, magic init
# and the zero-fill are ~37% of the profile here, and they are CHEAPER in mcfish than upstream
# -- so the whole-process ratio reads 0.987x ("mcfish is ahead") where the search-only ratio is
# 1.19x (mcfish is behind). Profile `printf 'quit\n' | <bin>` for a startup-only figure and
# subtract it, or name the offenders with perf_fingerprint.py costs.
#
# Usage: perf_callgrind.sh <binary> [bench-args...]   (CWD must hold the net: mcfish -> resources/)
#        OUT=path/to.out perf_callgrind.sh ./mcfish 16 1 8
set -u

BIN="${1:?usage: perf_callgrind.sh <binary> [bench-args...]  (run from the dir holding the net: mcfish -> resources/)}"
shift
BENCH_ARGS=("${@:-16 1 8}")
OUT="${OUT:-callgrind.out}"

command -v valgrind >/dev/null || { echo "error: valgrind not installed" >&2; exit 1; }
[ -x "$BIN" ] || { echo "error: $BIN is not executable" >&2; exit 1; }

echo "# callgrind: $BIN bench ${BENCH_ARGS[*]}  -> $OUT"
valgrind --tool=callgrind --callgrind-out-file="$OUT" --cache-sim=yes --branch-sim=yes \
  "$BIN" bench ${BENCH_ARGS[*]} 2>&1 |
  grep -E "Nodes searched|I   refs|I1  misses|D   refs|D1  misses|LLd misses|D1  miss rate|Branches|Mispredicts"

echo
echo "# node count above MUST match the other engine's, or the trees differ and every"
echo "# comparison below is void. Per-function breakdown:"
echo "#   tools/perf_fingerprint.py costs $OUT"
echo "#   tools/perf_fingerprint.py compare $OUT <other.out> --group name=REGEX --calls"
