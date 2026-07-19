#!/usr/bin/env bash
# Build and gate ccfish. One clang invocation per translation unit, no build
# system: the source set is small and fully enumerated below, so a Makefile would
# only add a dependency-tracking layer that the `clean && build` cycle already
# covers. Steps mirror the gate battery — run `./build.sh help` for the list.
set -euo pipefail

cd "$(dirname "$0")"

BIN=${BIN:-build/ccfish}
CC=${CC:-clang}

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
# Getting this wrong is not a small error. zfish's shipped binary on this host is
# x86-64-avx512icl with VNNI -- a single vpdpbusd does the whole u8xi8 dot product
# that the SSE4.1 path needs pmaddubsw + pmaddwd + paddd for. Every ccfish-vs-zfish
# nps number taken before this was SSE4.1 against AVX-512, which measures the tier
# and not the port. zfish records the same rule (docs/09-tooling-ci.md: "Comparing a
# native AVX-512 zfish against the SSE4.1 oracle measures the ARCH, not the code").
#
# The node count must not move across tiers -- the evaluation is integer-exact, so
# it is arch-invariant by construction. `./build.sh arch-determinism` is what checks
# that claim instead of trusting it.
CCFISH_ARCH=${CCFISH_ARCH:-sse41}
case "$CCFISH_ARCH" in
  sse41)  CFLAGS_ARCH=(-msse -msse2 -msse3 -mssse3 -msse4.1 -mpopcnt) ;;
  avx2)   CFLAGS_ARCH=(-mavx2 -mbmi -mbmi2 -mpopcnt) ;;
  native) CFLAGS_ARCH=(-march=native) ;;
  *)      red "unknown CCFISH_ARCH: $CCFISH_ARCH (want sse41, avx2 or native)"; exit 2 ;;
esac

CFLAGS_RELEASE=(-O3 -DNDEBUG -fno-math-errno "${CFLAGS_ARCH[@]}")

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
  src/engine/search/root_move_build.c
  src/engine/search/uci_wdl.c
  src/engine/search/movepick.c
  src/engine/search/history.c
  src/engine/search/timeman.c
  src/engine/search/tt.c
  src/platform/clock.c
  src/shell/bench_positions.c
  src/shell/benchmark.c
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
  src/engine/search/root_move_build.c
  src/engine/search/uci_wdl.c
  src/engine/search/movepick.c
  src/engine/search/history.c
  src/engine/search/timeman.c
  src/engine/search/tt.c
  src/platform/clock.c
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
  info "building build/ccfish-debug (asan+ubsan)"
  mkdir -p build
  "$CC" "${CFLAGS_COMMON[@]}" "${CFLAGS_DEBUG[@]}" -o build/ccfish-debug "${SOURCES[@]}" -lm
  green "built build/ccfish-debug"
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
  if [[ -n ${1:-} ]]; then "$BIN" bench "$1"; else "$BIN" bench; fi
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
  echo "The engine searches three directories, in this order:"
  echo "  1. <internal>     ccfish embeds no net, so this candidate always misses"
  echo "  2. .              the working directory the engine was launched from"
  echo "  3. <binary dir>/  the directory holding the executable"
  echo
  echo "So place the file at ./$name or beside the binary, or point the UCI"
  echo "option EvalFile at a full path:"
  echo "  setoption name EvalFile value /path/to/$name"
  echo
  echo "Obtain it with:"
  echo "  curl -fL -o $name https://tests.stockfishchess.org/api/nn/$name"
  echo
  echo "Without a net the engine still plays: it falls back to the classical"
  echo "placeholder evaluation and says so through an info string."
  echo

  found=0
  for dir in . "$(dirname "$BIN")"; do
    if [[ -f "$dir/$name" ]]; then
      green "found $dir/$name"
      found=1
    fi
  done
  [[ $found -eq 1 ]] || red "net NOT found in either directory: the engine will run classical."
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
  net_probe=$("$BIN" bench 1 2>&1 || true)
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
  actual=$("$BIN" bench 2>&1 >/dev/null | grep 'Nodes searched' | awk '{print $NF}')

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
  actual=$("$BIN" bench 2>&1 >/dev/null | grep 'Nodes searched' | awk '{print $NF}')
  { echo "# ccfish bench node signature: the full default position list at depth 13,"
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
  # zfish gates the same class through its C backend (tools/c_backend_check.sh),
  # where it caught a @Vector(N, bool) bitcast that was correct only under LLVM's
  # bit-packing and benched a wrong number through every other gate.
  info "simd-scalar: rebuilding with CCFISH_SIMD_SCALAR and re-asserting the anchor"
  mkdir -p build
  "$CC" "${CFLAGS_COMMON[@]}" "${CFLAGS_RELEASE[@]}" -DCCFISH_SIMD_SCALAR \
    -o build/ccfish-scalar "${SOURCES[@]}" -lm

  local net_probe
  net_probe=$(build/ccfish-scalar bench 1 2>&1 || true)
  if grep -q 'was not loaded' <<< "$net_probe"; then
    red "no NNUE net reachable — the simd-scalar gate did NOT run."
    return 127
  fi

  local expected actual
  expected=$(grep -v '^#' tools/signature.golden | tr -d '[:space:]')
  actual=$(build/ccfish-scalar bench 2>&1 >/dev/null | grep 'Nodes searched' | awk '{print $NF}')
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
    CCFISH_ARCH=$tier BIN=build/ccfish-$tier "$0" build > /dev/null || { red "$tier: build failed"; failed=1; continue; }
    actual=$(build/ccfish-$tier bench 2>&1 >/dev/null | grep 'Nodes searched' | awk '{print $NF}')
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
          | "$BIN" | grep 'Nodes searched' | awk '{print $NF}')
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
  sed -E 's/ nps [0-9]+//; s/ time [0-9]+//; s/^Total time \(ms\) *: [0-9]+$/Total time (ms) : <elided>/; s/^Nodes\/second *: [0-9]+$/Nodes\/second    : <elided>/' \
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
    actual=$("$BIN" < "$script" 2>&1 | normalize)
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
    "$BIN" < "$script" 2>&1 | normalize > "tools/${name}.golden"
    info "updated tools/${name}.golden"
  done
  red "Goldens regenerated. A golden can pin a DEFECT: verify each diff by hand"
  red "before committing, and say in the body what behaviour changed and why."
}

