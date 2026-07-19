#!/usr/bin/env bash
# Build and gate mcfish. One clang invocation per translation unit, no build
# system: the source set is small and fully enumerated below, so a Makefile would
# only add a dependency-tracking layer that the `clean && build` cycle already
# covers. Steps mirror the gate battery — run `./build.sh help` for the list.
set -euo pipefail

cd "$(dirname "$0")"

BIN=${BIN:-build/mcfish}
CC=${CC:-clang}

# The repository root, absolute: `engine` below runs from resources/, and a relative
# $BIN or case-file path would not survive that change of directory.
ROOT=$PWD

# The external runtime inputs: the NNUE net, the Syzygy tablebases under syzygy/,
# and an opening book if one is ever added. Fetched, optional and gitignored --
# the engine consumes them, which is the line between this directory and build/.
# `./build.sh net` and `./build.sh tb-fetch` fill it.
RESOURCES_DIR=resources

# Run the engine with resources/ as its working directory, for every run step.
#
# The engine searches upstream's three candidates and no others: <internal>,
# then the working directory, then the binary's own directory
# (src/engine/eval/nnue/network.c). Keep it that way -- a fourth candidate is a
# behaviour difference from upstream in a shipped engine. The gates come to the
# files instead.
#
# Every gate that RUNS the engine goes through here. The build steps do not:
# they write to build/ and must stay at the root.
# `engine_at` takes the binary, for the gates that build their own (simd-scalar,
# arch-determinism); `engine` is the common case. EVERY step that runs an engine
# must use one of them, or that step silently benches the classical fallback.
engine_at() { local b=$1; shift; ( cd "$ROOT/$RESOURCES_DIR" && "$ROOT/$b" "$@" ); }
engine() { engine_at "$BIN" "$@"; }

# Select the C23 flag this compiler spells. GCC only learned `-std=c23` in 14;
# 13 accepts the same language as `-std=c2x`. Probe rather than pin a compiler
# version, so the second-compiler portability check works on stock toolchains.
# Never fall back to a pre-C23 mode: the code uses `nullptr` and enums with a
# fixed underlying type, which older modes silently accept as extensions with
# different diagnostics.
detect_std_flag() {
  local f
  for f in -std=c23 -std=c2x; do
    if echo 'int main(void){return 0;}' \
       | "$CC" "$f" -x c - -o /dev/null > /dev/null 2>&1; then
      echo "$f"
      return 0
    fi
  done
  printf 'error: %s supports neither -std=c23 nor -std=c2x; a C23 compiler is required\n' \
    "$CC" >&2
  exit 2
}

STD_FLAG=$(detect_std_flag)

CFLAGS_COMMON=(
  "$STD_FLAG"
  -Wall -Wextra -Wshadow -Wstrict-prototypes -Wmissing-prototypes
  -Wconversion -Wsign-conversion -Wno-unused-parameter
  -Isrc
  -D_POSIX_C_SOURCE=200809L
)

# Match the ISA baseline upstream builds its reference binary with
# (ARCH=x86-64-sse41-popcnt). Without these, `__builtin_popcountll` cannot use the
# POPCNT instruction and the NNUE vector extensions in nnue/simd.h lower to SSE2
# only -- which costs several times the throughput and makes an nps comparison
# against upstream meaningless. Keep this in step with the oracle's ARCH, or the
# differential measures the compiler rather than the engine.
# ARCH is a knob, not a constant. Two different questions need two different
# answers, and conflating them invalidates the measurement:
#
#   x86-64-sse41-popcnt  matches the oracle's ARCH, so an instruction or nps
#                        differential against UPSTREAM compares code, not ISA.
#   native               what the engine should actually ship as, and the only
#                        honest basis for comparing against a natively-built port.
#
# Getting this wrong is not a small error. A native build on this host is
# x86-64-avx512icl with VNNI -- a single vpdpbusd does the whole u8xi8 dot product
# that the SSE4.1 path needs pmaddubsw + pmaddwd + paddd for. An nps number taken
# with SSE4.1 on one side and AVX-512 on the other measures the tier and not the
# code: comparing a native AVX-512 binary against the SSE4.1 oracle measures the
# ARCH. Both sides must be built at the same ARCH before any nps number means
# anything.
#
# The node count must not move across tiers -- the evaluation is integer-exact, so
# it is arch-invariant by construction. `./build.sh arch-determinism` is what checks
# that claim instead of trusting it.
MCFISH_ARCH=${MCFISH_ARCH:-sse41}
case "$MCFISH_ARCH" in
  sse41)  CFLAGS_ARCH=(-msse -msse2 -msse3 -mssse3 -msse4.1 -mpopcnt) ;;
  avx2)   CFLAGS_ARCH=(-mavx2 -mbmi -mbmi2 -mpopcnt) ;;
  native) CFLAGS_ARCH=(-march=native) ;;
  *)      red "unknown MCFISH_ARCH: $MCFISH_ARCH (want sse41, avx2 or native)"; exit 2 ;;
esac

# -flto is load-bearing, not a default worth having by habit. The NNUE kernels sit
# in their own translation units, so without it nnue_full_append_changed and
# nnue_bb_pieces_of_exact cannot be inlined AT ALL, and the affine's `sparse` and
# `OUT` never constant-fold. Measured on the search, startup subtracted: 2.387e9
# instructions to 2.218e9, taking the ratio against a clang-built upstream oracle
# at the same ISA from 1.242x to 1.154x.
CFLAGS_RELEASE=(-O3 -DNDEBUG -fno-math-errno -flto "${CFLAGS_ARCH[@]}")

# -fno-sanitize-recover is load-bearing: without it UBSan PRINTS a diagnostic and
# then continues, so the process still exits 0 and CI reports a green run over a
# real finding. Make undefined behaviour abort.
CFLAGS_DEBUG=(
  -O1 -g
  -fsanitize=address,undefined
  -fno-sanitize-recover=undefined
  -fno-omit-frame-pointer
)

