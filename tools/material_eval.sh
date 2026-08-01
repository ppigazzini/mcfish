#!/usr/bin/env bash
# Build the spine-isolation pair: mcfish and the ORACLE, both with the network
# replaced by the same material sum, and both at the same tier.
#
# tools/material_eval.patch is the formula for upstream's side; this is the procedure
# around it, because the procedure is where the trap is. Applying the patch is easy.
# Putting the oracle back afterwards is what gets skipped, and a stubbed oracle left
# on disk poisons every later measurement silently.
#
# REVERTING THE SOURCE IS NOT ENOUGH. That was this script's first form and it was
# wrong. The last thing built is the STUBBED oracle, and it stays on disk. Every
# in-repo consumer routes through upstream_oracle.sh, which would rebuild -- except
# the `.built-sha` STAMP still matches the pin, so it sees a current build and does
# nothing. Anyone running the oracle binary directly then measures a stub. That is
# not hypothetical in either port: it happened here during the c5aef2bf1 spine run
# (the restore ran from the wrong directory, exited 127, and the oracle benched
# 4186134 instead of the anchor until it was caught), and in the sibling port it
# reached a published NNUE ratio before a node-count guard caught it.
#
# So the EXIT trap reverts the source AND deletes the binary AND the stamp. A missing
# oracle fails loudly on the next use; a stubbed one does not. Ported from zfish
# 4ab4f4ac, whose fix this is.
#
# Usage:  tools/material_eval.sh [arch]        # default sse41, the tier the oracle builds
#         MCFISH_OUT=... SF_OUT=...            # where to leave the two binaries
set -uo pipefail

cd "$(dirname "$0")/.."
ROOT=$PWD
ARCH=${1:-sse41}
case "$ARCH" in
  sse41)    SF_ARCH=x86-64-sse41-popcnt ;;
  avx2)     SF_ARCH=x86-64-avx2 ;;
  vnni512)  SF_ARCH=x86-64-vnni512 ;;
  *) printf 'material-eval: unknown arch %s (want sse41, avx2, vnni512)\n' "$ARCH" >&2; exit 2 ;;
esac
ORACLE_DIR=${ORACLE_DIR:-$ROOT/../.mcfish-upstream-oracle}
MCFISH_OUT=${MCFISH_OUT:-$ROOT/build/mcfish-material}
SF_OUT=${SF_OUT:-$ROOT/build/stockfish-material}

red() { printf '\033[31m%s\033[0m\n' "$*" >&2; }
green() { printf '\033[32m%s\033[0m\n' "$*"; }
info() { printf '\033[36m==>\033[0m %s\n' "$*"; }

[[ -d $ORACLE_DIR/.git || -f $ORACLE_DIR/.git ]] || {
  red "material-eval: no oracle worktree at $ORACLE_DIR -- run tools/upstream/upstream_oracle.sh first"
  exit 2
}

# Leave nothing usable-but-wrong behind, however this exits. See the header.
finish() {
  git -C "$ORACLE_DIR" checkout -- src/evaluate.cpp 2>/dev/null
  rm -f "$ORACLE_DIR/src/stockfish" "$ORACLE_DIR/src/.built-sha"
  info "material-eval: oracle source reverted, and its BINARY AND STAMP removed --"
  info "material-eval: the next tools/upstream/upstream_oracle.sh rebuilds it pristine."
}
trap finish EXIT

info "material-eval: patching the oracle at $SF_ARCH"
git -C "$ORACLE_DIR" checkout -- src/evaluate.cpp 2>/dev/null
git -C "$ORACLE_DIR" apply "$ROOT/tools/material_eval.patch" || {
  red "material-eval: tools/material_eval.patch does not apply -- upstream moved, update it"
  exit 1
}
make -C "$ORACLE_DIR/src" -j"$(nproc)" build ARCH="$SF_ARCH" >/dev/null 2>&1 || {
  red "material-eval: oracle build failed"; exit 1; }
mkdir -p "$(dirname "$SF_OUT")"
cp "$ORACLE_DIR/src/stockfish" "$SF_OUT"

info "material-eval: building mcfish with MCFISH_EVAL_MATERIAL=1 at $ARCH"
MCFISH_EVAL_MATERIAL=1 MCFISH_ARCH="$ARCH" "$ROOT/build.sh" build >/dev/null 2>&1 || {
  red "material-eval: mcfish build failed"; exit 1; }
cp "$ROOT/build/mcfish" "$MCFISH_OUT"

# THE assertion. The two formulas are written out independently in the two engines
# precisely so they cannot drift, and a drift shows up here and nowhere else: the
# trees stop matching and every ratio taken from the pair is void.
fail=0
for d in 8 12; do
  m=$(cd "$ROOT/resources" && "$MCFISH_OUT" bench 16 1 "$d" 2>&1 | grep -oP 'Nodes searched\s*:\s*\K[0-9]+')
  s=$(cd "$ROOT/resources" && "$SF_OUT" bench 16 1 "$d" 2>&1 | grep -oP 'Nodes searched\s*:\s*\K[0-9]+')
  if [[ -n $m && $m == "$s" ]]; then
    printf '  bench 16 1 %-3s %-10s MATCH\n' "$d" "$m"
  else
    red "  bench 16 1 $d  mcfish=${m:-none} oracle=${s:-none}  *** TREES DIVERGE ***"
    fail=1
  fi
done
[[ $fail -eq 0 ]] || {
  red "material-eval: the two material formulas disagree -- fix tools/material_eval.patch"
  red "  against src/engine/search/search_common.c's material_only_eval before measuring."
  exit 1
}

green "material-eval: pair ready at $ARCH"
echo "  mcfish   $MCFISH_OUT"
echo "  oracle   $SF_OUT"
echo "Both are MEASUREMENT binaries: they play badly by construction and move the anchor."
