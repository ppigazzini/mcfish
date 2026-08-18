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

# Promote the conversions that break a domain type into errors.
#
# The C23 `typedef enum : uint8_t` is this port's newtype: `Square`, `Piece`,
# `Color`, `Direction`, `TbFile`, `TbStm` and the rest are distinct types with a
# fixed width, and both compilers already diagnose a confusion between two of
# them. But they diagnose it as a WARNING under a build that is not -Werror, so a
# `Direction` reaching a `Square` parameter printed a line and produced a binary
# -- an advisory, not a guarantee. These promotions are what make the enum tier
# load-bearing, and the tree is clean under every one of them:
#
#   enum-conversion           one domain type reaching another's parameter (gcc,
#                             and clang accepts the spelling too)
#   implicit-enum-enum-cast   the same, clang's own spelling
#   implicit-int-conversion   a raw narrowing integer entering a domain type
#
# The third is the one that matters for a fold: `edge_distance` maps eight board
# files onto four table files, and without it an unconverted board file reaches
# `TbFile` for the price of a warning. None of them stops an EXPLICIT cast, which
# is the point -- a conversion should be a line a reviewer can see.
#
# Probe rather than pin, because coverage is NOT the same on both compilers and
# an unrecognised `-Werror=` is a hard error rather than a no-op:
#
#   gcc 13.3 / 14.2   -Wenum-conversion only. It is already on under -Wall here
#                     and catches enum-to-enum; gcc has NO int-to-enum narrowing
#                     diagnostic, so that half stays advisory on this lane.
#   clang             all three spellings, so both halves are enforced.
#
# Measured on this host with gcc-13 (`-std=c2x`), gcc-14 (`-std=c23`) and clang,
# by compiling a confusion rather than by reading flag lists: gcc reports
# `implicit conversion from 'TB' to 'TA' [-Wenum-conversion]` and stays silent on
# the int-to-enum case. Re-measure rather than assume when a toolchain moves.
#
# `-Wassign-enum` is deliberately NOT promoted: `attacks.c`'s KnightSteps and
# KingSteps are `Direction` arrays of literal offsets, and a Direction is any
# signed step rather than only the eight the enum names.
#
# Two more ride along, both the same shape one level up -- an annotation is only
# as strong as the diagnostic that enforces it:
#
#   unused-result     39 functions here carry `[[nodiscard]]`. It was a warning,
#                     so discarding a `registry_map_wdl` or `pos_set` result
#                     compiled -- and two did, in the test suite, where a rejected
#                     FEN would have left the next assertion reading a stale
#                     position.
#   unused-parameter  this was SUPPRESSED tree-wide by `-Wno-unused-parameter`,
#                     the only suppression in the set. It hid nothing: zero
#                     findings on all three compilers. A dead suppression is worse
#                     than none, because it also covers everything written next.
#                     C23's `[[maybe_unused]]` is the per-parameter escape, and
#                     the tree already uses it where a signature is fixed by a
#                     callback contract.
#
# All three compilers accept both, and the tree is clean under each.
detect_enum_flags() {
  local f
  for f in -Werror=enum-conversion -Werror=implicit-enum-enum-cast \
           -Werror=implicit-int-conversion -Werror=unused-result \
           -Werror=unused-parameter; do
    if echo 'int main(void){return 0;}' \
       | "$CC" "$STD_FLAG" "$f" -x c - -o /dev/null > /dev/null 2>&1; then
      echo "$f"
    fi
  done
}

mapfile -t ENUM_FLAGS < <(detect_enum_flags)

# -lpthread on every link line. src/platform/thread*.c and numa.c call
# pthread_create, pthread_mutex_* and sched_setaffinity directly. It links today
# without this ONLY because glibc >= 2.34 folded the pthread symbols into libc --
# on any older glibc, or a musl/BSD host, the same sources fail at link. Asking
# for the library the code uses is not a portability nicety, it is the honest
# dependency, open since the thread modules landed.
LIBS=(-lm -lpthread)

CFLAGS_COMMON=(
  "$STD_FLAG"
  -Wall -Wextra -Wshadow -Wstrict-prototypes -Wmissing-prototypes
  -Wconversion -Wsign-conversion
  "${ENUM_FLAGS[@]}"
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
#   native               the widest tier this host can execute -- what the engine
#                        should ship as here, and the only honest basis for comparing
#                        against a natively-built port. It NAMES a tier below rather
#                        than emitting host-specific code; see the ladder.
#
# Getting this wrong is not a small error. A native build on this host selects
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
# `native` SELECTS one of the enumerated tiers below; it never asks the compiler what
# this host is. `-march=native` would make the emitted code a property of the machine
# that ran the build -- clang resolves it to a -target-cpu (znver4 here) with tuning and
# ISA extensions no tier name records, so two hosts sharing a label would ship different
# binaries and every per-tier number would silently mean "whatever box took it". The
# ladder is upstream's own ARCH set, so a tier name is a complete description of the
# code, both engines can be built at the SAME named ISA, and a standing filed under one
# is reproducible anywhere. ../zfish resolves native the same way, for the same reason.
#
# The floor is deliberate: a host with avx512f but no VNNI takes avx512, and one with
# no avx512 at all takes avx2. That may leave an extension unused where `-march=native`
# would have taken it. Reproducibility is worth more than the last extension here --
# every gate, budget and Elo standing in this tree compares across builds.
detect_arch_tier() {
  local f=/proc/cpuinfo
  if grep -qw avx512_vbmi2 $f 2>/dev/null && grep -qw avx512_bitalg $f 2>/dev/null \
     && grep -qw avx512_vnni $f 2>/dev/null; then echo avx512icl
  elif grep -qw avx512_vnni $f 2>/dev/null; then echo vnni512
  elif grep -qw avx512f $f 2>/dev/null;     then echo avx512
  elif grep -qw avx2 $f 2>/dev/null;        then echo avx2
  else echo sse41; fi
}
MCFISH_ARCH=${MCFISH_ARCH:-sse41}
# Keep what the caller asked for: only the report distinguishes `native` from the tier
# it chose, and every other consumer wants the concrete tier.
MCFISH_ARCH_SELECTOR=$MCFISH_ARCH
[[ $MCFISH_ARCH == native ]] && MCFISH_ARCH=$(detect_arch_tier)

# Upstream's implication chain, mirrored: each tier is every flag the ones below it
# carry, plus its own (Stockfish src/Makefile, the `findstring -<tier>` blocks and the
# per-feature CXXFLAGS below them). Written out per tier rather than accumulated, so a
# tier is readable in one line and cannot inherit a flag by accident.
case "$MCFISH_ARCH" in
  sse41)   CFLAGS_ARCH=(-msse -msse2 -msse3 -mssse3 -msse4.1 -mpopcnt) ;;
  avx2)    CFLAGS_ARCH=(-mavx2 -mbmi -mbmi2 -mpopcnt) ;;
  avx512)  CFLAGS_ARCH=(-mavx2 -mbmi -mbmi2 -mpopcnt -mavx512f -mavx512bw -mavx512dq
                        -mavx512vl) ;;
  # Mirror upstream's x86-64-vnni512 tier so the two engines can be compared at the
  # SAME named ISA: everything avx2 has, plus the avx512 foundation and VNNI.
  vnni512) CFLAGS_ARCH=(-mavx2 -mbmi -mbmi2 -mpopcnt -mavx512f -mavx512bw -mavx512dq
                        -mavx512vl -mavx512vnni) ;;
  # Upstream's top x86-64 tier. vbmi2/bitalg/ifma/vpopcntdq are what the ICL-gated
  # threat and move-sorting paths compile against, so this is the tier that builds
  # them by NAME -- before this existed they were reachable only through -march=native
  # on a capable host, which is to say only by accident of the build machine.
  avx512icl) CFLAGS_ARCH=(-mavx2 -mbmi -mbmi2 -mpopcnt -mavx512f -mavx512bw -mavx512dq
                          -mavx512vl -mavx512vnni -mavx512cd -mavx512ifma -mavx512vbmi
                          -mavx512vbmi2 -mavx512vpopcntdq -mavx512bitalg -mvpclmulqdq
                          -mgfni -mvaes) ;;
  *)       red "unknown MCFISH_ARCH: $MCFISH_ARCH (want sse41, avx2, avx512, vnni512, avx512icl or native)"; exit 2 ;;
esac

arch_report_label() {
  if [[ $MCFISH_ARCH_SELECTOR == native ]]; then echo "native ($MCFISH_ARCH)"
  else echo "$MCFISH_ARCH"; fi
}

# Name the tier IN THE BINARY, the way upstream's Makefile passes -DARCH. The
# `compiler` command reports it, and a bug report pastes that block verbatim -- so
# the one thing it must not do is guess. Upstream's own labels, so the two engines'
# blocks are comparable line for line. There is no `native` case and no `-class`
# suffix any more: the selector resolved to a real tier above, and the label is that
# tier's upstream name, which now fully describes the code.
case "$MCFISH_ARCH" in
  sse41)     MCFISH_ARCH_STRING=x86-64-sse41-popcnt ;;
  avx2)      MCFISH_ARCH_STRING=x86-64-avx2 ;;
  avx512)    MCFISH_ARCH_STRING=x86-64-avx512 ;;
  vnni512)   MCFISH_ARCH_STRING=x86-64-vnni512 ;;
  avx512icl) MCFISH_ARCH_STRING=x86-64-avx512icl ;;
esac
CFLAGS_ARCH+=("-DMCFISH_ARCH_STRING=\"$MCFISH_ARCH_STRING\"")

# -flto is load-bearing, not a default worth having by habit. The NNUE kernels sit
# in their own translation units, so without it nnue_full_append_changed and
# nnue_bb_pieces_of_exact cannot be inlined AT ALL, and the affine's `sparse` and
# `OUT` never constant-fold. Measured on the search, startup subtracted: 2.387e9
# instructions to 2.218e9, taking the ratio against a clang-built upstream oracle
# at the same ISA from 1.242x to 1.154x.
CFLAGS_RELEASE=(-O3 -DNDEBUG -fno-math-errno -flto "${CFLAGS_ARCH[@]}")

# MCFISH_EVAL_MATERIAL=1 replaces the per-node evaluation with a material sum
# (src/engine/search/search_common.c). It is a MEASUREMENT knob and never a build
# mode: the binary it produces plays badly by construction and moves the anchor,
# so no gate accepts it. What it buys is isolation — with the network gone, a
# differential against an oracle patched with the same formula measures the
# spine, which the whole-binary counters otherwise read through ~60% NNUE.
if [[ ${MCFISH_EVAL_MATERIAL:-0} == 1 ]]; then
  CFLAGS_RELEASE+=(-DMCFISH_EVAL_MATERIAL)
fi

# The accumulator ABLATIONS, and the same rule applies: measurement knobs, never build
# modes. Unlike MCFISH_EVAL_MATERIAL these are bit-exact -- all three builds search the
# identical tree and bench the same node total -- which is what makes them comparable:
# one workload, three amounts of work.
#
#   MCFISH_ACC_REFRESH_ONLY=1   rebuild the accumulator from the board at EVERY
#                               evaluation instead of updating it incrementally
#   MCFISH_NO_THREAT_RECORD=1   compile out do_move's dirty-threat recording; only
#                               valid with the above, which never reads a record
#
# The pair prices the incremental path against the rebuild it replaces, and separates
# what the RECORDING costs from what the delta BUYS. ../zfish f876cb5b measured the
# same architecture at 26.1% cheaper there and ../rfish measured its own 7.1% dearer,
# so the answer is a property of the data model and does not transfer -- it has to be
# measured in each tree.
if [[ ${MCFISH_ACC_REFRESH_ONLY:-0} == 1 ]]; then
  CFLAGS_RELEASE+=(-DMCFISH_ACC_REFRESH_ONLY)
fi
if [[ ${MCFISH_NO_THREAT_RECORD:-0} == 1 ]]; then
  CFLAGS_RELEASE+=(-DMCFISH_NO_THREAT_RECORD)
fi

# MCFISH_ACC_STATS=1 compiles the accumulator path counters into the ENGINE binary and
# makes `bench` print them. The test and tsan binaries already carry them, and the suite
# asserts each of the four ways up the stack is REACHED -- which is a different claim
# from which one carries the traffic. Only a bench answers that, and a refresh is the
# dearest of the four by a wide margin, so what still forces one is where the next lever
# is. Bit-exact and off by default: the macro compiles to nothing in the shipped binary,
# and the anchor is unchanged with it on.
if [[ ${MCFISH_ACC_STATS:-0} == 1 ]]; then
  CFLAGS_RELEASE+=(-DMCFISH_ACC_STATS)
fi

# Stop unrolling at the 512-bit tiers, where it is a pure I-cache cost. Unrolling is
# what makes the hot code big: at x86-64-avx512icl it accounts for 30% of .text
# (296384 -> 208031 B) and 71% of the static zmm ops (6355 -> 1821), because the NNUE
# kernels' constant-trip loops get replicated wholesale. -funroll-loops, which
# upstream's Makefile passes at every ARCH, is a BYTE-IDENTICAL no-op here -- clang
# already unrolls at -O3, so this is the only direction the flag has left. Confine it
# to the wide tiers: at sse41 the same loops are a quarter of the width and the
# unrolled form is far smaller, and that tier has not been measured.
case "$MCFISH_ARCH" in
  avx512 | vnni512 | avx512icl) CFLAGS_RELEASE+=(-fno-unroll-loops) ;;
esac

# -fno-sanitize-recover is load-bearing: without it UBSan PRINTS a diagnostic and
# then continues, so the process still exits 0 and CI reports a green run over a
# real finding. Make undefined behaviour abort.
# shellcheck disable=SC2054  # -fsanitize=address,undefined is ONE flag; the comma is its own
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
  src/engine/eval/nnue/nnue_write.c
  src/engine/eval/nnue/nnue_ft.c
  src/engine/eval/nnue/nnue_feature.c
  src/engine/eval/nnue/nnue_feature_bb.c
  src/engine/eval/nnue/nnue_acc_rowops.c
  src/engine/eval/nnue/nnue_accumulator.c
  src/engine/eval/nnue/nnue_affine.c
  src/engine/eval/nnue/nnue_inference.c
  src/engine/eval/nnue/network.c
  src/engine/search/search.c
  src/engine/search/search_common.c
  src/engine/search/search_setup.c
  src/engine/search/search_id.c
  src/engine/search/search_main.c
  src/engine/search/search_qsearch.c
  src/engine/search/search_control.c
  src/engine/search/search_emit.c
  src/engine/search/pool_source.c
  src/engine/search/worker_set.c
  src/engine/search/syzygy_pv.c
  src/engine/search/root_move_build.c
  src/engine/search/uci_wdl.c
  src/engine/search/movepick.c
  src/engine/search/history.c
  src/engine/search/timeman.c
  src/engine/search/tt.c
  src/engine/state/worker_construct.c
  src/engine/state/worker_layout.c
  src/platform/clock.c
  src/platform/memory.c
  src/platform/thread_runtime.c
  src/platform/thread.c
  src/platform/numa.c
  src/platform/numa_replication.c
  src/platform/thread_pool.c
  src/platform/worker_pool.c
  src/platform/tablebase.c
  src/platform/syzygy/tables.c
  src/platform/syzygy/registry.c
  src/platform/syzygy/probe.c
  src/platform/syzygy/encode.c
  src/platform/syzygy/decode.c
  src/platform/syzygy/wdl.c
  src/shell/bench_positions.c
  src/shell/speedtest.c
  src/shell/speedtest_positions.c
  src/shell/benchmark.c
  src/shell/ucioption.c
  src/shell/syzygy_option.c
  src/shell/uci_output.c
  src/shell/engine_nnue.c
  src/shell/engine_options.c
  src/shell/engine.c
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
  src/engine/eval/nnue/nnue_write.c
  src/engine/eval/nnue/nnue_ft.c
  src/engine/eval/nnue/nnue_feature.c
  src/engine/eval/nnue/nnue_feature_bb.c
  src/engine/eval/nnue/nnue_acc_rowops.c
  src/engine/eval/nnue/nnue_accumulator.c
  src/engine/eval/nnue/nnue_affine.c
  src/engine/eval/nnue/nnue_inference.c
  src/engine/eval/nnue/network.c
  src/engine/search/search.c
  src/engine/search/search_common.c
  src/engine/search/search_setup.c
  src/engine/search/search_id.c
  src/engine/search/search_main.c
  src/engine/search/search_qsearch.c
  src/engine/search/search_control.c
  src/engine/search/search_emit.c
  src/engine/search/pool_source.c
  src/engine/search/worker_set.c
  src/engine/search/syzygy_pv.c
  src/engine/search/root_move_build.c
  src/engine/search/uci_wdl.c
  src/engine/search/movepick.c
  src/engine/search/history.c
  src/engine/search/timeman.c
  src/engine/search/tt.c
  src/engine/state/worker_construct.c
  src/engine/state/worker_layout.c
  src/platform/clock.c
  src/platform/memory.c
  src/platform/thread_runtime.c
  src/platform/thread.c
  src/platform/numa.c
  src/platform/numa_replication.c
  src/platform/thread_pool.c
  src/platform/worker_pool.c
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

# Rebuild when the binary does not match its INPUTS -- by content, not by timestamp.
#
# The stamp hashes every source, every header, the full compile command and the
# compiler's own version, and lands beside the binary. A gate rebuilds only when that
# digest differs.
#
# Timestamps were the obvious implementation and are wrong in both directions. They
# rebuild for free on a `git checkout`, a `touch`, or an editor save that changed
# nothing -- seven seconds each time. And they MISS the case that matters most here:
# MCFISH_ARCH changes no file, so building at sse41 and then running a gate under
# `MCFISH_ARCH=native` left the sse41 binary in place and gated it while reporting the
# native tier. That is the same trap `perf-budget` documents, and it silently voids
# any per-tier comparison.
#
# Before either, this rebuilt only when the binary was ABSENT, so a gate could assert
# against code it had never compiled.
build_stamp() {
  # Read the header list into an ARRAY rather than splitting a command substitution:
  # one element per path whatever the path contains. Sorted, so the stamp is stable.
  local headers
  mapfile -t headers < <(find src -name '*.h' | sort)
  {
    printf '%s\0' "$CC" "$("$CC" -dumpversion 2> /dev/null)" \
      "${CFLAGS_COMMON[@]}" "${CFLAGS_RELEASE[@]}" "${SOURCES[@]}"
    cat "${SOURCES[@]}" "${headers[@]}"
  } 2> /dev/null | sha256sum | cut -d' ' -f1
}

# The same stamp over the DEBUG flags. `malformed` runs on the sanitized binary and
# is in `parity`, so it must not pay for a rebuild it does not need -- and must not
# skip one it does: a stale sanitized binary is a gate judging code that is gone.
debug_stamp() {
  local headers
  mapfile -t headers < <(find src -name '*.h' | sort)
  {
    printf '%s\0' "$CC" "$("$CC" -dumpversion 2> /dev/null)" \
      "${CFLAGS_COMMON[@]}" "${CFLAGS_DEBUG[@]}" "${SOURCES[@]}"
    cat "${SOURCES[@]}" "${headers[@]}"
  } 2> /dev/null | sha256sum | cut -d' ' -f1
}