SOURCES=(
  src/engine/board/bitboard.c
  src/engine/board/attacks.c
  src/engine/board/repetition.c
  src/engine/board/threats.c
  src/engine/board/zobrist.c
  src/engine/board/state_list.c
  src/engine/board/legality.c
  src/engine/board/fen.c
  src/engine/board/position.c
  src/engine/board/board_props.c
  src/engine/board/score.c
  src/engine/board/movegen.c
  src/engine/board/uci_move.c
  src/engine/eval/evaluate.c
  src/engine/eval/nnue/nnue_hash.c
  src/engine/eval/nnue/nnue_weight_storage.c
  src/engine/eval/nnue/nnue_parse.c
  src/engine/eval/nnue/nnue_ft.c
  src/engine/eval/nnue/nnue_feature.c
  src/engine/eval/nnue/nnue_feature_bb.c
  src/engine/eval/nnue/nnue_accumulator.c
  src/engine/eval/nnue/nnue_affine.c
  src/engine/eval/nnue/nnue_inference.c
  src/engine/eval/nnue/network.c
  src/engine/search/search.c
  src/engine/search/search_common.c
  src/engine/search/search_setup.c
  src/engine/search/search_id.c
  src/engine/search/search_main.c
  src/engine/search/search_back.c
  src/engine/search/search_qsearch.c
  src/engine/search/search_control.c
  src/engine/search/search_emit.c
  src/engine/search/syzygy_pv.c
  src/engine/search/root_move_build.c
  src/engine/search/uci_wdl.c
  src/engine/search/movepick.c
  src/engine/search/history.c
  src/engine/search/timeman.c
  src/engine/search/tt.c
  src/engine/state/correction_bundle.c
  src/engine/state/position_storage.c
  src/engine/state/root_move.c
  src/engine/state/shared_state.c
  src/engine/state/worker_histories.c
  src/engine/state/worker_construct.c
  src/engine/state/worker_layout.c
  src/platform/clock.c
  src/platform/memory.c
  src/platform/thread_runtime.c
  src/platform/thread.c
  src/platform/numa.c
  src/platform/thread_pool.c
  src/platform/tablebase.c
  src/platform/syzygy/tables.c
  src/platform/syzygy/registry.c
  src/platform/syzygy/probe.c
  src/platform/syzygy/encode.c
  src/platform/syzygy/decode.c
  src/platform/syzygy/wdl.c
  src/shell/bench_positions.c
  src/shell/benchmark.c
  src/shell/ucioption.c
  src/shell/syzygy_option.c
  src/shell/uci.c
  src/shell/main.c
)

# The engine zone must link without the shell: this is the subset `zone-check`
# builds standalone to prove no engine/ file reaches into shell/ or platform/
# beyond the declared seams.
ENGINE_SOURCES=(
  src/engine/board/bitboard.c
  src/engine/board/attacks.c
  src/engine/board/repetition.c
  src/engine/board/threats.c
  src/engine/board/zobrist.c
  src/engine/board/state_list.c
  src/engine/board/legality.c
  src/engine/board/fen.c
  src/engine/board/position.c
  src/engine/board/board_props.c
  src/engine/board/score.c
  src/engine/board/movegen.c
  src/engine/board/uci_move.c
  src/engine/eval/evaluate.c
  src/engine/eval/nnue/nnue_hash.c
  src/engine/eval/nnue/nnue_weight_storage.c
  src/engine/eval/nnue/nnue_parse.c
  src/engine/eval/nnue/nnue_ft.c
  src/engine/eval/nnue/nnue_feature.c
  src/engine/eval/nnue/nnue_feature_bb.c
  src/engine/eval/nnue/nnue_accumulator.c
  src/engine/eval/nnue/nnue_affine.c
  src/engine/eval/nnue/nnue_inference.c
  src/engine/eval/nnue/network.c
  src/engine/search/search.c
  src/engine/search/search_common.c
  src/engine/search/search_setup.c
  src/engine/search/search_id.c
  src/engine/search/search_main.c
  src/engine/search/search_back.c
  src/engine/search/search_qsearch.c
  src/engine/search/search_control.c
  src/engine/search/search_emit.c
  src/engine/search/syzygy_pv.c
  src/engine/search/root_move_build.c
  src/engine/search/uci_wdl.c
  src/engine/search/movepick.c
  src/engine/search/history.c
  src/engine/search/timeman.c
  src/engine/search/tt.c
  src/engine/state/correction_bundle.c
  src/engine/state/position_storage.c
  src/engine/state/root_move.c
  src/engine/state/shared_state.c
  src/engine/state/worker_histories.c
  src/engine/state/worker_construct.c
  src/engine/state/worker_layout.c
  src/platform/clock.c
  src/platform/memory.c
  src/platform/thread_runtime.c
  src/platform/thread.c
  src/platform/numa.c
  src/platform/thread_pool.c
  src/platform/tablebase.c
  src/platform/syzygy/tables.c
  src/platform/syzygy/registry.c
  src/platform/syzygy/probe.c
  src/platform/syzygy/encode.c
  src/platform/syzygy/decode.c
  src/platform/syzygy/wdl.c
)

red()   { printf '\033[31m%s\033[0m\n' "$*"; }
green() { printf '\033[32m%s\033[0m\n' "$*"; }
info()  { printf '\033[36m==>\033[0m %s\n' "$*"; }

need_binary() { [[ -x $BIN ]] || do_build; }

do_build() {
  info "building $BIN (release)"
  mkdir -p build
  "$CC" "${CFLAGS_COMMON[@]}" "${CFLAGS_RELEASE[@]}" -o "$BIN" "${SOURCES[@]}" -lm
  green "built $BIN"
}