do_test() {
  info "unit + property tests"
  mkdir -p build
  "$CC" "${CFLAGS_COMMON[@]}" -O1 -g -fsanitize=address,undefined \
    -o build/ccfish-test "${ENGINE_SOURCES[@]}" tests/test_main.c -lm
  ./build/ccfish-test
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

  build              compile the release binary          -> build/ccfish
  debug              compile with asan+ubsan             -> build/ccfish-debug
  test               build and run the unit/property suite
  bench [depth]      run the benchmark (default depth 13)
  simd-scalar        rebuild with the scalar SIMD path and re-assert the anchor
  arch-determinism   build every executable ISA tier and require one node count
  net                report where the NNUE net must be and how to obtain it
  signature          assert the bench node count vs tools/signature.golden
  perft              assert perft counts vs tools/perft.table
  golden             diff the UCI case outputs vs tools/*.golden
  zone-check         assert engine/+platform/ link without shell/
  fmt / fmt-fix      check / apply clang-format
  docs-lint          check docs for dead links and stale paths
  port-status        report progress toward the bit-exact 1:1 port
  upstream-parity    THE finish line: bench vs a pristine upstream build (red until done)
  parity             the aggregate: every in-repo gate above
  clean              remove build/

  signature-update   re-derive the signature golden  (intended changes only)
  golden-update      re-derive the UCI goldens       (intended changes only)

Read docs/07-tooling-ci.md before regenerating any golden: doing so on a red
gate pins the defect instead of fixing it.
EOF
}

case "${1:-build}" in
  build)            do_build ;;
  debug)            do_debug ;;
  test)             do_test ;;
  bench)            do_bench "${2:-}" ;;
  net)              do_net ;;
  signature)        do_signature ;;
  simd-scalar)      do_simd_scalar ;;
  arch-determinism) do_arch_determinism ;;
  signature-update) do_signature_update ;;
  perft)            do_perft ;;
  golden)           do_golden ;;
  golden-update)    do_golden_update ;;
  zone-check)       do_zone_check ;;
  docs-lint)        do_docs_lint ;;
  port-status)      do_port_status ;;
  upstream-parity)  shift; do_upstream_parity "$@" ;;
  fmt)              do_fmt ;;
  fmt-fix)          do_fmt_fix ;;
  parity)           do_parity ;;
  clean)            do_clean ;;
  help|-h|--help)   do_help ;;
  *)                red "unknown step: $1"; echo; do_help; exit 2 ;;
esac