need_debug_binary() {
  local want
  want=$(debug_stamp)
  if [[ -x build/mcfish-debug && -f build/mcfish-debug.stamp \
        && $(cat build/mcfish-debug.stamp) == "$want" ]]; then
    return
  fi
  info "build/mcfish-debug does not match its sources or flags -- rebuilding"
  do_debug
}

need_binary() {
  local want
  want=$(build_stamp)
  if [[ -x $BIN && -f $BIN.stamp && $(cat "$BIN.stamp") == "$want" ]]; then
    return
  fi
  info "$BIN does not match its sources or flags -- rebuilding before the gate runs"
  do_build
}

do_build() {
  info "building $BIN (release)"
  mkdir -p build
  "$CC" "${CFLAGS_COMMON[@]}" "${CFLAGS_RELEASE[@]}" -o "$BIN" "${SOURCES[@]}" -lm -lpthread
  build_stamp > "$BIN.stamp"
  green "built $BIN"
}

# Profile-guided optimisation. Upstream ships `make profile-build`; the release
# path here is LTO-only, so PGO is the one untapped codegen lever left in the
# toolchain. Three phases: instrument, PROFILE on the canonical bench, rebuild
# guided by the merged counts.
#
# The workload MUST be the fixed `bench` command and nothing else. It is the same
# position list the signature anchor is defined over, so the profile is a
# deterministic, reproducible artifact of the source -- not of whatever the machine
# happened to run. A profile taken from a live game would make the shipped binary
# depend on inputs no gate can reproduce. The profile steers block layout and
# inlining only: it CANNOT move the node count, and do_pgo re-asserts the signature
# to prove that.
#
# llvm-profdata must match clang's major version: clang writes a raw profile format
# that an older distro-default llvm-profdata rejects. Derive the tool from
# `clang -dumpversion` rather than pinning it.
do_pgo() {
  info "PGO build of $BIN ($MCFISH_ARCH): instrument -> profile bench -> rebuild"
  mkdir -p build

  local major profdata
  major=$("$CC" -dumpversion | cut -d. -f1)
  profdata="llvm-profdata-$major"
  command -v "$profdata" > /dev/null 2>&1 || profdata="llvm-profdata"
  command -v "$profdata" > /dev/null 2>&1 || {
    red "no llvm-profdata found -- PGO needs it to merge the raw profile."
    return 127
  }

  local profdir=build/pgo
  rm -rf "$profdir"; mkdir -p "$profdir"

  # Phase 1: instrumented binary. Same release flags so the profiled code is the
  # code that ships; -fprofile-generate adds the counters on top.
  info "phase 1/3: instrumented build -> build/mcfish-instr"
  "$CC" "${CFLAGS_COMMON[@]}" "${CFLAGS_RELEASE[@]}" -fprofile-generate="$profdir" \
    -o build/mcfish-instr "${SOURCES[@]}" "${LIBS[@]}"

  # Phase 2: run the canonical bench under the instrumented binary. The subshell
  # cds into resources/ so the net loads -- without it bench searches the classical
  # tree and the profile covers the wrong code. %p keeps one raw file per pid.
  info "phase 2/3: profiling the bench workload"
  ( cd "$ROOT/$RESOURCES_DIR" \
      && LLVM_PROFILE_FILE="$ROOT/$profdir/mcfish-%p.profraw" \
         "$ROOT/build/mcfish-instr" bench > /dev/null 2>&1 )
  compgen -G "$profdir/*.profraw" > /dev/null || {
    red "no raw profile written -- the instrumented run produced nothing to merge."
    return 1
  }
  "$profdata" merge -output="$profdir/merged.profdata" "$profdir"/*.profraw

  # Phase 3: rebuild guided by the merged profile. The -Wno-profile-instr flags
  # silence coverage notes about counters shifted by inlining -- not correctness
  # signals, and CFLAGS_COMMON runs -Wall.
  info "phase 3/3: profile-guided rebuild -> $BIN"
  "$CC" "${CFLAGS_COMMON[@]}" "${CFLAGS_RELEASE[@]}" \
    -fprofile-use="$profdir/merged.profdata" \
    -Wno-profile-instr-out-of-date -Wno-profile-instr-unprofiled \
    -o "$BIN" "${SOURCES[@]}" "${LIBS[@]}"
  build_stamp > "$BIN.stamp"
  green "built $BIN (PGO)"

  # The profile steers layout only. Prove it did not move the anchor.
  do_signature
}

do_debug() {
  info "building build/mcfish-debug (asan+ubsan)"
  mkdir -p build
  "$CC" "${CFLAGS_COMMON[@]}" "${CFLAGS_DEBUG[@]}" -o build/mcfish-debug "${SOURCES[@]}" -lm -lpthread
  debug_stamp > build/mcfish-debug.stamp
  green "built build/mcfish-debug"
}

# Hold the LISTS the zone check is only as large as.
#
# `do_zone_check` links ENGINE_SOURCES, so the invariant it proves covers exactly the
# files that array names. A new `src/engine/` file added to SOURCES and forgotten in
# ENGINE_SOURCES still ships, and is invisible to the zone check, to the unit test
# binary, to `tsan` and to both fuzzers -- every one of which links ENGINE_SOURCES.
# The proof silently SHRINKS and no gate goes red. docs/10-tooling-ci.md admits that
# in the word "listed" and docs/08-idiomatic-c.md names the consequence, but prose
# cannot hold a list; nothing contradicts a sentence that has drifted.
#
# Taken from ../zfish tools/headless_lint.sh, whose second half asserts that
# src/engine/headless.zig imports every engine-zone module, for the same reason: a
# hand-maintained proof root cannot report that it got smaller. Here it needs no text
# parsing -- both arrays are in scope, so the comparison is against the real lists.
#
# One direction only. A listed file that no longer EXISTS is a compile error in the
# very next command, loud by construction; a file that exists and is listed nowhere
# is the silent one.
list_has() {
  local want=$1 have
  shift
  for have in "$@"; do
    [[ $have == "$want" ]] && return 0
  done
  return 1
}

check_source_lists() {
  local -a disk
  mapfile -t disk < <(find src -name '*.c' | sort)
  # An empty `find` would leave every loop below iterating nothing and the check
  # green over the empty set -- the vacuous-gate failure da21799e guards the two text
  # extractions against. The floor is well under the current 72 and never rises with
  # a new file, so adding one cannot trip it; losing the tree always does.
  local floor=50
  if [[ ${#disk[@]} -lt $floor ]]; then
    red "zone check: only ${#disk[@]} .c files under src/ (floor $floor) -- refusing to report on an empty set"
    return 1
  fi

  local f rc=0
  for f in "${disk[@]}"; do
    if ! list_has "$f" "${SOURCES[@]}"; then
      red "zone check: $f is in no SOURCES entry -- nothing compiles it"
      rc=1
    fi
    case $f in
      src/engine/* | src/platform/*)
        if ! list_has "$f" "${ENGINE_SOURCES[@]}"; then
          red "zone check: $f is outside ENGINE_SOURCES -- the zone proof, the test binary, tsan and both fuzzers do not see it"
          rc=1
        fi
        ;;
    esac
  done

  # The link cannot fail on a shell file that is IN the array: including it supplies
  # the very symbol whose absence was the proof. Assert the gate's INPUT instead.
  for f in "${ENGINE_SOURCES[@]}"; do
    case $f in
      src/engine/* | src/platform/*) ;;
      *)
        red "zone check: ENGINE_SOURCES names $f, outside engine/ and platform/ -- the link would prove nothing"
        rc=1
        ;;
    esac
  done

  return $rc
}

do_zone_check() {
  info "zone check: engine/ + platform/ must link without shell/"
  check_source_lists
  info "zone check: ${#SOURCES[@]} sources, ${#ENGINE_SOURCES[@]} of them in the engine zone, all accounted for"
  mkdir -p build
  # Link with a stub main so the archive is exercised, not just compiled: a
  # forbidden call into shell/ is a link error, which compiling alone would miss.
  echo 'int main(void){return 0;}' > build/zone_stub.c
  "$CC" "${CFLAGS_COMMON[@]}" -O1 -o build/zone-check "${ENGINE_SOURCES[@]}" build/zone_stub.c -lm -lpthread
  green "zone check passed"
}

# Measure the engine->platform edge instead of describing it.
#
# `zone-check` above links ENGINE_SOURCES, which CONTAINS all of platform/, so it
# proves engine/ does not call into shell/ and is structurally blind to engine/
# calling into platform/. That edge is the one docs/00-architecture.md draws dashed
# and calls a gap. Prose cannot hold it: nothing contradicts a sentence that has
# drifted, so the edge has to be read off the linker instead.
#
# Link engine/ ALONE and take the undefined set. It is a ratchet,
# not a pass/fail on zero: the list in tools/engine_platform.baseline is what the
# edge is TODAY, a new symbol fails (that is a fresh host dependency, and the fix is
# a seam), and a symbol that has become unnecessary also fails, asking to be deleted
# -- a baseline that only ever grows stale is the failure mode the uncovered ratchet
# in upstream-map already guards against.
do_engine_standalone() {
  local base=tools/engine_platform.baseline
  [[ -f $base ]] || { red "engine-standalone: no $base"; return 1; }
  info "engine-standalone: link engine/ with no platform object, ratchet the edge"

  local dir; dir=$(mktemp -d)
  local f obj=()
  for f in $(find src/engine -name '*.c' | sort); do
    "$CC" "${CFLAGS_COMMON[@]}" -O1 -c "$f" -o "$dir/$(basename "${f%.c}").o" || {
      red "engine-standalone: $f does not compile on its own"; rm -rf "$dir"; return 1; }
    obj+=("$dir/$(basename "${f%.c}").o")
  done

  # No stub main: `main` then shows up as undefined and is filtered, which keeps the
  # link command honest about what the engine objects alone actually require.
  "$CC" -o "$dir/eng" "${obj[@]}" -lm 2>"$dir/err" || true
  # `|| true` twice, deliberately: grep exits 1 when it matches nothing, and matching
  # nothing is the SUCCESS case here -- an engine that needs no platform symbol at all.
  # Under `set -e` the bare pipeline killed the gate exactly when it had good news.
  { grep -oE "undefined reference to \`[A-Za-z_][A-Za-z0-9_]*'" "$dir/err" || true; } \
    | sed "s/.*\`//; s/'//" | sort -u | { grep -v '^main$' || true; } > "$dir/have"
  # Same `|| true`: an EMPTY baseline is the goal state, and grep exits 1 on it.
  { grep -vE '^\s*(#|$)' "$base" || true; } | tr -d ' \t' | sort -u > "$dir/want"

  local added removed
  added=$(comm -23 "$dir/have" "$dir/want")
  removed=$(comm -13 "$dir/have" "$dir/want")
  local n; n=$(wc -l < "$dir/have")
  rm -rf "$dir"

  if [[ -n $added ]]; then
    red "engine-standalone: NEW platform dependencies in engine/ --"
    printf '%s\n' "$added" | sed 's/^/      /'
    red "  Route it through an injection seam the host registers (search_set_time_source"
    red "  is the worked example); do not add it to $base."
    return 1
  fi
  if [[ -n $removed ]]; then
    red "engine-standalone: these are no longer needed -- delete them from $base:"
    printf '%s\n' "$removed" | sed 's/^/      /'
    red "  A baseline that outlives its entries stops measuring anything."
    return 1
  fi
  if [[ $n -eq 0 ]]; then
    green "engine-standalone: engine/ links with NO platform object — the edge is closed"
    return 0
  fi
  green "engine-standalone: edge holds at $n platform symbol(s) — see $base"
}


# Diff mcfish's WHOLE UCI transcript against the upstream oracle.
#
# The `golden` gate pins what MCFISH printed last time, so it catches a regression and
# is structurally blind to a divergence from the golden: both sides move together when
# a golden is re-derived. This step compares against upstream itself, which is the only
# thing that can answer "is the wire output still Stockfish's".
#
# LOCAL ONLY: it needs a pristine oracle build, which a CI checkout does not carry.
#
# Machine-dependent fields are elided (nps, time, hashfull, the net's size/arch banner)
# and nothing else. Accepted divergences live in tools/transcript_known.txt, one argued
# regex each -- a diff line matching none of them fails the step.
#
# Hold stdin open past the `go`: both engines run the search off the UCI thread, so
# closing the pipe immediately makes upstream abort at depth 1 and the comparison then
# measures the harness rather than the engines.
do_upstream_transcript() {
  need_binary
  local oracle=/home/usr00/_git/.mcfish-upstream-oracle/src/stockfish
  [[ -x $oracle ]] || { red "upstream-transcript: no oracle at $oracle"; return 127; }
  local known=tools/transcript_known.txt
  info "upstream-transcript: whole-transcript diff vs the oracle"

  local dir; dir=$(mktemp -d)
  # grep -f has NO comment syntax: every line of the file is a pattern, and a BLANK
  # line is a pattern matching everything. Feeding the annotated file straight to
  # `grep -vEf` therefore filtered every diff line and the gate could not fail.
  # Strip comments and blanks into a patterns-only file first.
  grep -vE '^\s*(#|$)' "$known" > "$dir/known" || true
  [[ -s $dir/known ]] || printf '%s\n' '$^' > "$dir/known"
  # Hold the case list before driving it. `failed` is the only thing this gate can
  # return non-zero on, so an empty glob would report "0 identical" and exit 0 -- and
  # worse than a bare zero, bash leaves the unmatched pattern as a literal, `cat`
  # fails on it, both engines read empty stdin, and their identical nothing counts as
  # an `ok`. A gate that passes having compared no transcripts reads exactly like one
  # that compared them all. Same reasoning as assert_fuzz_executed, one gate over.
  local cases=(tools/cases/transcript/*.uci)
  [[ -e ${cases[0]:-} ]] || {
    red "upstream-transcript: no cases at tools/cases/transcript/*.uci -- compared NOTHING"
    return 1
  }
  local ok=0 accepted=0 failed=0 rig=0 script name
  for script in "${cases[@]}"; do
    name=$(basename "$script" .uci)

    # HOLD STDIN FOR AS LONG AS THE CASE ACTUALLY SEARCHES. Five seconds covers every
    # case that ends in a shallow `go`, and a case that has to pass upstream's ten
    # million node reporting threshold needs a minute of it. Close the pipe first and
    # each engine stops wherever its own clock left it, which compares two truncations.
    # A case declares its own budget in a `# hold <seconds>` line, which both engines
    # ignore as a comment, so the number lives beside the search that needs it.
    local hold declared
    declared=$(grep -E '^# hold' "$script" || true)
    hold=$(printf '%s\n' "$declared" | sed -nE 's/^# hold ([0-9]+)$/\1/p' | head -1)
    if [[ -n $declared && -z $hold ]]; then
      red "  FAIL  $name -- unparsable hold header: $declared"
      red "        spell it exactly '# hold <seconds>'. Unparsed it falls to the 5s"
      red "        default, and the case it was written for is cut off mid-search."
      failed=$((failed + 1))
      continue
    fi
    hold=${hold:-5}

    transcript_drive "$script" "$hold" "$declared" "$ROOT/$RESOURCES_DIR" "$ROOT/$BIN" "$dir/mc"
    transcript_drive "$script" "$hold" "$declared" "$(dirname "$oracle")" "$oracle" "$dir/up"

    # TWO BLANK SIDES COMPARE EQUAL, and that is a rig fault rather than an
    # agreement. Every way a side can fail blanks it to nothing -- an engine that
    # dies before its banner, a failing `cd` into the resources dir, a
    # transcript_normalize filter that eats both outputs at once -- and `diff` of two
    # empty files is silent, so the case scores `ok` having compared nothing. Unlike
    # `golden` next door, this gate records no exit status in what it diffs, so the
    # blank is not distinguishable downstream; it has to be caught here. No case this
    # gate drives is legitimately empty on both sides, because every one of them
    # reaches at least the engine banner. ONE side blank stays a DIFF -- that is the
    # gate working. Guards the case that 01e0b71c's empty-CORPUS check cannot see;
    # ../zfish a4f0b6e9 is the sibling's version.
    if [[ ! -s $dir/mc && ! -s $dir/up ]]; then
      red "  RIG   $name -- BOTH engines produced no output; nothing was compared"
      rig=$((rig + 1))
      continue
    fi

    # A DECLARED HOLD IS A CLAIM THAT THE SEARCH FITS INSIDE IT, and on a loaded
    # machine it may not. Each side then stops wherever its own clock left it, so the
    # diff reports a divergence that is about the host rather than the engines -- and
    # if both happen to stop in the same place, it reports agreement over two halves
    # of a search. Neither reading is about mcfish, so report the rig rather than a
    # verdict. `bestmove` alone does not answer this: an engine that is CUT OFF prints
    # one too, because closing stdin is a `quit` and a stopped search still announces.
    # What separates the two is WHEN it appeared, which is what transcript_drive
    # records in the `.late` marker.
    if [[ -f $dir/mc.late || -f $dir/up.late ]]; then
      red "  RIG   $name -- a side did not answer inside its ${hold}s hold"
      red "        Raise the '# hold' line in $script; the transcripts are truncated"
      red "        at whatever each engine's own clock reached, and are not comparable."
      rig=$((rig + 1))
      continue
    fi

    local raw
    raw=$(diff "$dir/mc" "$dir/up" | grep -E '^[<>]' || true)
    printf '%s\n' "$raw" >> "$dir/all_diffs"

    local unexplained
    unexplained=$(printf '%s\n' "$raw" | grep -v '^$' | grep -vEf "$dir/known" || true)

    if [[ -z $raw ]]; then
      printf '  ok    %s\n' "$name"; ok=$((ok + 1))
    elif [[ -z $unexplained ]]; then
      printf '  known %s\n' "$name"; accepted=$((accepted + 1))
    else
      red "  FAIL  $name"
      printf '%s\n' "$unexplained" | head -12 | sed 's/^/        /'
      failed=$((failed + 1))
    fi
  done

  # EXPIRE THE ALLOWLIST. Every entry is a claim that mcfish may differ from the
  # golden, and an entry whose cause is gone hides nothing while still reading as an
  # accepted divergence -- which is how a filter outlives its gap and the gate quietly
  # stops comparing real output. So each EXPIRING pattern must have MATCHED something
  # in this run; if it did not, the divergence is retired and the line must go.
  #
  # PERMANENT entries are exempt (the engine's own identity will never converge), and
  # an UNTAGGED entry fails: the tag is the author deciding which of the two it is.
  local stale=0 untagged=0 pat tag n
  while IFS= read -r pat; do
    [[ -z $pat ]] && continue
    tag=$(grep -B1 -xF -- "$pat" "$ROOT/$known" | head -1 | grep -oE '^#= [A-Z]+' | awk '{print $2}' || true)
    n=$(grep -cE -- "$pat" "$dir/all_diffs" 2>/dev/null || true)
    case "$tag" in
      PERMANENT) printf '  note  %-52.52s permanent, seen %s\n' "$pat" "$n" ;;
      EXPIRING)
        if [[ ${n:-0} -eq 0 ]]; then
          red "  STALE $pat"
          red "        tagged EXPIRING and matched NOTHING in this run -- the divergence"
          red "        it accepts is gone. Delete the entry; it now hides nothing."
          stale=$((stale + 1))
        else
          printf '  note  %-52.52s expiring, seen %s\n' "$pat" "$n"
        fi ;;
      *)
        red "  UNTAGGED $pat"
        red "        every entry needs '#= EXPIRING' or '#= PERMANENT' on the line above it"
        untagged=$((untagged + 1)) ;;
    esac
  done < "$dir/known"
  rm -rf "$dir"

  [[ $untagged -eq 0 ]] || { red "upstream-transcript: $untagged untagged allowlist entr(ies)"; return 1; }
  [[ $stale -eq 0 ]] || {
    red "upstream-transcript: $stale allowlist entr(ies) no longer describe anything"
    return 1
  }

  # Report the rig BEFORE the verdict: a run that compared nothing must not publish
  # the standing of the cases it did compare, whichever way that standing went. Red
  # rather than ../zfish's exit 2 -- this driver has two codes, 127 for a gate whose
  # TOOL is missing and 1 for a gate that failed, and a rig fault is not a missing
  # tool. The empty-corpus guard above returns 1 for the same reason.
  [[ $rig -eq 0 ]] || {
    red "upstream-transcript: $rig of ${#cases[@]} case(s) compared NOTHING -- rig fault"
    red "  Both engines printed nothing. Check that $BIN and the oracle at"
    red "  $oracle each run, and that $RESOURCES_DIR is reachable."
    return 1
  }
  [[ $failed -eq 0 ]] || {
    red "upstream-transcript: $failed case(s) diverge from the golden"
    red "  Fix mcfish, or add an ARGUED line to $known naming what retires it."
    return 1
  }
  green "upstream-transcript: $ok identical, $accepted with known divergences only (${#cases[@]} cases)"
}

# Elide only what the machine decides. Anything else is a claim about behaviour.
# Drive ONE case through ONE engine, and write the normalised transcript to $6.
#
# The hold is a DEADLINE, not a delay. A case that declares one is waiting on a search
# long enough that sleeping through it would dominate the gate, so the writer closes
# the pipe as soon as the engine has answered -- and, because it can tell "answered"
# from "gave up", it can also say which happened. Not seeing the answer in time leaves
# a `.late` marker beside the transcript, which is the caller's rig signal: a cut-off
# engine prints `bestmove` too, so the presence of that line proves nothing.
#
# Cases WITHOUT a declared hold keep the flat sleep they have always had. Closing early
# on the first `bestmove` would change what they measure -- ponderhit and stop each run
# a SECOND search after the first announcement, and cutting stdin there would quit the
# engine in the middle of it.
# shellcheck disable=SC2094  # $raw is POLLED on one side of a pipeline and WRITTEN by the
# engine on the other -- two processes, one reading and one writing, which is the shape this
# driver exists to be. shellcheck cannot tell that from a self-clobbering redirect.
transcript_drive() {
  local script=$1 hold=$2 declared=$3 workdir=$4 bin=$5 out=$6
  local raw="$out.raw"
  rm -f "$out.late"
  : > "$raw"
  # `|| true`: a case may drive the engine to a deliberate non-zero exit (the
  # malformed-input one does, on both engines), and under `set -euo pipefail` that
  # would abort the sweep instead of being compared.
  {
    {
      cat "$script"
      if [[ -n $declared ]]; then
        local waited=0 limit=$((hold * 10))
        while ((waited < limit)); do
          grep -q '^bestmove' "$raw" 2>/dev/null && break
          sleep 0.1
          waited=$((waited + 1))
        done
        ((waited < limit)) || : > "$out.late"
      else
        sleep "$hold"
      fi
    } | (cd "$workdir" && "$bin") > "$raw" 2>&1
  } || true
  transcript_normalize < "$raw" > "$out"
}

transcript_normalize() {
  sed -E 's/ nps [0-9]+//; s/ time [0-9]+//; s/ hashfull [0-9]+//' \
    | sed -E 's/^(mcfish|Stockfish) [^ ]+ by .*/<engine banner>/' \
    | sed -E 's/\(([0-9]+)MiB, \(([0-9, ]+)\)\)/(<net>)/' \
    | tr -d '\r'
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

# Actually download the net into RESOURCES_DIR (do_net above only PRINTS how to). Idempotent
# and sha256-checked: the net's filename IS its sha256 prefix (nn-<12 hex>.nnue), so a mirror
# that answers a missing file with a 200 and an HTML page is rejected here rather than much
# later inside the parser as a corrupt-net report. Two sources, Fishtest first then the
# official-stockfish mirror -- the same order zfish's fetcher uses. This is what CI runs so the
# signature/bench/golden gates have a net; a developer runs it once and the net is cached in
# RESOURCES_DIR thereafter.
do_net_fetch() {
  local name want f code got url
  name=$(grep -oE 'nn-[0-9a-f]+\.nnue' src/engine/eval/nnue/network.h | head -1)
  [[ -n $name ]] || { red "could not read the default net name from network.h"; return 1; }
  want=${name#nn-}
  want=${want%.nnue}
  mkdir -p "$RESOURCES_DIR"
  f="$RESOURCES_DIR/$name"

  if [[ -s $f ]]; then
    got=$(sha256sum "$f" | cut -c1-12)
    [[ $got == "$want" ]] && { green "net present: $f"; return 0; }
    red "  existing $f has wrong sha256 ($got, want $want) -- refetching"
    rm -f "$f"
  fi

  info "fetching $name into $RESOURCES_DIR"
  for url in \
    "https://tests.stockfishchess.org/api/nn/$name" \
    "https://github.com/official-stockfish/networks/raw/master/$name"; do
    code=$(curl -sSL -o "$f" -w '%{http_code}' "$url") || code=000
    if [[ $code != 200 ]]; then
      red "  $url -> http $code"
      rm -f "$f"
      continue
    fi
    got=$(sha256sum "$f" | cut -c1-12)
    if [[ $got == "$want" ]]; then
      green "fetched $name ($(stat -c%s "$f") bytes) from $url"
      return 0
    fi
    red "  REJECT $url (sha256 $got, want $want)"
    rm -f "$f"
  done
  red "net-fetch: could not obtain a valid $name from any source"
  return 1
}

do_signature() {
  need_binary

  # The anchor is only meaningful WITH the net: NNUE and the classical fallback
  # search different trees, so a missing net yields a different count that looks
  # like drift. Fail loudly rather than compare a number the gate cannot interpret.
  # Buffer first: `grep -q` exits on the first match and closes the pipe, so bench
  # dies with SIGPIPE and `set -o pipefail` propagates 141 -- making this test read
  # FALSE even when the message is present.
  local net_probe
  net_probe=$(engine bench 1 1 1 2>&1 || true)
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

# --- net-roundtrip: the only instrument on the WRITING side --------------------
#
# Every other net gate reads what the ENGINE CONSUMES. `signature`, `simd-scalar` and
# the goldens all run the forward pass, and the forward pass never touches
# `export_net` -- so the whole NNUE write path is an output nothing in the battery
# looks at. A writer that drifts away from its reader is invisible to all of them:
# with two of the writer's eight operations swapped and the reader untouched, the
# anchor still matches and every golden still passes.
#
# This is the weld. The net on disk was written by upstream's own exporter, so
# exporting it back and comparing byte for byte checks every LEB128 group, every split
# point, every hash and the SIMD permutation's inverse at once -- against upstream's
# bytes rather than against a second derivation somebody thought to assert.
# ../rfish a469772 reports the same hole from the Rust side and closes it the same
# way.
#
# It writes OUTSIDE $RESOURCES_DIR, deliberately: that directory is one of the three
# candidates a load searches, so a half-written file there is a net the next run picks
# up. A missing net is a SKIP, as for `signature`.
do_net_roundtrip() {
  need_binary

  local name src dir out reply size_src size_out
  name=$(grep -oE 'nn-[0-9a-f]+\.nnue' src/engine/eval/nnue/network.h | head -1)
  [[ -n $name ]] || { red "net-roundtrip: could not read the default net name from network.h"; return 1; }

  src=$RESOURCES_DIR/$name
  if [[ ! -s $src ]]; then
    red "no NNUE net reachable — net-roundtrip did NOT run."
    red "Run './build.sh net' for where to obtain it. This is a SKIPPED gate."
    return 127
  fi

  dir=$(mktemp -d)
  out=$dir/exported.nnue
  info "net-roundtrip: export the resident net and compare it with $src"

  reply=$(engine export_net "$out" 2>&1 | tail -1)
  if [[ $reply != Network\ saved\ successfully* ]]; then
    red "net-roundtrip: export_net refused -- $reply"
    rm -rf "$dir"
    return 1
  fi

  # Refuse an empty subject rather than report a clean run over nothing: an export
  # that wrote no file compares equal to no file at all under a careless `cmp`.
  if [[ ! -s $out ]]; then
    red "net-roundtrip: export_net reported success and wrote nothing to $out"
    rm -rf "$dir"
    return 1
  fi

  # Two arms, because a short write and a wrong byte are different bugs: the first is
  # a section length or a missing region, the second is an encoding or an order.
  size_src=$(stat -c%s "$src")
  size_out=$(stat -c%s "$out")
  if [[ $size_src -ne $size_out ]]; then
    red "net-roundtrip: exported $size_out bytes, the net on disk is $size_src"
    rm -rf "$dir"
    return 1
  fi

  if ! cmp -s "$out" "$src"; then
    red "net-roundtrip: $(cmp "$out" "$src" 2>&1)"
    red "  The writer and the reader disagree about the format. Nothing else in the"
    red "  battery can see this: the anchor and the goldens read only what the engine"
    red "  CONSUMES, and export_net is not on the eval path."
    rm -rf "$dir"
    return 1
  fi

  rm -rf "$dir"
  green "net-roundtrip: $size_out bytes, byte-identical to $name"
}

# --- perf-budget: an ABSOLUTE instruction-count regression gate --------------------
#
# The bench signature proves the same NODE count; it is blind to how many x86
# instructions those nodes cost. So a refactor can shed no nodes yet run measurably
# slower with every correctness gate green -- exactly the time-domain divergence that
# costs Elo. Retired instructions are deterministic (~0.00002% spread across runs), so
# an absolute per-arch budget is a gateable anchor where thermally-void nps is not.
#
# LOCAL-first, and deliberately NOT in `parity`: perf_event_open is refused in some CI
# containers (the gate SKIPS there, exit 127), and the count is toolchain-specific -- a
# clang upgrade legitimately moves it, so re-derive with `perf-budget-update` then. The
# golden is keyed by ARCH because the count differs per ISA tier.
PERF_BUDGET_GOLDEN=tools/instr_budget.golden
PERF_BUDGET_BENCH=${PERF_BUDGET_BENCH:-16 1 13}
# 0.05%, and the figure is MEASURED, not chosen: five runs of this bench span under
# 0.00002%, so this is ~2000x the noise floor and still catches a per-node regression
# a fifth the size of the one it was set against. The previous 0.5% was ~40000x, and
# a real one walked through it -- forcing pos_adjust_key50_of out of line costs
# +0.238% here with `signature` green, which is exactly the class this gate owns.
# Set a tolerance by mutation or it is a decoration (../zfish 51031f48 learnt the
# same lesson at 0.20%).
PERF_BUDGET_TOL=${PERF_BUDGET_TOL:-0.0005}

# Key a budget row by the tier IN THE BINARY, never by the word that selected it.
# `native` names a different ISA on every host, so a row recorded under it is a
# number about one machine that the next machine will compare its own binary against.
# MCFISH_ARCH_STRING is a resolved tier by the time this runs, and since `native`
# SELECTS an enumerated tier rather than asking the compiler about the host, the tier
# name is a complete description of the binary: two hosts that resolve to the same
# tier build the same code and their counts are comparable, while a host that
# resolves elsewhere finds no row and SKIPS -- loudly, at 127 -- instead of measuring
# against a stranger. That is a property of the arch ladder above, so nothing is
# needed here beyond using the string.
# A WORKLOAD suffix keys its own row. The probing lane measures a different program
# path over a different position list, so its count is not comparable with the bench
# list's and must never land in the same row -- a budget is only a budget while every
# number under one key was taken over the same work.
perf_budget_key() { printf '%s%s' "$MCFISH_ARCH_STRING" "${1:+"+$1"}"; }

# Compile the counter (same cache perf_counters.sh uses) and read $BIN's median retired
# instruction count on the fixed bench. Echoes "INSTRUCTIONS <n>\nNODES <n>", or returns 3
# when perf_event_open is unavailable so the caller can SKIP.
measure_instructions() {
  local counter="${TMPDIR:-/tmp}/mcfish_perf_counters"
  if [[ ! -x $counter || tools/perf_counters.c -nt $counter ]]; then
    # Build the HARNESS under the same warning set as the engine it judges, and
    # fatally. It was the least-checked binary in the tree -- plain `-O2` with no
    # warnings at all -- while every gate that quotes an instruction count trusts
    # whatever it prints. ../zfish d30481f0 found its own parity harness compiled
    # with safety OFF, where a table entry two characters longer than a stack buffer
    # let it report a verdict for a command it never sent. It compiles clean under
    # these today, so -Werror costs nothing and keeps it that way.
    "$CC" -O2 "${CFLAGS_COMMON[@]}" -Werror -o "$counter" tools/perf_counters.c || return 2
  fi
  # No arguments means the bench list, which is what every caller but the probing lane
  # wants. The lane passes its own `bench ...` line because its workload is a FILE.
  local args=("$@")
  # shellcheck disable=SC2206  # PERF_BUDGET_BENCH is a bench ARGUMENT LIST; splitting it is the point
  [[ ${#args[@]} -eq 0 ]] && args=(bench $PERF_BUDGET_BENCH)
  ( cd "$ROOT/$RESOURCES_DIR" && "$counter" --single "$ROOT/$BIN" 5 "${args[@]}" )
}

# Compare ACTUAL against the budget recorded for KEY and print the verdict. Shared by
# both lanes: the ceiling arithmetic, the tolerance and the wording are properties of
# a BUDGET, not of a workload, and a second copy of them is a second thing to keep in
# step. WHAT names the step to re-run, so the advice a red gate prints leads to the
# lane that owns the row rather than to the other one.
perf_budget_verdict() {
  local key=$1 actual=$2 what=$3 budget
  if [[ ! -f $PERF_BUDGET_GOLDEN ]]; then
    info "$what: no budget file yet (tools/instr_budget.golden)."
    info "Record one from a known-good build: MCFISH_ARCH=$MCFISH_ARCH ./build.sh $what-update"
    return 127
  fi
  budget=$(grep -v '^#' "$PERF_BUDGET_GOLDEN" | awk -v a="$key" '$1==a{print $2}' || true)
  if [[ -z $budget ]]; then
    info "$what: no budget recorded for key '$key'."
    # Older files hold rows this key no longer produces: a bare `sse41`/`native` from
    # before the key was the tier string, and a `<class>`/`<class>+<target-cpu>` from
    # when `native` still meant -march=native. Say so rather than leaving a bare "no
    # budget": those numbers describe a build this tree cannot make any more.
    if grep -qv '^#' "$PERF_BUDGET_GOLDEN" \
       && grep -v '^#' "$PERF_BUDGET_GOLDEN" \
          | awk '$1 ~ /^(native|sse41|avx2|avx512|vnni512|avx512icl)$/ || $1 ~ /-class/ {
                   found=1 } END { exit !found }'; then
      info "  a LEGACY row is present, keyed by a name this build no longer emits --"
      info "  re-record it: the key is the tier, and the tier now describes the code"
    fi
    info "Record one from a known-good build: MCFISH_ARCH=$MCFISH_ARCH ./build.sh $what-update"
    return 127
  fi

  # Ceiling = budget * (1 + TOL). A regression INFLATES instructions; a drop is a win.
  awk -v a="$actual" -v b="$budget" -v tol="$PERF_BUDGET_TOL" -v arch="$key" -v what="$what" 'BEGIN{
    ceil = b * (1 + tol); floor = b * (1 - tol);
    # %g, not %.1f: a 0.05% tolerance printed as "0.1%" is a gate lying about its
    # own threshold, and the reader has no other place to learn it.
    if (a > ceil) {
      printf "\033[31m%s REGRESSION (%s): %d instr > budget %d + %g%% (%.0f)\033[0m\n", what, arch, a, b, tol*100, ceil;
      print  "\033[31mA refactor inflated the instruction count without moving the node signature.\033[0m";
      printf "\033[31mIf the change is intended, re-derive: ./build.sh %s-update\033[0m\n", what;
      exit 1;
    } else if (a < floor) {
      printf "\033[32m%s OK (%s): %d instr — IMPROVED vs budget %d (%.2f%%). Consider %s-update.\033[0m\n", what, arch, a, b, (a/b-1)*100, what;
    } else {
      printf "\033[32m%s OK (%s): %d instr (budget %d, within %g%%)\033[0m\n", what, arch, a, b, tol*100;
    }
  }'
}

# Compose the probing bench file and echo its path, or return 127 with the reason.
#
# The SyzygyPath goes INTO the file: upstream's bench reader dispatches a `setoption`
# line wherever the entry list came from (benchmark.c is-setoption), so a workload can
# carry the option it needs and every tool that takes bench arguments -- perf-budget-tb
# here, tools/perf_counters.sh for an A/B -- reaches the reader without learning what
# a tablebase is. The path is RELATIVE because the engine is run from $RESOURCES_DIR,
# which also keeps the composed line far inside the reader's 128-byte line bound.
#
# AN INCOMPLETE CORPUS SKIPS, LOUDLY, and it must: a probing measurement taken with
# tables the positions do not reach is the bench list wearing a different name, and it
# would certify a bound that was never executed.
tb_probe_workload() {
  local f n3=0 n5=0
  for f in "$TB_DIR"/*.rtbw "$TB_DIR"/*.rtbz; do [[ -s $f ]] && n3=$((n3 + 1)) || true; done
  for f in "$TB5_DIR"/*.rtbw "$TB5_DIR"/*.rtbz; do [[ -s $f ]] && n5=$((n5 + 1)) || true; done
  if [[ $n3 -ne 10 || $n5 -ne 6 ]]; then
    red "  the probing corpus is incomplete: $TB_DIR $n3/10, $TB5_DIR $n5/6." >&2
    red "  Run './build.sh tb-fetch 5' first -- the 5-man tables are what this measures." >&2
    return 127
  fi
  [[ -f $TB_PROBE_FENS ]] || { red "  missing $TB_PROBE_FENS" >&2; return 127; }

  local out; out=$(mktemp "${TMPDIR:-/tmp}/mcfish_tbprobe.XXXXXX")
  { echo "setoption name SyzygyPath value ${TB_DIR#"$RESOURCES_DIR"/}:${TB5_DIR#"$RESOURCES_DIR"/}"
    grep -vE '^\s*(#|$)' "$TB_PROBE_FENS"
  } > "$out"
  printf '%s' "$out"
}

do_perf_budget() {
  need_binary
  # Same net requirement as the signature gate: a fallback-eval run is a different tree
  # and a different instruction count -- meaningless against the budget.
  local net_probe
  net_probe=$(engine bench 1 1 1 2>&1 || true)
  if grep -q 'was not loaded' <<< "$net_probe"; then
    red "no NNUE net reachable — the perf-budget gate did NOT run (SKIPPED)."
    return 127
  fi

  info "perf-budget: tier $(arch_report_label), bench $PERF_BUDGET_BENCH"

  local out rc
  out=$(measure_instructions); rc=$?
  if [[ $rc -eq 3 ]]; then
    info "perf-budget: perf_event_open unavailable on this host — SKIPPED (this is local-only)."
    return 127
  fi
  [[ $rc -eq 0 ]] || { red "perf-budget: measurement failed (exit $rc)."; return 1; }

  # A missing golden (grep exits 2) or a header-only golden (grep -v '^#' finds nothing,
  # exits 1) must READ as "no budget yet", not abort the gate under `set -euo pipefail`.
  # perf_budget_verdict guards the file and neutralises grep's no-match exit.
  perf_budget_verdict "$(perf_budget_key)" "$(awk '/^INSTRUCTIONS/{print $2}' <<< "$out")" perf-budget
}

# --- perf-budget-tb: the same gate, on the workload the bench list cannot reach -----
#
# The bench list opens no tablebase, so `registry.c`, `do_probe_table` and the decode
# loop are absent from `perf-budget`'s figure entirely. That is the hole refish's
# `5ace08e4` names and this closes: a bound inside `decode_pairs` is free according to
# every instruction number this tree records, which is not the same as being free.
#
# Depth 14, not the bench's 13, and the difference is not cosmetic. These positions are
# five men and converge early, so a shallow run barely reaches the reader it exists to
# measure.
do_perf_budget_tb() {
  need_binary
  info "perf-budget-tb: retired instructions on the PROBING workload, tier $(arch_report_label)"

  local net_probe
  net_probe=$(engine bench 1 1 1 2>&1 || true)
  if grep -q 'was not loaded' <<< "$net_probe"; then
    red "no NNUE net reachable — perf-budget-tb did NOT run (SKIPPED)."
    return 127
  fi

  local fens; fens=$(tb_probe_workload) || return 127
  local positions; positions=$(grep -cve '^setoption' "$fens")
  info "  workload: $positions positions over the 3-man + 5-man set, at depth 14"
  info "  the corpus is 5-MAN, so every figure taken over it is a LOWER BOUND"

  local out rc
  out=$(measure_instructions bench 16 1 14 "$fens" depth); rc=$?
  rm -f "$fens"
  if [[ $rc -eq 3 ]]; then
    info "perf-budget-tb: perf_event_open unavailable on this host — SKIPPED (local-only)."
    return 127
  fi
  [[ $rc -eq 0 ]] || { red "perf-budget-tb: measurement failed (exit $rc)."; return 1; }

  info "  nodes: $(awk '/^NODES/{print $2}' <<< "$out") (a moved node count is a moved WORKLOAD, not a perf result)"
  perf_budget_verdict "$(perf_budget_key syzygy)" \
                      "$(awk '/^INSTRUCTIONS/{print $2}' <<< "$out")" perf-budget-tb
}

do_perf_budget_update() {
  need_binary
  local out rc actual nodes
  out=$(measure_instructions); rc=$?
  [[ $rc -eq 0 ]] || { red "perf-budget-update: measurement failed (exit $rc). Needs perf_event_open + the net."; return 1; }
  actual=$(awk '/^INSTRUCTIONS/{print $2}' <<< "$out")
  nodes=$(awk '/^NODES/{print $2}' <<< "$out")
  perf_budget_record "$(perf_budget_key)" "$actual" "$nodes" "bench $PERF_BUDGET_BENCH"
}

# Re-derive the PROBING row. Same measurement the lane asserts, written under its own
# key -- never the bench list's, because the two count different programs.
do_perf_budget_tb_update() {
  need_binary
  local fens; fens=$(tb_probe_workload) || return 1
  local out rc actual nodes
  out=$(measure_instructions bench 16 1 14 "$fens" depth); rc=$?
  rm -f "$fens"
  [[ $rc -eq 0 ]] || { red "perf-budget-tb-update: measurement failed (exit $rc). Needs perf_event_open + the net."; return 1; }
  actual=$(awk '/^INSTRUCTIONS/{print $2}' <<< "$out")
  nodes=$(awk '/^NODES/{print $2}' <<< "$out")
  perf_budget_record "$(perf_budget_key syzygy)" "$actual" "$nodes" "the probing workload at depth 14"
}

# Replace KEY's row (or append it), keeping the header and every other row.
perf_budget_record() {
  local key=$1 actual=$2 nodes=$3 workload=$4
  [[ -f $PERF_BUDGET_GOLDEN ]] || {
    { echo "# mcfish retired-instruction budget: median count on 'bench $PERF_BUDGET_BENCH',"
      echo "# per ISA TIER. Deterministic (~0.00002% spread), toolchain-specific: re-derive"
      echo "# on a clang upgrade or an intended perf change. One line per tier:"
      echo "# <tier> <count> -- the tier IN the binary, so a row means the same thing on"
      echo "# every host and a foreign tier finds none rather than matching. Every tier is"
      echo "# a fixed -m flag list, including the one \`native\` selects, so the name is a"
      echo "# complete description of the code the count was taken over."
      echo "# A regression that leaves the node signature untouched shows up ONLY here."
      echo "# A key carrying a +WORKLOAD suffix was taken over that workload instead of the"
      echo "# bench list, and the two are never comparable with each other."
    } > "$PERF_BUDGET_GOLDEN"
  }
  # Replace the line for this key (or append it), keeping the header and other rows.
  local tmp; tmp=$(mktemp)
  awk -v a="$key" -v n="$actual" '
    /^#/ { print; next }
    $1==a { next }
    { print }
    END { print a, n }' "$PERF_BUDGET_GOLDEN" > "$tmp" && mv "$tmp" "$PERF_BUDGET_GOLDEN"
  green "perf-budget golden for '$key' set to $actual instructions ($workload, $nodes nodes)"
}

# --- malformed: a file that was refused yesterday is refused today -----------------
#
# The one gate that watches the engine REFUSE rather than compute. `signature` is
# green with every parser defect this covers live, because the bench reads no file
# the engine did not ship with; `fuzz-tb` hunts for input nobody has described yet,
# probabilistically and on a nightly budget. Neither is a regression test for the
# bounds in decode.c and registry.c, and those bounds are all that stands between a
# crafted `.rtbw` -- the only attacker-supplyable binary input besides the net -- and
# the search. See tools/malformed.sh for what the four parts of a refusal are.
do_malformed() {
  need_debug_binary
  tools/malformed.sh
}

# --- negative-control: prove each correctness gate can actually FAIL --------------
#
# Every gate's power to detect a defect is an ASSUMPTION until something breaks the
# engine on purpose and watches the gate go red. This tree has run that experiment
# by hand, at the moment a gate was edited, and never again -- and twice this month a
# gate turned out to be incapable of failing at all (an empty transcript corpus
# scored as agreement; a docs check that read no subject). Mutation testing calls
# this "seen to fail"; one representative mutant per gate is enough under the
# competent-programmer hypothesis, which is what makes it cheap enough to gate.
#
# Each row applies ONE behavioural mutation, requires the named gate to exit
# non-zero, restores the file, and requires the gate to pass again. The restore is
# unconditional (an EXIT trap), and the run refuses to finish while any source is
# still mutated.
#
# THE MUTATION MUST BE SEEN TO APPLY. A pattern that has rotted matches nothing, the
# tree stays clean, the gate greens -- and that reads as "the gate failed to detect
# it", which is a rig fault reported as a finding. Every row asserts the file
# actually changed, and a row whose pattern no longer matches is exit 2, not a
# verdict.
#
# Rows are <label>|<file>|<sed script>|<gate>. Keep one gate per row: the mutations
# are deliberately narrow, so a row proves one gate's teeth and nothing else.
#
# The simd-scalar row targets the SCALAR arm, and that is not interchangeable with
# the vector one: the gate builds with MCFISH_SIMD_SCALAR and holds THAT binary to
# the anchor, so a mutation in the vector arm moves the anchor's own side and this
# gate never sees it. Mutating the scalar body is also the sharper test, because it
# is invisible to every other gate in the tree -- `signature` stays green, which is
# precisely why this gate exists.
#
# The last field is `run` or `hold`. A `hold` row is NOT run by default and the run
# says so rather than omitting it silently -- a row nobody sees is the lost test this
# gate exists to prevent. `simd-scalar` is held because its mutant is UNBOUNDED, and
# that is a measurement, not an opinion: inverting the activation clamp gives the
# search an evaluation with no ceiling, and the mutated gate ran past 900s where the
# clean gate takes ~90s. The row still works -- `./build.sh negative-control
# simd-scalar` reproduces the rig fault -- and a bounded mutant for the scalar arm is
# the open work, not a bigger timeout.
NEGATIVE_CONTROL_ROWS=(
  "razor margin 483->484%src/engine/search/search_common.c%s#483 + 318#484 + 318#%signature%run"
  "d omits Checkers:%src/engine/board/fen.c%s#Checkers: #CheckersZ: #%golden%run"
  "no knight under-promotion%src/engine/board/movegen.c%/make_move_typed(PROMOTION, from, to, KNIGHT)/d%perft%run"
  "scalar shift off by one%src/engine/eval/nnue/simd.h%s#(Elem) (a.l\[i\] >> s)#(Elem) (a.l[i] >> (s + 1))#%simd-scalar%run"
  "exported FT component hash zeroed%src/engine/eval/nnue/network.c%s#nnue_write_u32_le(w, nnue_feature_transformer_hash_value());#nnue_write_u32_le(w, 0);#%net-roundtrip%run"
  "a refused table says nothing%src/platform/syzygy/registry.c%s#report_unusable(path);#(void) path;#%malformed%run"
  # HELD: only the ABSORBED family detects this, and that family needs the 3-man
  # corpus. On a machine that has not fetched it the gate narrows and stays green,
  # which this rig would credit as "the gate passed a mutated engine" -- a verdict
  # about the machine rather than about the code. Run it by hand:
  #   ./build.sh tb-fetch && ./build.sh negative-control malformed
  "a decoded symbol is unbounded%src/platform/syzygy/decode.c%s#if ((size_t) sym >= d->symlen_size) {#if ((size_t) sym >= (size_t) -1) {#%malformed%hold"
  "an unbounded search is never stopped%src/shell/engine.c%s#if (search_running_unbounded())#if (false \&\& search_running_unbounded())#%async-check%run"
  "every search is stopped, bounded or not%src/shell/engine.c%s#if (search_running_unbounded())#if (true \|\| search_running_unbounded())#%async-check%run"
  "hash_bytes sign-extends its tail%src/engine/eval/nnue/nnue_hash.c%s#k = (k << 8) | (uint64_t) data\[tail + i\];#k = (k << 8) | (uint64_t) (int64_t) (int8_t) data[tail + i];#%test%run"
)

# Bound each mutated gate run. 900s clears every row measured here with room to
# spare; a mutant that needs more is a mutant that hangs (see the rig-fault arm).
NEG_GATE_TIMEOUT=${NEG_GATE_TIMEOUT:-900}

NEG_BACKUP_DIR=""
negative_control_restore() {
  [[ -n $NEG_BACKUP_DIR && -d $NEG_BACKUP_DIR ]] || return 0
  local f rel
  while IFS= read -r f; do
    rel=${f#"$NEG_BACKUP_DIR"/}
    cp "$f" "$ROOT/$rel"
  done < <(find "$NEG_BACKUP_DIR" -type f)
  rm -rf "$NEG_BACKUP_DIR"
  NEG_BACKUP_DIR=""
}

# --- tools-smoke: a tool no lane runs rots exactly like a lane in no gate --------
#
# Four tools in tools/ were invoked by NOTHING -- not build.sh, not a workflow, not
# another tool. Three now have callers (valgrind.sh from the memcheck lane,
# perf_counter_validate below, and this step for the rest); this is the argument for
# why that mattered: valgrind.sh's own header claimed the search was single-threaded
# and `Threads` accepted and ignored, a claim that stopped being true when Lazy-SMP
# landed and survived because nothing ran the file.
#
# A profiler wrapper cannot be GATED -- there is no verdict to assert -- so what this
# does is prove each one still RUNS and still prints the interface its callers read.
# That is the difference between a tool that works and a tool nobody has run since the
# toolchain moved.
# --- lane-coverage: every gate runs somewhere, or says why not -------------------
#
# "A lane that is in no gate is not a lane" was a rule enforced by somebody
# remembering it, and four differentials had quietly stopped being lanes -- including
# `upstream-parity`, the finish line. Now the rule is the gate: every step build.sh
# dispatches must appear in a workflow, in `parity`, or in the excused list below with
# a reason. A new step joins one of the three or this goes red.
#
# The excused list is the hole, so it is short and each line is argued. It expires in
# one direction by construction: a step named here that ALSO appears in a workflow is
# a stale excuse, and that is reported too.
LANE_EXCUSED=(
  "bench:a command, not a gate -- it asserts nothing"
  "clean:removes artefacts; nothing to assert"
  "engine-standalone:a link probe for the engine zone, subsumed by zone-check in parity"
  "fmt-fix:the writing half of fmt, which parity runs"
  "golden-update:refuses by default (do_golden_update); golden-audit --write replaces it"
  "help:prints the step list"
  "material-eval:a MEASUREMENT knob, not a gate -- it builds an engine that plays badly on purpose"
  "pgo:a build mode; the anchor is asserted by signature on the shipped one"
  "perf-budget:LOCAL -- needs perf_event_open, which CI containers refuse, and the golden is per-machine"
  "perf-budget-update:writes that per-machine golden"
  "perf-budget-tb:LOCAL -- perf_event_open plus the 5-man tables (tb-fetch 5), too large for a lane"
  "perf-budget-tb-update:writes that per-machine golden"
  "counter-validate:LOCAL -- needs perf_event_open"
  "signature-update:re-derives the anchor; signature is what asserts it"
  "tb-update:re-derives the tb golden; tb is what asserts it"
  "tb-cursed:needs the 5-man tables (tb-fetch 5), too large for a lane"
  "tb-cursed-update:re-derives that golden"
)

do_lane_coverage() {
  info "lane-coverage: every step in a workflow, in parity, or excused"

  mapfile -t ALL_STEPS < <(sed -n "$(grep -n '^case ' build.sh | tail -1 | cut -d: -f1),\$p" build.sh \
                           | grep -oE '^\s*[a-z][a-z0-9|-]*\)' | tr -d ' )' | tr '|' '\n' \
                           | grep -v '^-' | sort -u)
  # The same floor the docs gate carries, for the same reason: this reads build.sh as
  # TEXT, and an extraction that silently shrinks reports OK over nothing.
  if [[ ${#ALL_STEPS[@]} -lt 35 ]]; then
    red "lane-coverage: parsed only ${#ALL_STEPS[@]} steps (floor 35) -- the extraction went stale"
    return 2
  fi

  local wf_text parity_text
  # STRIP THE COMMENTS. A step NAMED in a workflow comment is not a step the workflow
  # RUNS -- `golden-update` is discussed in one and dispatched by none, and counting
  # that as a lane would excuse exactly the step this tree most wants excused.
  wf_text=$(cat .github/workflows/*.yml 2>/dev/null | sed -E 's/(^|[[:space:]])#.*$//')
  parity_text=$(sed -n '/^do_parity()/,/^}/p' build.sh)

  local unlaned=0 stale=0 step reason excused
  for step in "${ALL_STEPS[@]}"; do
    [[ -z $step || $step == "help" ]] && continue
    excused=""
    for e in "${LANE_EXCUSED[@]}"; do [[ ${e%%:*} == "$step" ]] && excused=${e#*:}; done

    if grep -qE "build\.sh $step\b" <<< "$wf_text" \
       || grep -qE "do_${step//-/_}\b" <<< "$parity_text"; then
      [[ -n $excused ]] && {
        red "  STALE EXCUSE  $step is excused but DOES run in a lane -- delete the excuse"
        stale=$((stale + 1))
      }
      continue
    fi
    if [[ -n $excused ]]; then
      continue
    fi
    red "  NO LANE  $step runs nowhere -- add it to a workflow or excuse it with a reason"
    unlaned=$((unlaned + 1))
  done

  [[ $stale -eq 0 ]] || { red "lane-coverage: $stale stale excuse(s)"; return 1; }
  [[ $unlaned -eq 0 ]] || { red "lane-coverage: $unlaned step(s) run in no lane"; return 1; }
  green "lane-coverage: ${#ALL_STEPS[@]} steps, every one in a lane or excused with a reason"
}

# --- golden-coverage: every golden is read by something ------------------------
#
# `lane-coverage` holds every build STEP to a lane. Nothing held the other half of
# the battery: a golden is a photograph, and a photograph nobody diffs is a file, not
# a check. The failure is silent by construction -- the gate that stopped reading a
# golden is not the gate that goes red -- so an orphaned `tools/*.golden` would sit
# in the tree looking exactly like a live one.
#
# `do_golden` already enforces CASE -> GOLDEN: it globs `tools/cases/*.uci` and goes
# red on a case with no golden. This asks the other direction, which nothing did:
# every golden IN THE TREE must be claimed, by a case file or by an owner row.
#
# THE UNIVERSE IS GLOBBED FROM THE TREE, NEVER LISTED. A second list of goldens would
# rot in exactly the way this gate exists to catch, which is why `lane-coverage`
# derives its step set from the dispatch table rather than from a table of its own.
#
# An owner row is the allowance for a golden no `.uci` case can produce, and it
# expires in three directions: the golden gone from the tree, a case file appearing
# that covers it, or an owner that does not NAME the golden. An owner is a claim that
# something reads the file, and a claim nothing witnesses is just a name.
#
# A LOCAL golden is absent from a fresh checkout by design -- `instr_budget.golden` is
# per-machine and gitignored, so it is present for whoever recorded one and missing
# everywhere else. Absence therefore cannot mean "retired" on its own, and reading it
# that way split this gate by machine: green where the file happened to sit, red in
# CI. `.gitignore` is what tells the two apart, and it is a fact in the tree rather
# than a second list -- so an absent golden must be ignored ON PURPOSE, and its owner
# must still name it. Retiring one means deleting the row, the file AND its ignore.
GOLDEN_OWNERS=(
  "signature.golden:build.sh:the bench anchor -- do_signature diffs it; no UCI case can produce a node count"
  "tb.golden:build.sh:do_tb's Syzygy discovery and root probe, which need a SyzygyPath rather than a case script"
  "tb_cursed.golden:build.sh:do_tb_cursed, LOCAL -- needs the 5-man tables tb-fetch does not get by default"
  "instr_budget.golden:build.sh:do_perf_budget, LOCAL -- a retired-instruction budget needs perf_event_open, which CI containers refuse, and the count is per-machine"
)

do_golden_coverage() {
  info "golden-coverage: every golden read by a gate, every case backed by a golden"
  local fails=0 owned=0 cased=0

  # TREE -> CLAIMED. Glob, never list.
  local path name owner reason row
  for path in tools/*.golden; do
    [[ -e $path ]] || { red "golden-coverage: no tools/*.golden at all -- the glob went stale"; return 2; }
    name=$(basename "$path")
    owner=""; reason=""
    for row in "${GOLDEN_OWNERS[@]}"; do
      [[ ${row%%:*} == "$name" ]] || continue
      owner=${row#*:}; reason=${owner#*:}; owner=${owner%%:*}
    done

    if [[ -f tools/cases/${name%.golden}.uci ]]; then
      [[ -n $owner ]] && {
        red "  STALE OWNER   $name has an owner row AND a UCI case -- delete the row"
        fails=$((fails + 1))
      }
      cased=$((cased + 1))
      continue
    fi

    if [[ -z $owner ]]; then
      red "  UNCLAIMED     $name is read by no case and no owner -- wire it to a gate or delete it"
      fails=$((fails + 1))
      continue
    fi
    if [[ -z $reason ]]; then
      red "  NO REASON     $name is owned by $owner with no argument -- say why it needs a row"
      fails=$((fails + 1))
      continue
    fi
    # WITNESS the claim. An owner that never names the golden is not reading it.
    if [[ ! -f $owner ]] || ! grep -q "$name" "$owner"; then
      red "  UNWITNESSED   $name claims owner $owner, which does not name it"
      fails=$((fails + 1))
      continue
    fi
    owned=$((owned + 1))
  done

  # An owner row for a golden that is gone is the third expiry direction -- unless the
  # golden is gitignored, which is the one way absence is a design and not a deletion.
  local absent=0
  for row in "${GOLDEN_OWNERS[@]}"; do
    name=${row%%:*}
    [[ -f tools/$name ]] && continue
    if ! command -v git >/dev/null 2>&1 || ! git rev-parse --git-dir >/dev/null 2>&1; then
      red "golden-coverage: $name is absent and no git here can say whether that is by design"
      return 2
    fi
    if ! git check-ignore -q "tools/$name"; then
      red "  ROW MISSING   $name is owned but not in the tree -- delete the row with the golden"
      fails=$((fails + 1))
      continue
    fi
    # Absent by design still owes the witness the present ones owe.
    owner=${row#*:}; reason=${owner#*:}; owner=${owner%%:*}
    if [[ ! -f $owner ]] || ! grep -q "$name" "$owner"; then
      red "  UNWITNESSED   $name claims owner $owner, which does not name it"
      fails=$((fails + 1))
      continue
    fi
    absent=$((absent + 1))
  done

  # DECLARED -> TREE, without an engine. do_golden catches this too, but only after a
  # build; here it is a link a lane needing no binary can check.
  local script case_name
  for script in tools/cases/*.uci; do
    case_name=$(basename "$script" .uci)
    [[ -f tools/${case_name}.golden ]] || {
      red "  NO GOLDEN     case $case_name has no tools/${case_name}.golden"
      fails=$((fails + 1))
    }
  done

  [[ $fails -eq 0 ]] || { red "golden-coverage: $fails unclaimed or stale golden(s)"; return 1; }
  green "golden-coverage: $((cased + owned)) goldens ($cased read by a case, $owned by an owner row, $absent owned and gitignored)"
}

do_tools_smoke() {
  info "tools-smoke: every tool no other lane invokes"
  local fails=0

  # perf_delta.py: the startup-subtracted counter standing. Its --help is the contract
  # its callers read, and it must still name the `#R` line format it consumes.
  # Capture, THEN grep. Under `pipefail` a tool that exits non-zero -- which a usage
  # message does by design -- fails the whole pipeline, so `if tool | grep` would
  # report every one of these broken no matter what they printed.
  local out
  out=$(python3 tools/perf_delta.py --help 2>&1 || true)
  if grep -q '#R' <<< "$out"; then
    printf '  \033[32mok\033[0m    perf_delta.py --help names its input format\n'
  else
    red "  perf_delta.py: --help no longer describes the #R lines it parses"; fails=$((fails + 1))
  fi

  # perf_callgrind_delta.py: the only startup-clean read of cache and branch
  # behaviour here. Its refusals ARE the tool -- a missing `.nodes` sidecar means
  # the workload is unknown, and a ratio over two different trees is void -- so the
  # smoke checks it still refuses rather than merely that it runs.
  out=$(python3 tools/perf_callgrind_delta.py 2>&1 || true)
  if grep -q 'startup-subtracted' <<< "$out"; then
    printf '  \033[32mok\033[0m    perf_callgrind_delta.py prints its contract\n'
  else
    red "  perf_callgrind_delta.py: no usage, or it stopped naming what it subtracts"
    fails=$((fails + 1))
  fi
  out=$(python3 tools/perf_callgrind_delta.py /dev/null /dev/null /dev/null /dev/null 2>&1 || true)
  if grep -q 'missing' <<< "$out"; then
    printf '  \033[32mok\033[0m    perf_callgrind_delta.py refuses an unknown workload\n'
  else
    red "  perf_callgrind_delta.py: accepted profiles with no node sidecar"
    fails=$((fails + 1))
  fi

  # perf_sample.sh: refuses without a binary, and its usage names the CWD requirement
  # (resources/), which is the part everyone gets wrong first.
  out=$(bash tools/perf_sample.sh 2>&1 || true)
  if grep -q 'CWD=resources' <<< "$out"; then
    printf '  \033[32mok\033[0m    perf_sample.sh prints its usage and its CWD rule\n'
  else
    red "  perf_sample.sh: no usage line, or it stopped naming the CWD requirement"; fails=$((fails + 1))
  fi

  # perf_counter_validate: the instrument that decides whether a counter can be
  # believed. Compiling it is the smoke; RUNNING it is `./build.sh counter-validate`,
  # which needs perf_event_open and is therefore local-only.
  if "$CC" -O2 "${CFLAGS_COMMON[@]}" -o build/perf_counter_validate \
       tools/perf_counter_validate.c > /dev/null 2>&1; then
    printf '  \033[32mok\033[0m    perf_counter_validate compiles\n'
  else
    red "  perf_counter_validate: does not compile"; fails=$((fails + 1))
  fi

  [[ $fails -eq 0 ]] || { red "tools-smoke: $fails tool(s) broken"; return 1; }
  green "tools-smoke: 4 of 4 unlaned tools still run and print their interface"
}

# Check the counter against two bottlenecks known from first principles. An
# instrument is a hypothesis until something confirms it, and two conclusions in this
# tree have died here.
#
# RUNNING THE VALIDATOR ALONE PROVES NOTHING -- it prints one checksum. The validator
# is a WORKLOAD, and the measurement is what a counter says about it:
#
#   chain  one serial dependency chain of 3-cycle multiplies. Latency-bound by
#          construction, so IPC pins near 1.
#   ilp    four independent chains. Throughput-bound, so IPC runs to 3+.
#
# If the counter does not separate those two, it does not mean what its name says on
# this host, and nothing may be built on it. LOCAL: needs perf_event_open.
do_counter_validate() {
  mkdir -p build
  "$CC" -O2 "${CFLAGS_COMMON[@]}" -o build/perf_counter_validate tools/perf_counter_validate.c \
    || { red "counter-validate: the validator does not compile"; return 1; }

  local counter="${TMPDIR:-/tmp}/mcfish_perf_counters"
  if [[ ! -x $counter || tools/perf_counters.c -nt $counter ]]; then
    "$CC" -O2 "${CFLAGS_COMMON[@]}" -Werror -o "$counter" tools/perf_counters.c \
      || { red "counter-validate: the counter harness does not compile"; return 1; }
  fi

  info "counter-validate: IPC against two known bottlenecks"
  local out ipc_chain ipc_ilp
  out=$("$counter" --single "$ROOT/build/perf_counter_validate" 3 2>&1) || {
    info "counter-validate: perf_event_open unavailable on this host -- SKIPPED."
    return 127
  }
  ipc_chain=$(awk '/^INSTRUCTIONS/{i=$2} /^CYCLES/{c=$2} END{ if (c>0) printf "%.2f", i/c; else print "0" }' <<< "$out")
  out=$("$counter" --single "$ROOT/build/perf_counter_validate" 3 ilp 2>&1) || {
    red "counter-validate: the ilp round failed"; return 1; }
  ipc_ilp=$(awk '/^INSTRUCTIONS/{i=$2} /^CYCLES/{c=$2} END{ if (c>0) printf "%.2f", i/c; else print "0" }' <<< "$out")

  printf '  chain (latency-bound, expect ~1)    IPC %s\n' "$ipc_chain"
  printf '  ilp   (throughput-bound, expect 3+) IPC %s\n' "$ipc_ilp"

  # The assertion is the SEPARATION, not either absolute: a counter that reports the
  # same IPC for both is not measuring what its name claims, whatever the value.
  awk -v a="$ipc_chain" -v b="$ipc_ilp" 'BEGIN{
    if (a <= 0 || b <= 0) { print "counter-validate: read zero cycles -- the counter is dead"; exit 1 }
    if (a > 1.6)  { printf "counter-validate: the LATENCY-bound loop reports IPC %.2f -- above 1.6 it is not measuring a serial chain\n", a; exit 1 }
    if (b < 2.5)  { printf "counter-validate: the THROUGHPUT-bound loop reports IPC %.2f -- below 2.5 it cannot separate the two\n", b; exit 1 }
    if (b / a < 2.0) { printf "counter-validate: ilp/chain is %.2f -- the counter does not separate the bottlenecks\n", b/a; exit 1 }
  }' || return 1

  green "counter-validate: the counter separates latency from throughput"
}

# --- speedtest-check: the one command whose OUTPUT nothing else can pin ------------
#
# `speedtest` reports a throughput, so every number in it is a property of this
# machine at this moment: no golden can hold it, and `normalize()` elides nothing that
# would make one possible. That is exactly the shape of surface this tree has twice
# found rotting -- an output path no instrument reads -- so what CAN be asserted is
# asserted: the run completes, it drives every position in the table, and the report
# carries all sixteen of its fields with a positive node count under them.
#
# The workload is the table's own, at the smallest budget that still runs it: one
# second across 258 positions. That is ~4 ms per search, which measures nothing about
# throughput and everything about whether the command works.
#
# The POSITION COUNT is the sharp half. It comes from the table rather than from a
# literal here, so a game dropped from `speedtest_positions.c` -- which no other gate
# reads at all -- fails this instead of quietly shortening the run.
do_speedtest_check() {
  need_binary

  local out expected
  expected=$(grep -cE '^    "' src/shell/speedtest_positions.c)
  info "speedtest-check: the report's shape over $expected positions"

  # stderr carries the whole report; stdout carries only the option echo.
  out=$(engine speedtest 1 8 1 2>&1 >/dev/null | tr '\r' '\n') || {
    red "speedtest-check: the run did not complete"; return 1; }

  local fails=0 label
  for label in "Version" "Compiled by" "Compilation architecture" "Compilation settings" \
               "Compiler __VERSION__ macro" "Large pages" "User invocation" \
               "Filled invocation" "Available processors" "Thread count" "Thread binding" \
               "TT size \[MiB\]" "Hash max, avg \[per mille\]" "Total nodes searched" \
               "Total search time \[s\]" "Nodes/second"; do
    grep -qE "^$label +:" <<< "$out" || { red "  missing report field: $label"; fails=$((fails + 1)); }
  done

  # The invocation echo is two different strings and the difference is the feature:
  # what was typed against what ran. `1 8 1` fills every field, so here they agree.
  grep -qF "User invocation            : speedtest 1 8 1" <<< "$out" \
    || { red "  the user invocation is not echoed back"; fails=$((fails + 1)); }
  grep -qF "Filled invocation          : speedtest 1 8 1" <<< "$out" \
    || { red "  the filled invocation is not echoed back"; fails=$((fails + 1)); }

  local last nodes
  last=$(grep -oE '^Position [0-9]+/[0-9]+' <<< "$out" | tail -1)
  if [[ $last != "Position $expected/$expected" ]]; then
    red "  the run ended at '$last', not at position $expected/$expected --"
    red "  either it stopped early or the position table changed size"
    fails=$((fails + 1))
  fi

  nodes=$(grep -E '^Total nodes searched' <<< "$out" | grep -oE '[0-9]+$')
  if [[ -z ${nodes:-} || $nodes -le 0 ]]; then
    red "  the report claims ${nodes:-no} nodes searched -- it measured nothing"
    fails=$((fails + 1))
  fi

  [[ $fails -eq 0 ]] || { red "speedtest-check: $fails problem(s)"; return 1; }
  green "speedtest-check: $expected positions, 16 report fields, $nodes nodes"
}

do_async_check() {
  need_binary
  # The METAMORPHIC half of the async surface. `stop` and `ponderhit` have transcript
  # cases (tools/cases/transcript/), and those adjudicate what piped driving produces:
  # the command overtakes the search and both engines return the same cut-short answer.
  # What no byte-golden can hold is a stop that lands inside a RUNNING search -- it ends
  # wherever the clock got to, and the final info line's node count moves run to run
  # (measured: 443388 then 460932 on two consecutive runs of one binary).
  #
  # So this gate asserts INVARIANTS instead of values, which needs no reference at all:
  # a search that is stopped still answers with exactly one legal bestmove and leaves an
  # engine that is still alive. Those hold whatever the clock did. This is not an
  # mcfish-authored expectation of upstream's OUTPUT -- it is a property of the UCI
  # contract, and it is the only instrument that reaches the interrupted-search path.
  info "async-check: stop/ponderhit invariants on a RUNNING search"

  local legal
  legal=$(printf 'position startpos\ngo perft 1\nquit\n' \
          | ( cd "$ROOT/$RESOURCES_DIR" && "$ROOT/$BIN" ) 2>&1 \
          | grep -oE '^[a-h][1-8][a-h][1-8][qrbn]?:' | tr -d ':')
  [[ -n $legal ]] || { red "async-check: could not read the legal move list -- rig fault"; return 2; }

  local fails=0

  # 1. A stop inside a running search: exactly one bestmove, and it is legal.
  local out bm n
  out=$({ printf 'position startpos\ngo infinite\n'; sleep 2; printf 'stop\nisready\nquit\n'; sleep 1; } \
        | ( cd "$ROOT/$RESOURCES_DIR" && timeout 60 "$ROOT/$BIN" ) 2>&1 || true)
  n=$(grep -cE '^bestmove ' <<< "$out" || true)
  bm=$(grep -E '^bestmove ' <<< "$out" | head -1 | awk '{print $2}' || true)
  if [[ $n -ne 1 ]]; then
    red "  stop: expected exactly one bestmove, got $n"; fails=$((fails + 1))
  elif ! grep -qx -- "$bm" <<< "$legal"; then
    red "  stop: bestmove '$bm' is not legal in the position"; fails=$((fails + 1))
  elif ! grep -q '^readyok' <<< "$out"; then
    red "  stop: the engine did not answer isready afterwards"; fails=$((fails + 1))
  else
    printf '  \033[32mok\033[0m    stop ended a running search with one legal bestmove (%s)\n' "$bm"
  fi

  # 2. A bare stop with no search running emits NO bestmove and leaves the engine up.
  #    Upstream ignores it; an engine that answered here would be inventing a move.
  out=$(printf 'position startpos\nstop\nisready\nquit\n' \
        | ( cd "$ROOT/$RESOURCES_DIR" && timeout 30 "$ROOT/$BIN" ) 2>&1 || true)
  # `grep -c` exits 1 on zero matches and zero is what this probe ASSERTS, so the
  # `|| true` is load-bearing: without it `set -e` turns the passing case into a fail.
  n=$(grep -cE '^bestmove ' <<< "$out" || true)
  if [[ $n -ne 0 ]]; then
    red "  idle stop: emitted $n bestmove line(s) with no search running"; fails=$((fails + 1))
  elif ! grep -q '^readyok' <<< "$out"; then
    red "  idle stop: the engine did not answer isready afterwards"; fails=$((fails + 1))
  else
    printf '  \033[32mok\033[0m    a stop with no search running answers nothing and stays up\n'
  fi

  # 3. quit during a running search terminates. The timeout is the assertion: before
  #    `go` ran off the UCI thread this would have hung, and a hang in CI reads as an
  #    infrastructure flake rather than as the engine ignoring quit.
  local rc=0
  { printf 'position startpos\ngo infinite\n'; sleep 2; printf 'quit\n'; } \
    | ( cd "$ROOT/$RESOURCES_DIR" && timeout 30 "$ROOT/$BIN" ) > /dev/null 2>&1 || rc=$?
  if [[ $rc -eq 124 ]]; then
    red "  quit: the engine did not exit within 30s of quit during a search"; fails=$((fails + 1))
  else
    printf '  \033[32mok\033[0m    quit during a running search exits\n'
  fi

  # 4+5. THE ZERO-WAIT SHAPE, which the three above cannot reach: `stop` / `quit` in
  #      the SAME input buffer as the `go` they interrupt, with no sleep between them.
  #      Every invariant above pauses two seconds first, so all three probe a search
  #      that is definitely running; the shape every harness and every one of these
  #      gates actually uses is a single `printf` piped in at once, where the
  #      interrupting command is readable before the search has begun.
  #
  #      mcfish answers both today, by construction rather than by luck: `execute`
  #      dispatches one line at a time on the UCI thread, and `search_go_start` clears
  #      the stop flag there -- before it returns, so before `stop` can be read. That
  #      is exactly why it needs a gate. Move the clear onto the worker (a plausible
  #      refactor of the dispatch) and the interrupting command is swallowed by the
  #      search it was meant to end, while invariants 1-3 stay green because their
  #      sleep puts them after the clear either way.
  #
  #      Taken from ../rfish, which HANGS on the quit form: its reader thread tested
  #      whether the search was unbounded at the moment it read the line, and piped
  #      input puts `quit` in the buffer ahead of the `go` being dispatched. Not a bug
  #      here -- but nothing here could have seen it either.
  out=$(printf 'uci\nisready\nposition startpos\ngo infinite\nstop\nisready\nquit\n' \
        | ( cd "$ROOT/$RESOURCES_DIR" && timeout 30 "$ROOT/$BIN" ) 2>&1 || true)
  n=$(grep -cE '^bestmove ' <<< "$out" || true)
  bm=$(grep -E '^bestmove ' <<< "$out" | head -1 | awk '{print $2}' || true)
  if [[ $n -ne 1 ]]; then
    red "  zero-wait stop: expected exactly one bestmove, got $n"; fails=$((fails + 1))
  elif ! grep -qx -- "$bm" <<< "$legal"; then
    red "  zero-wait stop: bestmove '$bm' is not legal in the position"; fails=$((fails + 1))
  else
    printf '  \033[32mok\033[0m    a stop read before the search starts still ends it (%s)\n' "$bm"
  fi

  rc=0
  printf 'uci\nisready\nposition startpos\ngo infinite\nquit\n' \
    | ( cd "$ROOT/$RESOURCES_DIR" && timeout 30 "$ROOT/$BIN" ) > /dev/null 2>&1 || rc=$?
  if [[ $rc -eq 124 ]]; then
    red "  zero-wait quit: the engine did not exit within 30s of a piped quit"; fails=$((fails + 1))
  else
    printf '  \033[32mok\033[0m    a quit in the same buffer as the go it interrupts exits\n'
  fi

  # 6. THE WEDGE. `setoption` arriving during `go infinite` is upstream's own defect:
  #    its setoption waits for the search to finish, the main worker spins on
  #    `!stop && (ponder || infinite)`, and only the UCI thread can set `stop` -- which
  #    is now blocked inside setoption. Neither `stop` nor `quit` is ever read again,
  #    and the process has to be killed. Any GUI that pushes an option mid-ponder
  #    hits it.
  #
  #    This tree does not wedge, and it is worth being precise about WHY, because the
  #    reason is not the one a reader expects: the dispatch ends a running search
  #    BEFORE applying any mutating command, and `end_search` STOPS an unbounded
  #    search rather than waiting for it. A port that keeps the wait and drops the
  #    stop -- which is upstream's shape, and the obvious way to write it -- wedges
  #    exactly here while every invariant above stays green, because all five of them
  #    interrupt with `stop` or `quit`, the two commands that are dispatched BEFORE
  #    the end-search call and so never reach it.
  #
  #    The timeout is the assertion. A wedge is indistinguishable from a slow engine
  #    without one, which is the property that let this defect stand upstream.
  rc=0
  out=$({ printf 'position startpos\ngo infinite\n'; sleep 2
          printf 'setoption name Hash value 32\nisready\n'; sleep 1
          printf 'quit\n'; sleep 1; } \
        | ( cd "$ROOT/$RESOURCES_DIR" && timeout 30 "$ROOT/$BIN" ) 2>&1) || rc=$?
  n=$(grep -cE '^bestmove ' <<< "$out" || true)
  bm=$(grep -E '^bestmove ' <<< "$out" | head -1 | awk '{print $2}' || true)
  if [[ $rc -eq 124 ]]; then
    red "  setoption wedge: the engine never answered -- a permanent wedge"; fails=$((fails + 1))
  elif [[ $n -ne 1 ]]; then
    red "  setoption wedge: expected exactly one bestmove, got $n"; fails=$((fails + 1))
  elif ! grep -qx -- "$bm" <<< "$legal"; then
    red "  setoption wedge: bestmove '$bm' is not legal in the position"; fails=$((fails + 1))
  elif ! grep -q '^readyok' <<< "$out"; then
    red "  setoption wedge: the engine did not answer isready afterwards"; fails=$((fails + 1))
  else
    printf '  \033[32mok\033[0m    a setoption during go infinite does not wedge (%s)\n' "$bm"
  fi

  # 7. The other half, and the one a fix for 6 can break: a BOUNDED search followed by
  #    a mutating command must still run to completion. Stopping every search on the
  #    way to applying an option is the obvious fix for the wedge and it truncates
  #    this -- a sibling port took exactly that route and collapsed a golden to
  #    `nodes 0`. Here the search is waited out and only an unbounded one is stopped,
  #    so the node count is the SAME with and without the trailing command, which is
  #    the sharp form of the claim.
  local with without
  with=$(printf 'position startpos\ngo depth 10\nsetoption name Hash value 32\nquit\n' \
         | ( cd "$ROOT/$RESOURCES_DIR" && timeout 60 "$ROOT/$BIN" ) 2>&1 \
         | grep -E '^info depth 10 ' | tail -1 | grep -oE 'nodes [0-9]+' || true)
  without=$(printf 'position startpos\ngo depth 10\nquit\n' \
            | ( cd "$ROOT/$RESOURCES_DIR" && timeout 60 "$ROOT/$BIN" ) 2>&1 \
            | grep -E '^info depth 10 ' | tail -1 | grep -oE 'nodes [0-9]+' || true)
  if [[ -z $without ]]; then
    red "  bounded truncation: the control search did not reach depth 10"; fails=$((fails + 1))
  elif [[ $with != "$without" ]]; then
    red "  bounded truncation: a trailing setoption cut the search ($with vs $without)"
    fails=$((fails + 1))
  else
    printf '  \033[32mok\033[0m    a bounded search is not truncated by a following setoption (%s)\n' \
           "$without"
  fi

  [[ $fails -eq 0 ]] || { red "async-check: $fails invariant(s) broken"; return 1; }
  green "async-check: 7 of 7 invariants hold on the interrupted-search path"
}

do_fixture_coverage() {
  info "fixture-coverage: property list vs the fixture sets"
  bash tools/fixture_coverage.sh
}

do_negative_control() {
  local want=("$@")
  NEG_BACKUP_DIR=$(mktemp -d)
  trap negative_control_restore EXIT

  local pass=0 fail=0 ran=0
  local held=()
  local row label file script gate
  for row in "${NEGATIVE_CONTROL_ROWS[@]}"; do
    # Fields are %-separated because every sed script here contains the characters a
    # more obvious separator would use. The script itself is everything between the
    # file and the LAST field, so it may contain % nowhere -- keep it that way.
    label=${row%%\%*}
    local rest=${row#*%}
    file=${rest%%\%*}
    rest=${rest#*%}
    local mode=${rest##*\%}
    rest=${rest%\%*}
    script=${rest%\%*}
    gate=${rest##*\%}

    if [[ ${#want[@]} -gt 0 ]]; then
      local keep=0 w
      for w in "${want[@]}"; do [[ $w == "$gate" ]] && keep=1; done
      [[ $keep == 0 ]] && continue
    elif [[ $mode == hold ]]; then
      held+=("$gate")
      continue
    fi

    ran=$((ran + 1))
    info "negative-control: $gate -- $label"

    mkdir -p "$NEG_BACKUP_DIR/$(dirname "$file")"
    cp "$file" "$NEG_BACKUP_DIR/$file"
    sed -i "$script" "$file"
    if cmp -s "$file" "$NEG_BACKUP_DIR/$file"; then
      red "  the mutation did not apply -- '$script' matches nothing in $file."
      red "  The pattern has rotted; this is a RIG FAULT, not a gate verdict."
      negative_control_restore
      trap - EXIT
      return 2
    fi

    local rc=0
    do_build > /dev/null 2>&1 || rc=$?
    if [[ $rc -ne 0 ]]; then
      red "  the mutated tree does not COMPILE -- the mutation is not behavioural."
      negative_control_restore
      trap - EXIT
      return 2
    fi

    # BOUND THE MUTATED RUN. A mutant is a deliberately broken engine, and a broken
    # engine does not always fail fast -- the scalar-min mutant turns the activation
    # clamp into its opposite, and the resulting evaluation made `bench` run for over
    # 25 minutes without returning. A gate that never answers is not a gate that
    # failed, so a timeout is a RIG FAULT here and never a verdict: reporting it as a
    # detection would credit the gate for an experiment that never finished.
    rc=0
    timeout "$NEG_GATE_TIMEOUT" bash -c "cd '$ROOT' && ./build.sh $gate" > /dev/null 2>&1 || rc=$?
    if [[ $rc -eq 124 ]]; then
      red "  the mutated $gate did not finish within ${NEG_GATE_TIMEOUT}s -- RIG FAULT."
      red "  A mutant that hangs proves nothing. Choose one whose cost is bounded, or"
      red "  raise NEG_GATE_TIMEOUT if this gate is legitimately that slow."
      cp "$NEG_BACKUP_DIR/$file" "$file"
      negative_control_restore
      trap - EXIT
      do_build > /dev/null 2>&1 || true
      return 2
    fi
    if [[ $rc -eq 0 ]]; then
      red "  FAIL  $gate PASSED a mutated engine -- it cannot see this class of defect"
      fail=$((fail + 1))
    else
      printf '  \033[32mok\033[0m    %s went red (exit %d)\n' "$gate" "$rc"
      pass=$((pass + 1))
    fi

    cp "$NEG_BACKUP_DIR/$file" "$file"
    rm -f "$NEG_BACKUP_DIR/$file"
  done

  negative_control_restore
  trap - EXIT

  [[ $ran -gt 0 ]] || { red "negative-control: no rows selected -- compared NOTHING"; return 2; }

  # Restore the binary the mutations built over, and prove the tree is clean again by
  # running one gate green rather than by asserting it.
  do_build > /dev/null 2>&1 || { red "negative-control: the restored tree does not build"; return 1; }
  local rc=0
  do_signature > /dev/null 2>&1 || rc=$?
  [[ $rc -eq 0 ]] || { red "negative-control: the tree did NOT come back clean (signature exit $rc)"; return 1; }

  [[ $fail -eq 0 ]] || { red "negative-control: $fail of $ran gate(s) passed a mutated engine"; return 1; }
  [[ ${#held[@]} -eq 0 ]] || info "negative-control: HELD (not run by default): ${held[*]} -- see the row table"
  green "negative-control: $ran of $ran gate(s) detected their mutation, tree restored"
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
  # The net probe is `bench 1 1 1`, not `bench 1`. Both answer the only question here --
  # did the net load -- but `bench 1` sets HASH to 1 MB and then runs the FULL depth-13
  # suite, so this gate used to run two complete benches on the slowest binary in the
  # tree to ask a startup question. Measured on the vector build: 11.06s against 0.24s.
  info "simd-scalar: rebuilding with MCFISH_SIMD_SCALAR and re-asserting the anchor"
  mkdir -p build

  # Delete the previous binary FIRST, and check the compile's exit status.
  #
  # Neither was done, and together they made this gate unable to fail: a build
  # error left `build/mcfish-scalar` holding the last GOOD binary, which then
  # benched the anchor and reported "vector and scalar paths agree". The gate was
  # green on a tree that did not compile. It surfaced only on a clean CI checkout,
  # where no stale binary existed and the node count came back empty -- so the
  # machine with no history caught what the developer's box hid.
  rm -f build/mcfish-scalar
  if ! "$CC" "${CFLAGS_COMMON[@]}" "${CFLAGS_RELEASE[@]}" -DMCFISH_SIMD_SCALAR \
       -o build/mcfish-scalar "${SOURCES[@]}" -lm -lpthread; then
    red "simd-scalar: the scalar build does not compile — the gate did NOT run."
    return 1
  fi

  local net_probe
  net_probe=$(engine_at build/mcfish-scalar bench 1 1 1 2>&1 || true)
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
  #
  # Name every tier. `native` used to sit at the end of this list as the only way to
  # reach the widest code the host could run; it is an ALIAS for one of these now, so
  # listing it would build a duplicate and test nothing the alias target does not.
  local expected tiers=(sse41)
  expected=$(grep -v '^#' tools/signature.golden | tr -d '[:space:]')
  grep -qw avx2 /proc/cpuinfo && tiers+=(avx2)
  grep -qw avx512f /proc/cpuinfo && tiers+=(avx512)
  grep -qw avx512_vnni /proc/cpuinfo && tiers+=(vnni512)
  grep -qw avx512_vbmi2 /proc/cpuinfo && grep -qw avx512_bitalg /proc/cpuinfo \
    && grep -qw avx512_vnni /proc/cpuinfo && tiers+=(avx512icl)

  info "arch-determinism: ${tiers[*]} must all bench $expected"
  local tier actual failed=0
  for tier in "${tiers[@]}"; do
    MCFISH_ARCH=$tier BIN=build/mcfish-$tier "$0" build > /dev/null || { red "$tier: build failed"; failed=1; continue; }
    actual=$(engine_at "build/mcfish-$tier" bench 2>&1 >/dev/null | grep 'Nodes searched' | awk '{print $NF}')
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
  # The optional fourth field is the VARIANT, and it cannot be inferred from the FEN.
  # `Position::set` takes the flag, and it decides whether the castling rook is tested
  # for screening its own king (position.cpp:686) -- so a Shredder-FEN row driven
  # without it walks a different legality path than the one it was written for.
  while IFS='|' read -r fen depth expected variant; do
    [[ $fen =~ ^#.*$ || -z $fen ]] && continue
    local got setup=""
    [[ ${variant-} == 960 ]] && setup=$'setoption name UCI_Chess960 value true\n'
    got=$(printf '%sposition fen %s\ngo perft %s\nquit\n' "$setup" "$fen" "$depth" \
          | engine | grep 'Nodes searched' | awk '{print $NF}')
    if [[ $got == "$expected" ]]; then
      printf '  ok   depth %s  %s%s\n' "$depth" "${fen:0:40}" "${variant:+  [$variant]}"
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
# The golden-diff normalization is one function shared with
# tools/upstream_golden_audit.sh, so it lives in a file both can source rather
# than in a file the other has to parse. See tools/lib/normalize.sh.
. "$ROOT/tools/lib/normalize.sh"

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

  # EXPIRE normalize()'s ONE declared gap. Its other substitutions elide what the
  # MACHINE decides -- timings, the toolchain, the processor list, the NUMA suffix --
  # and those can never retire. The replica backing is different: it is elided because
  # mcfish holds the net in ordinary process memory and says `Local memory.` where
  # upstream maps it system-wide, and that difference RETIRES the day the network
  # registers for NUMA replication. An elision whose subject has gone stops comparing
  # real output while still reading as a declared gap, so the subject is asserted here
  # rather than remembered: normalize()'s own header says to delete the line first and
  # let the gate go red, which is a process control where this is a mechanical one.
  local raw_replica
  raw_replica=$({ engine < "$ROOT/tools/cases/newgame.uci" 2>&1; } | grep -c 'Network replica' || true)
  if [[ ${raw_replica:-0} -eq 0 ]]; then
    red "golden: normalize() elides the replica BACKING word, and mcfish no longer prints"
    red "  a 'Network replica' line at all. The elision now has no subject -- either the"
    red "  line was lost (a regression the elision is hiding) or replication landed and"
    red "  the elision must be deleted so the two sides are compared in full."
    return 1
  fi

  green "golden gate passed"
}

# REFUSE A GOLDEN THE ENGINE PRODUCED, unless someone says so out loud.
#
# This step drives MCFISH and writes what it printed. That makes every golden it
# touches a PHOTOGRAPH OF MCFISH: it pins a defect exactly as faithfully as it pins
# correct behaviour, and the gate then passes BECAUSE the engine is wrong. It has
# already happened here -- `board.golden` recorded a `d` with no `Checkers:` line and
# `errors.golden` recorded three invalid FENs producing no diagnostic at all, both
# green for as long as they existed (tools/GOLDEN_PROVENANCE.md).
#
# `./build.sh golden-audit --write` is the regenerator to reach for: it drives the
# pristine ORACLE, so the golden it leaves behind is adjudicated by construction. The
# two commands sit one keystroke apart and their diffs look identical, which is
# exactly why the wrong one needs to be harder to run than the right one.
#
# The escape hatch stays, because one case can legitimately need it -- a case upstream
# cannot be driven through at all -- and a gate with no override gets worked around
# rather than argued with. It just has to be deliberate.
do_golden_update() {
  if [[ ${MCFISH_GOLDEN_UPDATE_FROM_MCFISH:-0} != 1 ]]; then
    red "golden-update drives MCFISH, so it writes a photograph of mcfish, not a reference."
    red ""
    red "  Use this instead -- it drives the pristine oracle:"
    red "      ./build.sh golden-audit --write            # every case that differs"
    red "      ./build.sh golden-audit --write <case>     # just one"
    red ""
    red "  If this case genuinely cannot be driven through upstream, say so on purpose:"
    red "      MCFISH_GOLDEN_UPDATE_FROM_MCFISH=1 ./build.sh golden-update"
    red "  and record WHY in the commit body -- tools/GOLDEN_PROVENANCE.md is the page."
    return 2
  fi
  red "golden-update: writing goldens FROM MCFISH by explicit override."
  red "  Every golden written here is a photograph of this binary, not upstream's bytes."
  do_golden_update_impl
}

do_golden_update_impl() {
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

# -DMCFISH_ACC_STATS is passed HERE and to `tsan`, and to no other build.
#
# It compiles the accumulator's path counters in. The four ways up to the top slot
# agree on every value they produce, so a step that stops running is answered
# correctly by its own fallback and passes the anchor, the goldens and the
# differential alike. The suite asserts each path was TAKEN as well as that it
# agreed, and it cannot do that without a counter. The release binary keeps none:
# the increment would sit in the hottest function in the engine.
do_test() {
  info "unit + property tests"
  mkdir -p build
  # -fno-sanitize-recover=undefined, and the same halt_on_error the CI lane sets.
  #
  # WITHOUT THESE THIS STEP IS WEAKER THAN CI AND LIES BY OMISSION. UBSan's default
  # is to print a diagnostic and CARRY ON, so a real finding scrolled past in the
  # middle of the suite's output and the step still exited 0. That is not a
  # hypothetical: a null-pointer memcpy in worker_root_setup printed here twice,
  # exited 0, was committed, and turned the blocking CI lane red -- where the same
  # binary aborts because the workflow exports halt_on_error=1. A gate that is
  # softer than CI trains you to trust an exit code that does not mean what CI
  # means by it. Keep these in step with .github/workflows/mcfish_parity.yml.
  "$CC" "${CFLAGS_COMMON[@]}" -O1 -g -fsanitize=address,undefined \
    -fno-sanitize-recover=undefined -DMCFISH_ACC_STATS \
    -o build/mcfish-test "${ENGINE_SOURCES[@]}" tests/test_main.c -lm -lpthread
  ASAN_OPTIONS=detect_leaks=1:abort_on_error=1:print_stacktrace=1 \
  UBSAN_OPTIONS=halt_on_error=1:abort_on_error=1:print_stacktrace=1 \
    ./build/mcfish-test
}

# Assert a fuzz lane EXECUTED, rather than merely exiting 0.
#
# A libFuzzer lane that stalls still exits 0. That is not hypothetical here:
# `fuzz-search` spent months green while executing three inputs per ninety
# seconds, because llvm-symbolizer was serialising the run (see -print_funcs
# below). The exit code cannot tell "found nothing" from "fuzzed nothing", and
# only the first is worth reporting. zfish hit the same blind spot from the other
# side -- its fuzzer prints no total at all, so it decodes the coverage file's
# header instead (zfish b9aa6d03).
#
# libFuzzer does print the total, under -print_final_stats, so read that. The
# floor is a RATE times the budget, set at roughly 1-2% of what each lane clears
# locally: low enough that a slow runner passes, high enough that a stalled lane
# cannot. A missing count is a failure too -- it means the run never reached its
# own summary.
assert_fuzz_executed() {
  local label=$1 log=$2 per_sec=$3 seconds=$4
  local n floor
  n=$(grep -oE 'stat::number_of_executed_units: *[0-9]+' "$log" | grep -oE '[0-9]+$' | tail -1)
  floor=$((per_sec * seconds))
  [[ $floor -lt 1 ]] && floor=1
  if [[ -z $n ]]; then
    red "$label: no execution count in the run — the lane never reached its summary"
    return 1
  fi
  if [[ $n -lt $floor ]]; then
    red "$label: executed $n inputs, floor $floor — the lane ran but fuzzed nothing"
    return 1
  fi
  info "$label executed $n inputs (floor $floor)"
}

# Assert a fuzz lane REACHED the code it exists to cover, rather than merely
# executing.
#
# `assert_fuzz_executed` counts what libFuzzer ran. It cannot tell a decoder
# exercised thousands of times from a `set` that refused every file: the whole-file
# driver supplies the magic itself, so an input dying on a length check counts as an
# execution exactly like one that walked the decoder. The floor was therefore
# insensitive to the one regression it exists to catch -- a parse that starts
# refusing everything reads as a clean run.
#
# tools/fuzz_tb_file.c prints its own three counts on exit and this holds each to a
# floor. PARSED comes from the registry, not from the probe, because a probe reports
# one FAIL whether the parse refused the file or the decoder declined to answer, and
# adding those two rates together is how the distinction gets lost. The line missing
# entirely is a failure too: it means the run never reached its own summary.
#
# ../rfish dd0df54 and ../zfish 741f8ffc found the same defect in their own sweeps.
assert_fuzz_reached() {
  local log=$1 parsed_per_sec=$2 answered_per_sec=$3 seconds=$4
  local line rounds parsed answered floor_p floor_a
  line=$(grep -E '^fuzz-tb-file: rounds [0-9]+ parsed [0-9]+ answered [0-9]+$' "$log" | tail -1)
  if [[ -z $line ]]; then
    red "fuzz-tb whole-file lane: no reach counts in the run -- it never printed its summary"
    return 1
  fi
  read -r rounds parsed answered < <(awk '{print $3, $5, $7}' <<< "$line")

  floor_p=$((parsed_per_sec * seconds)); [[ $floor_p -lt 1 ]] && floor_p=1
  floor_a=$((answered_per_sec * seconds)); [[ $floor_a -lt 1 ]] && floor_a=1

  if [[ $parsed -lt $floor_p ]]; then
    red "fuzz-tb whole-file lane: $parsed file(s) parsed, floor $floor_p -- the lane"
    red "  wrote and executed, but the parse accepted almost nothing, so the decoder"
    red "  and everything below it went unexercised."
    return 1
  fi
  if [[ $answered -lt $floor_a ]]; then
    red "fuzz-tb whole-file lane: $answered probe(s) answered, floor $floor_a -- files"
    red "  parsed but no probe produced a value, so no consumer of one ran."
    return 1
  fi
  info "fuzz-tb whole-file lane reached $rounds rounds / $parsed parsed / $answered answered"
  info "  (floors $floor_p parsed, $floor_a answered)"
}

# Coverage-guided, in-process fuzzing of the real search -- the gap
# tools/uci_fuzz.py cannot close, because that lane drives the shipped binary's
# stdin over a pipe, so a mutation spends most of its budget on the UCI parser
# rather than the search. tools/fuzz_search.c links ENGINE_SOURCES against
# libFuzzer's driver instead of a stub main; see that file's header for what one
# iteration does (a random-legal-move walk from the start position, then a
# shallow search_go).
#
# clang-only: libFuzzer has no gcc equivalent, unlike every sanitizer flag used
# elsewhere in this file.
#
# Kept OUT of `parity`, same reason as tsan: its own build and a real time
# budget, and a clean run only means "no crash was FOUND in that budget," not
# "there is none." Run it by hand, seconds argument optional (default 30).
do_fuzz_search() {
  local seconds=${1:-30}
  info "in-process search fuzzing: ${seconds}s"
  mkdir -p build

  local name
  name=$(grep -oE 'nn-[0-9a-f]+\.nnue' src/engine/eval/nnue/network.h | head -1)
  if [[ -n $name && -f "$RESOURCES_DIR/$name" ]]; then
    info "net found in $RESOURCES_DIR/: covers the NNUE accumulator path too"
  else
    info "no net in $RESOURCES_DIR/: covers the classical fallback only (see ./build.sh net)"
  fi

  "$CC" "${CFLAGS_COMMON[@]}" -O1 -g -fsanitize=fuzzer,address,undefined \
    -o build/mcfish-fuzz-search "${ENGINE_SOURCES[@]}" tools/fuzz_search.c -lm -lpthread

  # -print_funcs=0: see do_fuzz_tb. This lane is where it actually mattered --
  # the engine has enough functions that the symbolizer never stopped: 30s of
  # budget executed THREE inputs, and the nightly 600s job was fuzzing nothing.
  # `set -o pipefail` is on, so the tee does not mask the fuzzer's exit code —
  # the trap CONTRIBUTING.md warns about needs a pipeline without it.
  ./build/mcfish-fuzz-search -max_total_time="$seconds" -print_funcs=0 -print_final_stats=1 \
    2>&1 | tee build/fuzz-search.log
  assert_fuzz_executed "fuzz-search" build/fuzz-search.log 2 "$seconds"
  green "fuzz-search clean: ${seconds}s, no crash found"
}

# Coverage-guided fuzzing of the Syzygy table parse, in two lanes.
#
# The complement to `fuzz-search` on the other untrusted input: SyzygyPath names
# a BINARY file the engine did not write, and every offset the parse advances
# comes out of that file.
#
#   parse  tools/fuzz_tb_parse.c -- decode_set_sizes and decode_pairs called
#          directly, linking three files rather than ENGINE_SOURCES because the
#          decoder cluster depends on nothing but libc. Hundreds of thousands of
#          iterations a second, and the only lane fast enough to explore header
#          SHAPES, but it reaches the decoder by reimplementing registry.c's
#          carve -- which is a claim about the code under test.
#   file   tools/fuzz_tb_file.c -- a real file, tablebase_init over a real
#          SyzygyPath, a real probe. Thousands of times slower and worth it:
#          it is the only lane that runs `set`, `set_groups`, `set_dtz_map` and
#          map_file at all, and it tests the carve instead of asserting it.
#
# Neither subsumes the other, so both run and both must be clean. -timeout: a
# corrupt btree used to be able to make the descent run forever, so a hang is a
# finding here and libFuzzer must be told to call it one rather than sit on it.
#
# clang-only and kept OUT of `parity`, for the same reasons as `fuzz-search`.
do_fuzz_tb() {
  local seconds=${1:-30}
  mkdir -p build

  "$CC" "${CFLAGS_COMMON[@]}" -O1 -g -fsanitize=fuzzer,address,undefined \
    -fno-sanitize-recover=undefined \
    -o build/mcfish-fuzz-tb \
    src/platform/syzygy/decode.c src/platform/syzygy/tables.c \
    src/platform/syzygy/encode.c tools/fuzz_tb_parse.c

  "$CC" "${CFLAGS_COMMON[@]}" -O1 -g -fsanitize=fuzzer,address,undefined \
    -fno-sanitize-recover=undefined \
    -o build/mcfish-fuzz-tb-file "${ENGINE_SOURCES[@]}" tools/fuzz_tb_file.c \
    -lm -lpthread

  # -print_funcs=0: libFuzzer symbolizes every newly-covered function to name it
  # in the log, and llvm-symbolizer costs SECONDS per call here. Left on, a 45s
  # budget took 90s of wall clock and executed 41 inputs; off, the same budget
  # runs 9 million. The names buy nothing a crash report does not already print.
  info "Syzygy parse fuzzing, decoder lane: ${seconds}s"
  ./build/mcfish-fuzz-tb \
    -max_total_time="$seconds" -timeout=8 -print_funcs=0 -print_final_stats=1 \
    2>&1 | tee build/fuzz-tb.log
  assert_fuzz_executed "fuzz-tb decoder lane" build/fuzz-tb.log 2000 "$seconds"

  # Seed the whole-file lane from the real 3-man tables when they are present.
  # Mutating a table that PARSES is worth far more than mutating noise -- the
  # same principle tools/uci_fuzz.py applies to almost-valid commands -- and a
  # 3-man file already satisfies the length rule, so `[stem][len][bodies]`
  # reconstructs both files byte for byte. Without the tables the lane still runs
  # unseeded, and says so rather than implying it was seeded. The directory is
  # libFuzzer's output corpus too, so it accumulates across runs -- which is why
  # the message is about the SEED and not about the corpus being empty.
  local corpus=build/fuzz-tb-corpus
  mkdir -p "$corpus"
  local seeded=0 stem sel w z wlen
  for sel in 0 1; do
    stem=$([[ $sel -eq 0 ]] && echo KQvK || echo KPvK)
    w="$TB_DIR/$stem.rtbw"
    z="$TB_DIR/$stem.rtbz"
    [[ -s $w && -s $z ]] || continue
    wlen=$(($(stat -c%s "$w") - 4))
    # shellcheck disable=SC2059  # the format string IS the payload: four octal escapes, built here
    printf "$(printf '\\%03o' "$sel")$(printf '\\%03o' $(((wlen >> 16) & 0xFF)))$(printf '\\%03o' $(((wlen >> 8) & 0xFF)))$(printf '\\%03o' $((wlen & 0xFF)))" \
      > "$corpus/seed-$stem"
    tail -c +5 "$w" >> "$corpus/seed-$stem"
    tail -c +5 "$z" >> "$corpus/seed-$stem"
    seeded=$((seeded + 1))
  done
  if [[ $seeded -eq 0 ]]; then
    red "no tables in $TB_DIR: the whole-file lane runs UNSEEDED"
    red "  (./build.sh tb-fetch gives it tables that parse to mutate from)"
  else
    info "seeded the whole-file lane from $seeded real table pair(s)"
  fi

  info "Syzygy parse fuzzing, whole-file lane: ${seconds}s"
  ./build/mcfish-fuzz-tb-file \
    -max_total_time="$seconds" -timeout=8 -print_funcs=0 -print_final_stats=1 "$corpus" \
    2>&1 | tee build/fuzz-tb-file.log
  assert_fuzz_executed "fuzz-tb whole-file lane" build/fuzz-tb-file.log 2 "$seconds"
  assert_fuzz_reached build/fuzz-tb-file.log 1 1 "$seconds"

  green "fuzz-tb clean: ${seconds}s per lane, no crash, no hang"
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
    -fsanitize=thread -DMCFISH_ACC_STATS \
    -o build/mcfish-tsan "${ENGINE_SOURCES[@]}" tests/test_main.c -lm -lpthread
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
# test_main.c:719 and exit 66. Re-run that experiment rather than trusting this
# comment if a zero here ever needs to be believed.
do_tsan_search() {
  local depth=${1:-14} threads=${2:-8}
  info "full engine under ThreadSanitizer: Threads=$threads, go depth $depth"
  mkdir -p build
  "$CC" "$STD_FLAG" -Wall -Isrc -D_POSIX_C_SOURCE=200809L -O1 -g \
    -fsanitize=thread "${CFLAGS_ARCH[@]}" \
    -o build/mcfish-tsan-engine "${SOURCES[@]}" -lm -lpthread

  local log script; log=$(mktemp); script=$(mktemp)
  printf 'setoption name Threads value %d\nsetoption name Hash value 1\nucinewgame\nposition startpos\ngo depth %d\nquit\n' \
    "$threads" "$depth" > "$script"

  # Measure the thread count from the OS, not from the sanitizer's output.
  #
  # Any "Thread T<n>" string TSan prints appears only INSIDE a race report, so on a
  # clean run it is zero however many threads ran -- counting it would answer "did
  # this go parallel?" with the one number that cannot mean anything, and a
  # `Threads value N` that silently spawned nothing would pass identically.
  #
  # `exec` so $! is the engine itself rather than the subshell, then sample
  # /proc/<pid>/task while it searches and keep the peak.
  ( cd "$ROOT/$RESOURCES_DIR" && exec "$ROOT/build/mcfish-tsan-engine" ) \
    < "$script" > "$log" 2>&1 &
  local pid=$! peak=0 n
  if [[ -d /proc/$pid/task ]]; then
    while kill -0 "$pid" 2>/dev/null; do
      # Two `set -euo pipefail` hazards live in these three lines, and both showed
      # up as the gate dying with the engine's own timing rather than as a verdict:
      #   - `ls` exits 2 the moment /proc/<pid>/task stops existing, which is a
      #     normal end to the poll; under `pipefail` that status becomes the
      #     assignment's, and `set -e` takes the script down. Hence `|| n=0`.
      #   - a bare `[[ ... ]] && peak=$n` is an && list whose status is 1 on every
      #     sample that does not beat the peak, which `set -e` also treats as
      #     fatal. Hence the `if`.
      # shellcheck disable=SC2012  # /proc task entries are numeric TIDs, so `ls` cannot be confused here
      n=$(ls /proc/"$pid"/task 2>/dev/null | wc -l) || n=0
      if [[ ${n:-0} -gt $peak ]]; then
        peak=$n
      fi
    done
  else
    peak=-1  # no procfs: cannot measure, and must not claim
  fi
  wait "$pid" || true
  rm -f "$script"

  local races
  races=$(grep -c "WARNING: ThreadSanitizer" "$log" || true)

  if ! grep -q "^bestmove" "$log"; then
    red "tsan-search: the search did not complete -- this is not a clean run"
    tail -20 "$log" >&2
    rm -f "$log"
    return 1
  fi

  if [[ $races -ne 0 ]]; then
    red "tsan-search: $races race(s) reported"
    grep -A6 "WARNING: ThreadSanitizer" "$log" | head -40 >&2
    rm -f "$log"
    return 1
  fi

  # Zero races over a single-threaded run proves nothing, so make that a FAILURE
  # rather than a footnote the reader is asked to check.
  if [[ $peak -lt 0 ]]; then
    red "tsan-search: no procfs -- could not confirm the search ran multi-threaded."
    red "  0 races is unproven on this host. Treat as SKIPPED, not as a pass."
    rm -f "$log"
    return 127
  fi
  if [[ $peak -lt $threads ]]; then
    red "tsan-search: asked for Threads=$threads but the process peaked at $peak OS thread(s)."
    red "  0 races over a run that never went parallel is not a clean bill of health."
    rm -f "$log"
    return 1
  fi

  # The floor is CONCURRENCY, not worker count. `Threads 1` is a legitimate run of
  # this gate: `go` dispatches even a one-thread search onto worker 0's own OS
  # thread, so the main/worker dispatch-and-join handshake is still crossed and is
  # still a race surface (docs/04-multithreading.md). What proves nothing is a
  # process that only ever held one thread.
  if [[ $peak -lt 2 ]]; then
    red "tsan-search: the process never held more than $peak thread."
    red "  A race needs two. 0 races here is SKIPPED, not a pass."
    rm -f "$log"
    return 127
  fi

  green "tsan-search: 0 races over a depth-$depth search (peak $peak OS threads, Threads=$threads)"
  printf '  See docs/04-multithreading.md.\n'
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

# The differential that the bench anchor CANNOT fake.
#
# `signature` is one number over a fixed 51-position list. A port can be nudged
# toward it without becoming faithful -- tune a constant until the total lands,
# special-case whatever the bench happens to exercise -- and the number then says
# nothing. This reaches positions by playing random legal moves from the start,
# so they appear in no bench list, no golden and no test, then drives BOTH
# engines over them with identical commands and compares node counts per depth.
#
# Matching upstream on positions nobody tuned against is the real claim. It is
# also M1's second gate, which nothing ran until this step existed.
#
# Needs the oracle, so it is OUT of `parity`: a developer without one would see a
# skip on every run, and a gate that is usually skipped stops being read.
do_upstream_nodes() {
  info "randomised node-for-node differential vs the upstream oracle"
  python3 tools/upstream_nodes.py "$@"
}

# Report the drift between the pinned SHA and the golden checkout.
#
# ONE tracked project, and that is the point. The files under tools/upstream/
# record which Stockfish commit this tree is a clone of; nothing read them before
# this step existed, so a pin sat five commits stale while reading as
# authoritative -- a pin nobody checks is worse than no pin, because it is a false
# record rather than an absent one.
#
# ../zfish is deliberately NOT here. It is a SIBLING port of the same golden, not
# a source this tree owes a debt to, so there is no commit range to be "behind"
# and nothing to gate: most of its log is Zig work that will never have a
# counterpart, and a status line calling that a 78-commit deficit is a false alarm
# by construction. It also does not reciprocate -- zfish pins Stockfish and
# nothing else. See tools/upstream/README.md for how the two relate now.
#
# This does not gate. A tracked repository moving is normal and is not a defect
# here; the failure it catches is not NOTICING that it moved.
do_sync_status() {
  local name dir pin head ahead behind
  local rc=0
  # shellcheck disable=SC2043  # ONE tracked repo today; the loop is what lets a second be added as a row
  for pair in "Stockfish:../Stockfish:tools/upstream/UPSTREAM_BASE"; do
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

    # BOTH directions, and they are not the same finding.
    #
    # `ahead` is the tracked project having moved past the pin: normal, and the
    # resync worklist's input. `behind` is the CHECKOUT sitting before the pin,
    # which is a defect in the workspace -- ../Stockfish is the golden, so a
    # checkout behind the pin means every grep of it, and upstream_map.py's
    # fallback read, answers from source this tree has already ported past.
    # Count only the first direction and that state prints as "in sync at <pin>",
    # which is worse than silence: it asserts the thing a reader would verify.
    ahead=$(git -C "$dir" rev-list --count "$pin..HEAD")
    behind=$(git -C "$dir" rev-list --count "HEAD..$pin")

    if [[ $ahead -eq 0 && $behind -eq 0 ]]; then
      green "  $name: in sync at ${pin:0:9}"
    elif [[ $behind -ne 0 ]]; then
      # Red, and rc=1: this one is actionable here rather than a port decision.
      red "  $name: CHECKOUT IS BEHIND THE PIN by $behind commit(s)"
      red "      pinned ${pin:0:9}, $dir HEAD ${head:0:9}$([[ $ahead -ne 0 ]] && echo ' (diverged)')"
      red "      $dir is the golden -- fetch and check it out at the pin before"
      red "      comparing anything against it."
      rc=1
    else
      printf '  \033[33m%s: pin is %d commit(s) behind the checkout\033[0m  (pinned %s, HEAD %s)\n' \
        "$name" "$ahead" "${pin:0:9}" "${head:0:9}"
      git -C "$dir" log --oneline --reverse "$pin..HEAD" | sed 's/^/      /'
    fi
  done

  # UPSTREAM_TARGET is the SHA the port is aiming AT while catching up to a moving
  # upstream; UPSTREAM_BASE is what the tree claims to match today. Equal means the
  # port is not chasing anything. Until now nothing read the target at all, so a pin
  # advanced without its partner -- or a target left pointing at a commit the golden
  # no longer has -- was a file nobody would notice, which is how UPSTREAM_BASE itself
  # once sat five commits stale while reading as authoritative.
  local target_file=tools/upstream/UPSTREAM_TARGET
  if [[ ! -f $target_file ]]; then
    red "  UPSTREAM_TARGET is missing -- the pin pair is incomplete"
    rc=1
  else
    local target base
    target=$(tr -d '[:space:]' < "$target_file")
    base=$(tr -d '[:space:]' < tools/upstream/UPSTREAM_BASE)
    if [[ -z $target ]]; then
      red "  UPSTREAM_TARGET is empty"
      rc=1
    elif [[ $target == "$base" ]]; then
      green "  target: equal to the base at ${base:0:9} -- the port is chasing nothing"
    elif [[ -d ../Stockfish/.git ]] && ! git -C ../Stockfish cat-file -e "${target}^{commit}" 2>/dev/null; then
      red "  UPSTREAM_TARGET ${target:0:9} is not a commit in ../Stockfish"
      rc=1
    else
      # BOTH DIRECTIONS, as for the pin itself. A target AHEAD of the base is the
      # normal mid-catch-up state. A target the base has already passed is a defect:
      # you cannot be aiming at a commit you have ported past, and the file would
      # quietly describe a finish line behind you.
      local t_ahead t_behind
      t_ahead=$(git -C ../Stockfish rev-list --count "${base}..${target}" 2>/dev/null || echo 0)
      t_behind=$(git -C ../Stockfish rev-list --count "${target}..${base}" 2>/dev/null || echo 0)
      if [[ ${t_ahead:-0} -gt 0 ]]; then
        printf '  \033[33mtarget: %s, %s commit(s) ahead of the base -- the port is mid-catch-up\033[0m\n' \
          "${target:0:9}" "$t_ahead"
      else
        red "  UPSTREAM_TARGET ${target:0:9} is BEHIND the base by ${t_behind} commit(s)"
        red "      The base is what the tree already matches, so a target behind it"
        red "      names a finish line this port has passed. Advance it or delete it."
        rc=1
      fi
    fi
  fi

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
  info "docs-lint: links, paths, signature, symbols, step coverage"
  bash tools/docs_lint.sh
}

# --- cite-check: a cited SHA still names a commit a reader can reach ---------------
#
# docs-lint checks paths and symbols; nothing checked the ~37 commit SHAs the pages
# quote. The test is ANCESTRY, not existence: a rebase leaves its pre-rebase commits
# in the object store and a backup ref pins them forever, so `git cat-file -e`
# resolves on the author's machine and nowhere else. See tools/docs_cite.sh for the
# four tiers and why a missing sibling narrows this instead of failing it.
do_cite_check() {
  bash tools/docs_cite.sh
}

# --- shellcheck: read the language the gates are written in -----------------------
#
# Over three thousand lines of bash here decide every claim this tree makes, and
# nothing read them until this landed. The Python is linted, formatted and
# type-checked by pre-commit; the shell had nothing. See tools/shellcheck.sh for the
# scope rule, the no-baseline rule and why the version is pinned in two fields.
do_shellcheck() {
  bash tools/shellcheck.sh
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
  # shellcheck disable=SC2046  # sources_to_format emits one path per line and none of them contains a space
  "$cf" --dry-run --Werror $(sources_to_format) || return 1
  green "format clean ($cf)"
}

do_fmt_fix() {
  local cf
  cf=$(find_clang_format) || { red "clang-format not found on PATH"; return 127; }
  # shellcheck disable=SC2046  # as above: a deliberate split of a space-free path list
  "$cf" -i $(sources_to_format)
  green "formatted ($cf)"
}

# The 3-man Syzygy set the `tb` gate runs against: KPvK KNvK KBvK KRvK KQvK, WDL
# and DTZ. Never committed -- 10 binary files are a runtime input, like the net.
TB_DIR=$RESOURCES_DIR/syzygy
TB5_DIR=$RESOURCES_DIR/syzygy5
# The probing workload perf-budget-tb measures. See the file for why it exists.
TB_PROBE_FENS=tools/cases/tb_probe.fens

# `tb-fetch` gets the 3-man set (10 files, ~60 KiB). `tb-fetch 5` adds KNNvKP
# (~24 MiB), the smallest table carrying CURSED WINS -- a win whose DTZ exceeds
# 100 plies, which the fifty-move rule turns into a draw. That pair of branches in
# `map_score_dtz` and `probe_dtz` is unreachable from any 3-man table, so the `tb`
# gate cannot exercise it without this.
do_tb_fetch() {
  local want5=0
  [[ ${1:-} == 5 ]] && want5=1
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

  # shellcheck disable=SC2016  # the backticks are markdown for the reader, not a substitution
  [[ $want5 -eq 1 ]] || { printf '  (run `./build.sh tb-fetch 5` to add the 5-man cursed-win table)\n'; return 0; }

  info "fetching the 5-man cursed-win set (KNNvKP, KNNvK, KNvKP) into $TB5_DIR"
  mkdir -p "$TB5_DIR"
  # KNNvKP alone is NOT enough. The DTZ walk recurses through captures, and taking
  # the pawn reaches KNNvK -- without that child table a cursed win probes as a
  # plain draw (DTZ 0, state 0) rather than as an error, which is the failure mode
  # that makes a missing table look like a correct answer.
  local t5
  for t5 in KNNvKP KNNvK KNvKP; do
  for ext in rtbw rtbz; do
    if [[ $ext == rtbw ]]; then dir=3-4-5-wdl; magic=71e8235d; else dir=3-4-5-dtz; magic=d7660ca5; fi
    f="$TB5_DIR/$t5.$ext"
    [[ -s $f ]] && continue
    code=$(curl -sS -o "$f" -w '%{http_code}' \
      "https://tablebase.lichess.ovh/tables/standard/$dir/$t5.$ext") || code=000
    got=$(xxd -p -l 4 "$f" 2> /dev/null || true)
    if [[ $code != 200 || $got != "$magic" ]]; then
      red "  REJECT $t5.$ext (http=$code magic=${got:-none} want=$magic)"
      rm -f "$f"
      fails=$((fails + 1))
    else
      printf '  ok   %s (%s bytes)\n' "$t5.$ext" "$(stat -c%s "$f")"
    fi
  done
  done
  [[ $fails -eq 0 ]] || { red "tb-fetch: $fails 5-man file(s) failed"; return 1; }
  green "5-man cursed-win table present in $TB5_DIR"
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
    if [[ -n $tbpath ]]; then
      green "tb gate passed (discovery + root probe)"
    else
      green "tb gate passed (discovery only -- probe unexercised)"
    fi
  else
    red "tb gate: drifted from tools/tb.golden"
    return 1
  fi
}

# Re-derive tools/tb.golden from the ORACLE, never from mcfish. The oracle is run
# from its own directory, so the table path must be absolute.
# Pin the cursed-win / blessed-loss WDL+DTZ pair (M5). LOCAL ONLY: it needs the
# 5-man tables from `./build.sh tb-fetch 5`, which the 3-man set never contains,
# so it is NOT in `parity` -- a gate that is usually skipped stops being read.
#
# A cursed win is a win whose DTZ exceeds 100 plies, which the fifty-move rule
# turns into a draw. Those two branches are unreachable from any 3-man table, so
# without this they were never executed at all.
# Guard the 5-man tables both the gate and its update need. Exit 127, never 1: an
# absent table is a gate that could not run, not a gate that failed.
tb_cursed_need_tables() {
  [[ -s $TB5_DIR/KNNvKP.rtbz && -s $TB5_DIR/KNNvK.rtbz ]] || {
    red "$1: no 5-man tables in $TB5_DIR -- run './build.sh tb-fetch 5'."
    red "  NOT a pass: the cursed-win branches are UNEXERCISED without them."
    return 127
  }
}

# Derive the two halves. Both the gate and tb-cursed-update read them through these,
# so a regenerated golden cannot be produced by a different command than the one that
# checks it -- the way tb_cursed.golden's node legs drifted for three commits while
# a comment claimed the oracle derived them.
tb_cursed_probe_half() {
  local label fen
  while read -r label fen; do
    [[ -z $label || $label == \#* ]] && continue
    printf '%s %s\n' "$label" \
      "$(printf 'setoption name SyzygyPath value %s\nposition fen %s\nd\nquit\n' \
           "$ROOT/$TB5_DIR:$ROOT/$TB_DIR" "$fen" \
         | ( cd "$ROOT/$RESOURCES_DIR" && "$ROOT/$BIN" ) 2>&1 \
         | grep -E '^Tablebases' | tr '\n' '|' )"
  done < "$ROOT/tools/cases/tb_cursed.fens"
}

tb_cursed_nodes_half() {
  local nlabel nlimit nfen
  while read -r nlabel nlimit nfen; do
    [[ -z $nlabel || $nlabel == \#* ]] && continue
    printf '%s %s\n' "$nlabel" \
      "$(printf 'setoption name SyzygyPath value %s\nposition fen %s\ngo nodes %s\nquit\n' \
           "$ROOT/$TB5_DIR:$ROOT/$TB_DIR" "$nfen" "$nlimit" \
         | ( cd "$ROOT/$RESOURCES_DIR" && "$ROOT/$BIN" ) 2>&1 \
         | grep -oE 'nodes [0-9]+' | tail -1)"
  done < "$ROOT/tools/cases/tb_nodes.fens"
}

do_tb_cursed() {
  need_binary
  tb_cursed_need_tables tb-cursed || return $?
  info "tb-cursed: cursed-win / blessed-loss WDL+DTZ vs tools/tb_cursed.golden"
  local actual
  actual=$(tb_cursed_probe_half)

  if diff -u <(grep -v '^nodes-tb-' tools/tb_cursed.golden) <(printf '%s\n' "$actual"); then
    green "tb-cursed passed (cursed-win and blessed-loss)"
  else
    red "tb-cursed: drifted from tools/tb_cursed.golden"
    return 1
  fi

  # Node-limited TB legs: pin the search total on positions where tablebase
  # probes fire mid-search. The time-check counter resets after every probe
  # (upstream search.cpp:917), and only this leg can see that reset: the counter
  # phase shifts the node-limited stop by hundreds of nodes when the reset is
  # wrong, while every fixed-depth gate stays green.
  #
  # These two values are a SELF-golden and cannot be otherwise: the oracle
  # early-returns at depth 1 once the root is in a tablebase and reports `nodes 0`
  # for both legs, which is the same asymmetry tb.golden documents. So they move
  # with anything that moves a node count -- a net sync moves them -- and nothing
  # re-derives them automatically. Re-derive from mcfish when the anchor moves for
  # an intended reason, and only after the WDL/DTZ half above is still green: that
  # half is oracle-pinned and is what says the prober is right.
  local nactual
  nactual=$(tb_cursed_nodes_half)

  if diff -u <(grep '^nodes-tb-' tools/tb_cursed.golden) <(printf '%s\n' "$nactual"); then
    green "tb-cursed nodes legs passed (TB-probe time-check reset pinned)"
  else
    red "tb-cursed: node-limited TB totals drifted from tools/tb_cursed.golden"
    red "  A net or search change moves these; ./build.sh tb-cursed-update re-derives"
    red "  them, but ONLY once the WDL/DTZ half above is green -- that half is the"
    red "  oracle-pinned one and is what says the prober is still right."
    return 1
  fi
}

# Re-derive tools/tb_cursed.golden. A golden with no regeneration step rots, and this
# one rots invisibly: it is outside `parity` and needs tables `tb-fetch` does not get
# by default, so nothing re-derives it when a net change moves every node count.
#
# REFUSE unless the probe half already matches. That half is derived from the oracle
# and is what says the prober is right; the node legs below it are a self-golden that
# only pins the time-check-counter phase. Regenerating both together would let a real
# prober regression be written straight into the expectation -- the exact laundering
# docs/10-tooling-ci.md describes. So the probe half is a precondition here, not an
# output.
do_tb_cursed_update() {
  need_binary
  tb_cursed_need_tables tb-cursed-update || return $?

  local probe
  probe=$(tb_cursed_probe_half)
  if ! diff -u <(grep -v '^nodes-tb-' tools/tb_cursed.golden) <(printf '%s\n' "$probe") \
       > /dev/null; then
    red "tb-cursed-update: the WDL/DTZ half does not match the golden."
    red "  That half is ORACLE-derived: a mismatch is a prober bug, not a stale value."
    red "  Fix the code. Re-deriving here would pin the defect."
    return 1
  fi
  info "tb-cursed-update: WDL/DTZ half matches; re-deriving the node-limited legs"

  local nodes
  nodes=$(tb_cursed_nodes_half)
  { printf '%s\n' "$probe"; printf '%s\n' "$nodes"; } > tools/tb_cursed.golden
  green "tb_cursed golden re-derived:"
  printf '%s\n' "$nodes" | sed 's/^/  /'
}

do_upstream_map() {
  # LOCAL ONLY: reads the pinned upstream tree from this repo's git objects or the
  # sibling golden checkout, which a CI checkout of origin does not carry. The audit
  # holds tools/upstream_map.tsv to the citation-derived reality (ROT and DRIFT both
  # fail); the ratchet holds the uncovered upstream surface at
  # tools/upstream_map.baseline -- lower it as citations land, never raise it.
  local sha
  sha=$(cat tools/upstream/UPSTREAM_BASE)
  if ! git cat-file -e "${sha}^{commit}" 2>/dev/null \
     && ! git -C ../Stockfish cat-file -e "${sha}^{commit}" 2>/dev/null; then
    red "upstream-map: pin $sha reachable in neither this repo nor ../Stockfish -- SKIPPED"
    return 127
  fi
  info "upstream-map: declared-vs-derived audit + uncovered ratchet"
  python3 tools/upstream_map.py --audit --baseline
}

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
  do_shellcheck || { [[ $? -eq 127 ]] && skipped+=(shellcheck) || return 1; }
  do_cite_check
  do_fixture_coverage
  do_lane_coverage
  do_golden_coverage
  do_tools_smoke
  do_test

  # Signature exits 127 when no net is reachable, for the same reason fmt does when
  # clang-format is absent: the gate could not run. Name it as skipped rather than
  # let a fallback-tree node count read as an anchor comparison.
  do_signature || { [[ $? -eq 127 ]] && skipped+=(signature) || return 1; }
  do_net_roundtrip || { [[ $? -eq 127 ]] && skipped+=(net-roundtrip) || return 1; }
  do_speedtest_check
  do_simd_scalar || { [[ $? -eq 127 ]] && skipped+=(simd-scalar) || return 1; }

  do_perft
  do_golden
  do_tb
  do_malformed

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
  pgo                profile-guided build (instrument, bench, rebuild) -> build/mcfish
  debug              compile with asan+ubsan             -> build/mcfish-debug
  test               build and run the unit/property suite (asan+ubsan)
  fuzz-search [s]    in-process libFuzzer over search_go, s seconds (default 30)
  fuzz-tb [s]        in-process libFuzzer over the Syzygy parse, s seconds (default 30)
  tsan               re-run the suite under ThreadSanitizer (the thread-pool gate)
  tsan-search [d] [t] run a real search under ThreadSanitizer (the search-race baseline)
  bench [depth]      run the benchmark (default depth 13)
  simd-scalar        rebuild with the scalar SIMD path and re-assert the anchor
  async-check        stop/ponderhit invariants on a RUNNING search
  tools-smoke        assert every tool no other lane invokes still runs
  lane-coverage      every step runs in a lane, or is excused with a reason
  golden-coverage    every golden is read by a gate, or owned with a reason
  counter-validate   LOCAL: check a perf counter against two known bottlenecks
  fixture-coverage   hold the property list to the fixtures, both directions
  negative-control   mutate the engine and require each named gate to go RED
  arch-determinism   build every executable ISA tier and require one node count
  net                report where the NNUE net must be and how to obtain it
  net-fetch          download + sha256-verify the net -> resources/ (what CI runs)
  signature          assert the bench node count vs tools/signature.golden
  net-roundtrip      export the net and require it back byte-identical
                     (the only gate on the WRITING side of the .nnue format)
  speedtest-check    drive `speedtest` and assert its report's shape
  perf-budget-tb     LOCAL: the same budget on a PROBING workload (needs tb-fetch 5)
  perf-budget        LOCAL: assert retired instructions vs tools/instr_budget.golden
                     (catches an nps regression the node signature is blind to)
  perft              assert perft counts vs tools/perft.table
  golden             diff the UCI case outputs vs tools/*.golden
  tb-fetch [5]       download + magic-verify the Syzygy sets -> resources/syzygy[5]
  tb-cursed          LOCAL: cursed-win/blessed-loss DTZ>100 branches (needs tb-fetch 5)
  tb                 assert Syzygy discovery and the root probe vs tools/tb.golden
  malformed          assert a crafted tablebase file is still refused (asan+ubsan)
  zone-check         assert engine/+platform/ link without shell/
  engine-standalone  ratchet the engine->platform edge (link engine/ alone)
  fmt / fmt-fix      check / apply clang-format
  docs-lint          check docs for dead links and stale paths
  shellcheck         lint every .sh in the tree (pinned shellcheck, severity style)
  cite-check         every commit SHA the docs cite still resolves (needs a full clone)
  sync-status        report drift between the pinned SHAs and the tracked repos
  upstream-map       LOCAL: audit the declared upstream map, ratchet uncovered surface
  upstream-nodes     node-for-node differential on RANDOM positions vs the oracle
  upstream-transcript LOCAL: whole UCI transcript diffed against the oracle
  golden-audit       LOCAL: adjudicate every UCI golden against the oracle
  fingerprint        LOCAL: assert mcfish CALLS what upstream calls, as often
  material-eval [arch] LOCAL: build the spine-isolation pair (network removed, both sides)
  upstream-parity    THE finish line: bench vs a pristine upstream build (red until done)
  parity             the aggregate: every in-repo gate above
  clean              remove build/

  signature-update   re-derive the signature golden  (intended changes only)
  perf-budget-update re-derive the instruction budget for this arch (known-good build)
  perf-budget-tb-update re-derive the PROBING budget row (known-good build)
  golden-update      re-derive the UCI goldens FROM MCFISH -- prefer golden-audit --write,
                     which derives them from the oracle instead (intended changes only)
  tb-update          re-derive tools/tb.golden FROM THE ORACLE
  tb-cursed-update   re-derive the tb-cursed node legs (refuses on a bad WDL/DTZ half)

Read docs/10-tooling-ci.md before regenerating any golden: doing so on a red
gate pins the defect instead of fixing it.
EOF
}

case "${1:-build}" in
  build)            do_build ;;
  pgo)              do_pgo ;;
  debug)            do_debug ;;
  test)             do_test ;;
  fuzz-search)      shift; do_fuzz_search "$@" ;;
  fuzz-tb)          shift; do_fuzz_tb "$@" ;;
  tsan)             do_tsan ;;
  tsan-search)      shift; do_tsan_search "$@" ;;
  bench)            do_bench "${2:-}" ;;
  net)              do_net ;;
  net-fetch)        do_net_fetch ;;
  signature)        do_signature ;;
  net-roundtrip)    do_net_roundtrip ;;
  perf-budget)      do_perf_budget ;;
  perf-budget-update) do_perf_budget_update ;;
  perf-budget-tb)   do_perf_budget_tb ;;
  perf-budget-tb-update) do_perf_budget_tb_update ;;
  simd-scalar)      do_simd_scalar ;;
  async-check)      do_async_check ;;
  speedtest-check)  do_speedtest_check ;;
  tools-smoke)      do_tools_smoke ;;
  lane-coverage)    do_lane_coverage ;;
  golden-coverage)  do_golden_coverage ;;
  counter-validate) do_counter_validate ;;
  fixture-coverage) do_fixture_coverage ;;
  negative-control) shift; do_negative_control "$@" ;;
  arch-determinism) do_arch_determinism ;;
  signature-update) do_signature_update ;;
  perft)            do_perft ;;
  golden)           do_golden ;;
  tb)               do_tb ;;
  upstream-map)     do_upstream_map ;;
  tb-fetch)         shift; do_tb_fetch "$@" ;;
  malformed)        do_malformed ;;
  tb-cursed)        do_tb_cursed ;;
  tb-update)        do_tb_update ;;
  tb-cursed-update) do_tb_cursed_update ;;
  golden-update)    do_golden_update ;;
  zone-check)       do_zone_check ;;
  engine-standalone) do_engine_standalone ;;
  docs-lint)        do_docs_lint ;;
  upstream-nodes)   shift; do_upstream_nodes "$@" ;;
  upstream-transcript) do_upstream_transcript ;;
  golden-audit)     shift; tools/upstream_golden_audit.sh "$@" ;;
  fingerprint)      shift; tools/upstream_fingerprint.sh "$@" ;;
  material-eval)    shift; tools/material_eval.sh "$@" ;;
  sync-status)      do_sync_status ;;
  upstream-parity)  shift; do_upstream_parity "$@" ;;
  shellcheck)       do_shellcheck ;;
  cite-check)       do_cite_check ;;
  fmt)              do_fmt ;;
  fmt-fix)          do_fmt_fix ;;
  parity)           do_parity ;;
  clean)            do_clean ;;
  help|-h|--help)   do_help ;;
  *)                red "unknown step: $1"; echo; do_help; exit 2 ;;
esac