do_debug() {
  info "building build/mcfish-debug (asan+ubsan)"
  mkdir -p build
  "$CC" "${CFLAGS_COMMON[@]}" "${CFLAGS_DEBUG[@]}" -o build/mcfish-debug "${SOURCES[@]}" -lm
  green "built build/mcfish-debug"
}

do_zone_check() {
  info "zone check: engine/ + platform/ must link without shell/"
  mkdir -p build
  # Link with a stub main so the archive is exercised, not just compiled: a
  # forbidden call into shell/ is a link error, which compiling alone would miss.
  echo 'int main(void){return 0;}' > build/zone_stub.c
  "$CC" "${CFLAGS_COMMON[@]}" -O1 -o build/zone-check "${ENGINE_SOURCES[@]}" build/zone_stub.c -lm
  green "zone check passed"
}

do_bench() {
  need_binary
  # No depth argument means upstream's definition (depth 13, full default list,
  # Hash 16, one ucinewgame). Overriding the depth measures a different search
  # and cannot be compared with upstream's published number.
  if [[ -n ${1:-} ]]; then engine bench "$1"; else engine bench; fi
}

# Report where the NNUE net must be and how to get it. Deliberately does NOT
# download: the net is a runtime input rather than a build product, and a build
# step that fetches ~90 MB turns every clean build into a network dependency.
# Read the expected name from the source, so a net bump edits one line and this
# step follows it.
do_net() {
  local name dir found
  name=$(grep -oE 'nn-[0-9a-f]+\.nnue' src/engine/eval/nnue/network.h | head -1)
  [[ -n $name ]] || { red "could not read the default net name from network.h"; return 1; }

  info "expected net: $name"
  echo
  echo "This repository keeps it in $RESOURCES_DIR/, beside the Syzygy tables:"
  echo "  $RESOURCES_DIR/$name"
  echo
  echo "Every ./build.sh step that runs the engine runs it FROM $RESOURCES_DIR/, so that"
  echo "is the directory the gates read. The engine itself searches upstream's"
  echo "three candidates and no others:"
  echo "  1. <internal>     mcfish embeds no net, so this candidate always misses"
  echo "  2. .              the working directory the engine was launched from"
  echo "  3. <binary dir>/  the directory holding the executable"
  echo
  echo "Running it by hand from elsewhere therefore needs a cd or a full path:"
  echo "  (cd $RESOURCES_DIR && $ROOT/$BIN)"
  echo "  setoption name EvalFile value $ROOT/$RESOURCES_DIR/$name"
  echo
  echo "Obtain it with:"
  echo "  curl -fL -o $RESOURCES_DIR/$name https://tests.stockfishchess.org/api/nn/$name"
  echo
  echo "Without a net the engine still plays: it falls back to the classical"
  echo "placeholder evaluation and says so through an info string."
  echo

  # Report every directory the engine could load from, not just the canonical
  # one: a stray copy beside the binary silently wins over $RESOURCES_DIR/ for a
  # hand-run from build/, and finding that by surprise costs an afternoon.
  found=0
  for dir in "$RESOURCES_DIR" . "$(dirname "$BIN")"; do
    if [[ -f "$dir/$name" ]]; then
      green "found $dir/$name"
      found=1
    fi
  done
  [[ $found -eq 1 ]] || red "net NOT found in $RESOURCES_DIR/ or any fallback: the engine will run classical."
}

do_signature() {
  need_binary

  # The anchor is only meaningful WITH the net: NNUE and the classical fallback
  # search different trees, so a missing net yields a different count that looks
  # like drift. Fail loudly rather than compare a number the gate cannot interpret.
  # Buffer first: `grep -q` exits on the first match and closes the pipe, so bench
  # dies with SIGPIPE and `set -o pipefail` propagates 141 -- making this test read
  # FALSE even when the message is present. Same trap as tools/port_status.sh.
  local net_probe
  net_probe=$(engine bench 1 2>&1 || true)
  if grep -q 'was not loaded' <<< "$net_probe"; then
    red "no NNUE net reachable — the signature gate did NOT run."
    red "The anchor is defined with the net loaded; without it bench searches the"
    red "classical fallback tree and produces an unrelated number."
    red "Run './build.sh net' for where to obtain it. This is a SKIPPED gate."
    return 127
  fi

  local expected actual
  expected=$(grep -v '^#' tools/signature.golden | tr -d '[:space:]')
  # bench prints its banners on stderr (upstream does too); read the total from there.
  actual=$(engine bench 2>&1 >/dev/null | grep 'Nodes searched' | awk '{print $NF}')

  if [[ $actual == "$expected" ]]; then
    green "signature OK: $actual nodes"
  else
    red "signature DRIFT: expected $expected, got $actual"
    red "A byte-changing engine edit moved the anchor. If the change is intended,"
    red "re-derive it with: ./build.sh signature-update"
    return 1
  fi
}

do_signature_update() {
  need_binary
  local actual
  actual=$(engine bench 2>&1 >/dev/null | grep 'Nodes searched' | awk '{print $NF}')
  { echo "# mcfish bench node signature: the full default position list at depth 13,"
    echo "# Threads 1, Hash 16, and a SINGLE ucinewgame -- the table and the history"
    echo "# block carry across positions. Every one of those four facts changes the"
    echo "# number; see src/shell/benchmark.c. Requires a net: a fallback-eval run"
    echo "# produces an unrelated total."
    echo "# Regenerate ONLY for an intended behaviour change, and say what moved it in"
    echo "# the commit body. Updating this on a red gate launders a bug into the anchor."
    echo "$actual"
  } > tools/signature.golden
  green "signature golden set to $actual"
}

do_simd_scalar() {
  # Build the engine with EVERY vector type and intrinsic compiled out, and require
  # the same bench anchor.
  #
  # This is the correctness oracle for src/engine/eval/nnue/simd.h. That header
  # provides one vocabulary in two implementations -- GCC vector extensions and a
  # plain lane loop -- and asserts they are value-identical. Every other gate here
  # runs the vector path only, so a wrong assumption about vector lowering is
  # invisible to all of them, and the failure mode is not a crash: the engine
  # searches a different tree and still looks like a working chess engine.
  #
  # nnue_dot4_i32 is the specific reason this gate exists. On x86 it lowers to
  # pmaddubsw + pmaddwd, and pmaddubsw SATURATES its int16 intermediate; the scalar
  # body cannot. They agree only because activation outputs are capped at 127 and
  # weights are int8, so the pair sum peaks at 32512. That is an argument, and this
  # gate is what checks it against the real net rather than believing it.
  #
  # This class of bug is invisible to every other gate: a vector operation that is
  # correct only under one backend's lowering benches a wrong number everywhere
  # else without a single diagnostic.
  info "simd-scalar: rebuilding with MCFISH_SIMD_SCALAR and re-asserting the anchor"
  mkdir -p build
  "$CC" "${CFLAGS_COMMON[@]}" "${CFLAGS_RELEASE[@]}" -DMCFISH_SIMD_SCALAR \
    -o build/mcfish-scalar "${SOURCES[@]}" -lm

  local net_probe
  net_probe=$(engine_at build/mcfish-scalar bench 1 2>&1 || true)
  if grep -q 'was not loaded' <<< "$net_probe"; then
    red "no NNUE net reachable — the simd-scalar gate did NOT run."
    return 127
  fi

  local expected actual
  expected=$(grep -v '^#' tools/signature.golden | tr -d '[:space:]')
  actual=$(engine_at build/mcfish-scalar bench 2>&1 >/dev/null | grep 'Nodes searched' | awk '{print $NF}')
  if [[ $actual == "$expected" ]]; then
    green "simd-scalar OK: $actual nodes — vector and scalar paths agree"
  else
    red "simd-scalar MISMATCH: scalar=$actual vector=$expected"
    red "The two bodies of simd.h are NOT value-identical. Suspect the one reducing"
    red "primitive (nnue_dot4_i32) and its saturation argument first."
    return 1
  fi
}

do_arch_determinism() {
  # Every ISA tier the host can execute must bench the SAME node count.
  #
  # The evaluation is integer-exact from features to score, so it is arch-invariant
  # by construction -- but simd.h is written in GCC vector extensions, and widening
  # the flags changes how every one of them lowers. That is exactly where a
  # bit-exactness break would hide, and no other gate here builds more than one tier.
  #
  # Gate each tier on host capability rather than assuming: a tier the CPU cannot
  # execute would SIGILL and read as a failure of the port.
  local expected tiers=(sse41)
  expected=$(grep -v '^#' tools/signature.golden | tr -d '[:space:]')
  grep -qw avx2 /proc/cpuinfo && tiers+=(avx2)
  tiers+=(native)

  info "arch-determinism: ${tiers[*]} must all bench $expected"
  local tier actual failed=0
  for tier in "${tiers[@]}"; do
    MCFISH_ARCH=$tier BIN=build/mcfish-$tier "$0" build > /dev/null || { red "$tier: build failed"; failed=1; continue; }
    actual=$(engine_at build/mcfish-$tier bench 2>&1 >/dev/null | grep 'Nodes searched' | awk '{print $NF}')
    if [[ $actual == "$expected" ]]; then
      green "  ok   $tier: $actual"
    else
      red "  FAIL $tier: $actual (expected $expected)"
      failed=1
    fi
  done
  [[ $failed -eq 0 ]] || { red "the evaluation is NOT arch-invariant -- suspect simd.h lowering"; return 1; }
  green "arch-determinism passed"
}

do_perft() {
  need_binary
  info "perft gate vs tools/perft.table"
  local fails=0
  while IFS='|' read -r fen depth expected; do
    [[ $fen =~ ^#.*$ || -z $fen ]] && continue
    local got
    got=$(printf 'position fen %s\ngo perft %s\nquit\n' "$fen" "$depth" \
          | engine | grep 'Nodes searched' | awk '{print $NF}')
    if [[ $got == "$expected" ]]; then
      printf '  ok   depth %s  %s\n' "$depth" "${fen:0:40}"
    else
      red "  FAIL depth $depth  $fen"
      red "       expected $expected, got ${got:-<none>}"
      fails=$((fails + 1))
    fi
  done < tools/perft.table

  [[ $fails -eq 0 ]] || { red "perft: $fails position(s) failed"; return 1; }
  green "perft gate passed"
}

# Strip the fields that legitimately vary between runs, so a golden pins BEHAVIOUR
# and not the speed of the machine that produced it. `nodes` is deterministic and
# stays; `nps` and `time` are wall-clock derived and cannot be compared.
# Keep this list minimal: every field normalised away is a field no golden guards.
normalize() {
  # Elide what is volatile, and DROP what is a declared gap -- never both silently.
  #
  # The identity banner carries a version and a git sha, so it differs between
  # mcfish and the oracle by construction and cannot be compared; it is replaced,
  # not removed, so its ABSENCE is still a diff.
  #
  # The dropped lines below are upstream output mcfish does not yet produce because
  # the corresponding subsystem is unwired. Each is a GAP, and this filter is the
  # only thing keeping it out of the goldens -- so when the subsystem lands, delete
  # its line here FIRST and let the gate go red. A filter that outlives its gap
  # silently stops comparing real output.
  #   - "Available processors" / "Using N thread" / "Network replica": thread pool
  #     and NNUE shared-memory replication (src/platform/thread_pool.c, unwired).
  sed -E 's/ nps [0-9]+//; s/ time [0-9]+//; s/^Total time \(ms\) *: [0-9]+$/Total time (ms) : <elided>/; s/^Nodes\/second *: [0-9]+$/Nodes\/second    : <elided>/' \
    | sed -E 's/^(mcfish|Stockfish) [^ ]+ by .*/<engine banner>/' \
    | grep -vE '^info string (Available processors|Using [0-9]+ thread|Network replica)' \
    | tr -d '\r'
}

do_golden() {
  need_binary
  info "golden-diff gate vs tools/*.golden"
  local fails=0
  for script in tools/cases/*.uci; do
    local name golden actual
    name=$(basename "$script" .uci)
    golden="tools/${name}.golden"
    [[ -f $golden ]] || { red "  missing golden for $name"; fails=$((fails + 1)); continue; }

    # Merge both streams: some checks read stderr (bench banners) and some stdout.
    #
    # Record the EXIT STATUS as part of the fingerprint. A critical error makes the
    # engine terminate non-zero on purpose (upstream does the same), so the status is
    # contract, not noise -- an engine that printed the diagnostic and then kept
    # running would otherwise pass this gate. `|| true` keeps `set -e` from aborting
    # the whole step on that intended failure.
    local rc
    actual=$({ engine < "$ROOT/$script" 2>&1; printf 'exit=%d\n' "$?"; } | normalize) || true
    rc=0
    if diff -u <(cat "$golden") <(printf '%s\n' "$actual") > /dev/null; then
      printf '  ok   %s\n' "$name"
    else
      red "  FAIL $name"
      diff -u "$golden" <(printf '%s\n' "$actual") | head -20 || true
      fails=$((fails + 1))
    fi
  done

  [[ $fails -eq 0 ]] || { red "golden: $fails case(s) drifted"; return 1; }
  green "golden gate passed"
}

do_golden_update() {
  need_binary
  for script in tools/cases/*.uci; do
    local name
    name=$(basename "$script" .uci)
    # Produce EXACTLY what do_golden compares, including the trailing `exit=N`.
    # Without it this step wrote a golden the gate could never match, and — because
    # a case like board.uci exits 1 by design — `set -e` then killed the loop after
    # truncating the first file, leaving the rest stale.
    { engine < "$ROOT/$script" 2>&1; printf 'exit=%d\n' "$?"; } | normalize \
      > "tools/${name}.golden" || true
    info "updated tools/${name}.golden"
  done
  red "Goldens regenerated. A golden can pin a DEFECT: verify each diff by hand"
  red "before committing, and say in the body what behaviour changed and why."
}

do_test() {
  info "unit + property tests"
  mkdir -p build
  "$CC" "${CFLAGS_COMMON[@]}" -O1 -g -fsanitize=address,undefined \
    -o build/mcfish-test "${ENGINE_SOURCES[@]}" tests/test_main.c -lm
  ./build/mcfish-test
}

# Re-run the suite under ThreadSanitizer.
#
# This is the gate the threading zone actually needs. The pool spawns real OS threads,
# hands them jobs, waits on a condition variable and joins them; a missing broadcast or a
# `worker` slot written after the join is invisible to every other gate here, because the
# single-threaded search never reaches that code and a race does not have to fire. TSan
# instruments the happens-before edges instead of hoping the schedule lands badly.
#
# Kept OUT of `parity`: TSan needs its own build of the whole engine and roughly triples
# the suite's runtime. Run it whenever src/platform/thread*.c changes.
do_tsan() {
  info "unit + property tests under ThreadSanitizer"
  mkdir -p build
  "$CC" "$STD_FLAG" -Wall -Wextra -Isrc -D_POSIX_C_SOURCE=200809L -O1 -g \
    -fsanitize=thread \
    -o build/mcfish-tsan "${ENGINE_SOURCES[@]}" tests/test_main.c -lm
  ./build/mcfish-tsan
  green "tsan clean"
}

# Run a REAL SEARCH under ThreadSanitizer, not the unit suite.
#
# `tsan` above builds ENGINE_SOURCES plus the test binary, so the only concurrent
# code it reaches is the thread-pool test: it gates thread.c, thread_pool.c and
# thread_runtime.c and no engine code at all. This step builds the whole engine,
# shell included, and drives `go` through the UCI front end -- which is the only
# way a race in the SEARCH can be observed.
#
# Today it reports zero, and that number is a measurement rather than a claim:
# `Threads` is accepted and ignored, so the process never leaves one thread and
# the shared state cannot be raced. Both halves are worth having on record. The
# step exists now so that the day the pool is driven, the first run of it is a
# comparison against a known-zero baseline instead of a first look.
#
# Do NOT read a green run as "the search is race-free". It says "no race FIRED on
# one thread", which is a much weaker statement and stays weak until Threads > 1
# does something.
#
# The instrumentation itself is known to work: making the pool test's counter a
# plain int instead of an atomic_int makes `./build.sh tsan` report the race at
# test_main.c:717 and exit 66. Re-run that experiment rather than trusting this
# comment if a zero here ever needs to be believed.
do_tsan_search() {
  local depth=${1:-14} threads=${2:-8}
  info "full engine under ThreadSanitizer: Threads=$threads, go depth $depth"
  mkdir -p build
  "$CC" "$STD_FLAG" -Wall -Isrc -D_POSIX_C_SOURCE=200809L -O1 -g \
    -fsanitize=thread "${CFLAGS_ARCH[@]}" \
    -o build/mcfish-tsan-engine "${SOURCES[@]}" -lm

  local log; log=$(mktemp)
  printf 'setoption name Threads value %d\nsetoption name Hash value 1\nucinewgame\nposition startpos\ngo depth %d\nquit\n' \
    "$threads" "$depth" \
    | ( cd "$ROOT/$RESOURCES_DIR" && "$ROOT/build/mcfish-tsan-engine" ) > "$log" 2>&1

  local races threads_seen
  races=$(grep -c "WARNING: ThreadSanitizer" "$log" || true)
  threads_seen=$(grep -c "ThreadSanitizer.*Thread T[1-9]" "$log" || true)

  if ! grep -q "^bestmove" "$log"; then
    red "tsan-search: the search did not complete -- this is not a clean run"
    tail -20 "$log" >&2
    rm -f "$log"
    return 1
  fi

  if [[ $races -eq 0 ]]; then
    green "tsan-search: 0 races over a depth-$depth search"
    printf '  the process stayed on ONE thread (%s extra thread(s) seen): `Threads` is\n' "$threads_seen"
    printf '  accepted and ignored, so this is a baseline, not a clean bill of health.\n'
    printf '  See docs/04-multithreading.md.\n'
  else
    red "tsan-search: $races race(s) reported"
    grep -A6 "WARNING: ThreadSanitizer" "$log" | head -40 >&2
    rm -f "$log"
    return 1
  fi
  rm -f "$log"
}

# Resolve clang-format, preferring a versioned binary. Echo the name, or nothing
# when none is installed — never fall back to a no-op, because a formatting gate
# that silently does nothing is worse than no gate at all.
find_clang_format() {
  local c
  for c in clang-format clang-format-22 clang-format-21 clang-format-20 \
           /usr/lib/llvm-22/bin/clang-format /usr/lib/llvm-18/bin/clang-format; do
    command -v "$c" > /dev/null 2>&1 && { echo "$c"; return 0; }
  done
  return 1
}

sources_to_format() { find src tests -name '*.c' -o -name '*.h'; }

do_port_status() {
  bash tools/port_status.sh
}

# Report the drift between the pinned SHAs and the tracked repositories.
#
# The three files under tools/upstream/ record which commit of each tracked
# project this tree has been ported UP TO. Nothing read them before this step
# existed, so ZFISH_BASE sat five commits stale while reading as authoritative --
# a pin nobody checks is worse than no pin, because it is a false record rather
# than an absent one.
#
# This does not gate. A tracked repository moving is normal and is not a defect
# here; the failure it catches is not NOTICING that it moved.
do_sync_status() {
  local name dir pin head n
  local rc=0
  for pair in "Stockfish:../Stockfish:tools/upstream/UPSTREAM_BASE" \
              "zfish:../zfish:tools/upstream/ZFISH_BASE"; do
    name=${pair%%:*}; dir=$(echo "$pair" | cut -d: -f2); pinfile=$(echo "$pair" | cut -d: -f3)

    if [[ ! -d $dir/.git ]]; then
      red "  $name: no checkout at $dir -- cannot verify ${pinfile##*/}"
      rc=1
      continue
    fi
    pin=$(tr -d '[:space:]' < "$pinfile")
    head=$(git -C "$dir" rev-parse HEAD)

    if ! git -C "$dir" cat-file -e "$pin^{commit}" 2>/dev/null; then
      red "  $name: pinned $pin is not a commit in $dir"
      rc=1
      continue
    fi

    n=$(git -C "$dir" rev-list --count "$pin..HEAD")
    if [[ $n -eq 0 ]]; then
      green "  $name: in sync at ${pin:0:9}"
    else
      printf '  \033[33m%s: %d commit(s) behind\033[0m  (pinned %s, HEAD %s)\n' \
        "$name" "$n" "${pin:0:9}" "${head:0:9}"
      git -C "$dir" log --oneline --reverse "$pin..HEAD" | sed 's/^/      /'
    fi
  done

  # There is nothing else to check. This tree holds no upstream mirrors: the
  # copies of tests/ and scripts/ were deleted rather than kept in step, because
  # nothing consumed them and a stale mirror manufactures rebase conflicts rather
  # than smoothing them. Everything that IS tracked upstream is tracked by SHA.
  return $rc
}

# The finish-line gate. RED until the port completes -- that is the definition of
# done, not a regression. Kept OUT of `parity` for exactly that reason: parity must
# stay green on a correct in-progress tree.
do_upstream_parity() {
  bash tools/upstream/upstream_parity.sh "$@"
}

do_docs_lint() {
  info "docs-lint: dead links, named paths, pinned signature"
  bash tools/docs_lint.sh
}

do_fmt() {
  info "clang-format --dry-run --Werror"
  local cf
  if ! cf=$(find_clang_format); then
    red "clang-format not found on PATH — the format gate did NOT run."
    red "Install it (apt install clang-format) and re-run. This is a SKIPPED gate,"
    red "not a passing one."
    return 127
  fi
  # Check this explicitly. do_parity calls do_fmt as the left operand of `||`, and
  # bash disables `set -e` for the WHOLE body of a function invoked that way -- so
  # without `|| return 1` a clang-format failure does not abort the function,
  # execution falls through to the green line below, and parity prints "format
  # clean" and "all gates passed" over real violations. Any gate body reached from
  # do_parity must check its own commands rather than lean on `set -e`.
  "$cf" --dry-run --Werror $(sources_to_format) || return 1
  green "format clean ($cf)"
}

do_fmt_fix() {
  local cf
  cf=$(find_clang_format) || { red "clang-format not found on PATH"; return 127; }
  "$cf" -i $(sources_to_format)
  green "formatted ($cf)"
}

# The 3-man Syzygy set the `tb` gate runs against: KPvK KNvK KBvK KRvK KQvK, WDL
# and DTZ. Never committed -- 10 binary files are a runtime input, like the net.
TB_DIR=$RESOURCES_DIR/syzygy

do_tb_fetch() {
  info "fetching the 3-man Syzygy set into $TB_DIR"
  mkdir -p "$TB_DIR"
  local stem ext dir magic f code got fails=0
  for stem in KPvK KNvK KBvK KRvK KQvK; do
    for ext in rtbw rtbz; do
      if [[ $ext == rtbw ]]; then dir=3-4-5-wdl; magic=71e8235d; else dir=3-4-5-dtz; magic=d7660ca5; fi
      f="$TB_DIR/$stem.$ext"
      [[ -s $f ]] && continue
      code=$(curl -sS -o "$f" -w '%{http_code}' \
        "https://tablebase.lichess.ovh/tables/standard/$dir/$stem.$ext") || code=000
      # Verify the Syzygy magic, not just the HTTP status. A mirror that answers a
      # missing file with a 200 and an HTML error page would otherwise be stored as
      # a table and fail much later, inside the decoder, as a corrupt-file report.
      got=$(xxd -p -l 4 "$f" 2> /dev/null || true)
      if [[ $code != 200 || $got != "$magic" ]]; then
        red "  REJECT $stem.$ext (http=$code magic=${got:-none} want=$magic)"
        rm -f "$f"
        fails=$((fails + 1))
      else
        printf '  ok   %s (%s bytes)\n' "$stem.$ext" "$(stat -c%s "$f")"
      fi
    done
  done
  [[ $fails -eq 0 ]] || { red "tb-fetch: $fails file(s) failed"; return 1; }
  green "3-man set present in $TB_DIR"
}

# Emit the fingerprint the `tb` gate compares: the discovery report for an absent
# path, then -- only when the tables are there -- the load report and, per battery
# position, the ROOT probe's score and tbhits and the PV.
#
# The DEPTH-1 line is the one read, because at depth 1 the PV is entirely the work
# of syzygy_extend_pv: the search has contributed one move, everything after it is
# the tablebase's own minimum-DTZ walk. That makes it exactly reproducible against
# the oracle, and an unported or half-ported extension shows up as a one-move PV.
#
# Score, tbhits and pv are pinned; nodes, seldepth and bestmove are NOT -- they are
# search-side and this gate is not a search gate.
#
# PAUSE is the seconds to wait after each `go` before sending `quit`. mcfish's `go`
# is synchronous and needs 0; the oracle runs it on another thread, so a piped
# `quit` cuts the search off and the fingerprint records an EMPTY pv -- a golden
# that then matches nothing this gate can produce.
tb_fingerprint() {
  local bin=$1 tbpath=$2 fens=$3 pause=${4:-0} label fen out
  printf 'discovery-absent %s\n' \
    "$(printf 'setoption name SyzygyPath value /nonexistent-syzygy-dir\nquit\n' \
       | "$bin" 2>&1 | grep -oE 'Found .*' || echo MISSING)"

  [[ -n $tbpath ]] || return 0
  printf 'discovery-present %s\n' \
    "$(printf 'setoption name SyzygyPath value %s\nquit\n' "$tbpath" \
       | "$bin" 2>&1 | grep -oE 'Found .*' || echo MISSING)"

  while read -r label fen; do
    [[ -z $label || $label == \#* ]] && continue
    # Match ` pv ` with its leading space: an unanchored `pv` also matches inside
    # `multipv`, which swallows the whole line as one field.
    out=$({ printf 'setoption name SyzygyPath value %s\nposition fen %s\ngo depth 12\n' \
              "$tbpath" "$fen"
            [[ $pause != 0 ]] && sleep "$pause"
            printf 'quit\n'; } \
          | "$bin" 2>&1 | grep -E '^info depth 1 ' | head -1 \
          | grep -oE 'score [a-z]+ -?[0-9]+|tbhits [0-9]+| pv .*' | tr '\n' ' ' | tr -s ' ')
    printf '%s %s\n' "$label" "${out:-NO-PROBE}"
  done < "$fens"
}

do_tb() {
  need_binary
  info "tb gate: Syzygy discovery and root probe vs tools/tb.golden"
  [[ -f tools/tb.golden ]] || { red "missing tools/tb.golden"; return 1; }

  # Run the probe half only with a complete set. A missing table must read as
  # UNEXERCISED, never as a pass -- a gate that quietly compares two empty halves
  # is exactly how an unwired prober stays green.
  # Count with a glob, not `ls | wc -l`: under `set -o pipefail` a failing `ls`
  # (no such directory) would abort the gate before it printed why.
  local tbpath='' f n=0
  for f in "$TB_DIR"/*.rtbw "$TB_DIR"/*.rtbz; do [[ -s $f ]] && n=$((n + 1)) || true; done
  if [[ $n -eq 10 ]]; then
    tbpath=$PWD/$TB_DIR
  else
    red "  $TB_DIR has $n/10 files: the PROBE PATH IS UNEXERCISED by this run."
    red "  Only discovery-with-no-tables is checked. Run './build.sh tb-fetch' first."
  fi

  local actual
  # Run from $RESOURCES_DIR like every other engine invocation, so the net loads; the
  # oracle path in do_tb_update already does the same from its own directory.
  actual=$(cd "$RESOURCES_DIR" && tb_fingerprint "$ROOT/$BIN" "$tbpath" "$ROOT/tools/cases/tb.fens")
  # Compare only the lines this run could produce, so an absent set narrows the
  # gate instead of failing it -- while the message above keeps that visible.
  if diff -u <(grep -E "^($(printf '%s' "$actual" | cut -d' ' -f1 | paste -sd'|'))\b" tools/tb.golden) \
             <(printf '%s\n' "$actual") ; then
    [[ -n $tbpath ]] && green "tb gate passed (discovery + root probe)" \
                     || green "tb gate passed (discovery only -- probe unexercised)"
  else
    red "tb gate: drifted from tools/tb.golden"
    return 1
  fi
}

# Re-derive tools/tb.golden from the ORACLE, never from mcfish. The oracle is run
# from its own directory, so the table path must be absolute.
do_tb_update() {
  local f n=0
  for f in "$TB_DIR"/*.rtbw "$TB_DIR"/*.rtbz; do [[ -s $f ]] && n=$((n + 1)) || true; done
  [[ $n -eq 10 ]] || { red "need all 10 files in $TB_DIR; run './build.sh tb-fetch'"; return 1; }
  local oracle=/home/usr00/_git/.mcfish-upstream-oracle/src/stockfish
  [[ -x $oracle ]] || { red "no oracle at $oracle"; return 1; }
  # Run the oracle from its own directory so it finds its net, and hand it
  # absolute paths for both the battery and the tables.
  local here=$PWD out
  out=$(cd "$(dirname "$oracle")" \
        && tb_fingerprint "$oracle" "$here/$TB_DIR" "$here/tools/cases/tb.fens" 5)
  printf '%s\n' "$out" > tools/tb.golden
  info "updated tools/tb.golden from the oracle"
}

do_parity() {
  # The aggregate. Run this before calling any behaviour-changing change done.
  #
  # A gate whose TOOL is missing exits 127. Treat that as SKIPPED and keep going,
  # but never let it read as a pass: the summary below names every skipped gate,
  # because "parity passed" over a silently absent linter is how a gate rots.
  local skipped=()

  do_build
  do_zone_check

  do_fmt || { [[ $? -eq 127 ]] && skipped+=(fmt) || return 1; }

  do_docs_lint
  do_test

  # Signature exits 127 when no net is reachable, for the same reason fmt does when
  # clang-format is absent: the gate could not run. Name it as skipped rather than
  # let a fallback-tree node count read as an anchor comparison.
  do_signature || { [[ $? -eq 127 ]] && skipped+=(signature) || return 1; }
  do_simd_scalar || { [[ $? -eq 127 ]] && skipped+=(simd-scalar) || return 1; }

  do_perft
  do_golden
  do_tb

  if [[ ${#skipped[@]} -eq 0 ]]; then
    green "=== parity: all gates passed ==="
  else
    green "=== parity: gates passed, ${#skipped[@]} SKIPPED: ${skipped[*]} ==="
    red "A skipped gate proves nothing. Install the missing tool before relying on this run."
    return 0
  fi
}

do_clean() {
  # Preserve any net sitting in build/. `build/` is one of the three directories
  # network_load searches, so a plain `rm -rf build` silently destroys a ~90 MB
  # download and the next signature run reports a skipped gate instead.
  local stash=""
  if compgen -G 'build/*.nnue' > /dev/null; then
    stash=$(mktemp -d)
    mv build/*.nnue "$stash"/
  fi

  rm -rf build

  if [[ -n $stash ]]; then
    mkdir -p build
    mv "$stash"/*.nnue build/
    rmdir "$stash"
    green "cleaned (kept the NNUE net)"
  else
    green "cleaned"
  fi
}

do_help() {
  cat <<'EOF'
usage: ./build.sh <step> [args]

  build              compile the release binary          -> build/mcfish
  debug              compile with asan+ubsan             -> build/mcfish-debug
  test               build and run the unit/property suite (asan+ubsan)
  tsan               re-run the suite under ThreadSanitizer (the thread-pool gate)
  tsan-search [d] [t] run a real search under ThreadSanitizer (the search-race baseline)
  bench [depth]      run the benchmark (default depth 13)
  simd-scalar        rebuild with the scalar SIMD path and re-assert the anchor
  arch-determinism   build every executable ISA tier and require one node count
  net                report where the NNUE net must be and how to obtain it
  signature          assert the bench node count vs tools/signature.golden
  perft              assert perft counts vs tools/perft.table
  golden             diff the UCI case outputs vs tools/*.golden
  tb-fetch           download + magic-verify the 3-man Syzygy set -> resources/syzygy
  tb                 assert Syzygy discovery and the root probe vs tools/tb.golden
  zone-check         assert engine/+platform/ link without shell/
  fmt / fmt-fix      check / apply clang-format
  docs-lint          check docs for dead links and stale paths
  port-status        report progress toward the bit-exact 1:1 port
  sync-status        report drift between the pinned SHAs and the tracked repos
  upstream-parity    THE finish line: bench vs a pristine upstream build (red until done)
  parity             the aggregate: every in-repo gate above
  clean              remove build/

  signature-update   re-derive the signature golden  (intended changes only)
  golden-update      re-derive the UCI goldens       (intended changes only)
  tb-update          re-derive tools/tb.golden FROM THE ORACLE

Read docs/09-tooling-ci.md before regenerating any golden: doing so on a red
gate pins the defect instead of fixing it.
EOF
}

case "${1:-build}" in
  build)            do_build ;;
  debug)            do_debug ;;
  test)             do_test ;;
  tsan)             do_tsan ;;
  tsan-search)      shift; do_tsan_search "$@" ;;
  bench)            do_bench "${2:-}" ;;
  net)              do_net ;;
  signature)        do_signature ;;
  simd-scalar)      do_simd_scalar ;;
  arch-determinism) do_arch_determinism ;;
  signature-update) do_signature_update ;;
  perft)            do_perft ;;
  golden)           do_golden ;;
  tb)               do_tb ;;
  tb-fetch)         do_tb_fetch ;;
  tb-update)        do_tb_update ;;
  golden-update)    do_golden_update ;;
  zone-check)       do_zone_check ;;
  docs-lint)        do_docs_lint ;;
  port-status)      do_port_status ;;
  sync-status)      do_sync_status ;;
  upstream-parity)  shift; do_upstream_parity "$@" ;;
  fmt)              do_fmt ;;
  fmt-fix)          do_fmt_fix ;;
  parity)           do_parity ;;
  clean)            do_clean ;;
  help|-h|--help)   do_help ;;
  *)                red "unknown step: $1"; echo; do_help; exit 2 ;;
esac
