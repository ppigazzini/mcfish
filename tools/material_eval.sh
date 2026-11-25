#!/usr/bin/env bash
# Build the spine-isolation pair: mcfish and the ORACLE, both with the network
# replaced by the same material sum, and both at the same tier.
#
# tools/material_eval.patch is the formula for upstream's side. `upstream_oracle.sh
# --patch` builds it as a variant of its own, keyed by the patch, so the pristine oracle
# is never edited and there is nothing to put back.
#
# Usage:  tools/material_eval.sh [arch]        # default sse41, the tier the oracle builds
#         MCFISH_OUT=... SF_OUT=...            # where to leave the two binaries
set -uo pipefail

cd "$(dirname "$0")/.." || exit 2
ROOT=$PWD
ARCH=${1:-sse41}
case "$ARCH" in
  sse41)    SF_ARCH=x86-64-sse41-popcnt ;;
  avx2)     SF_ARCH=x86-64-avx2 ;;
  vnni512)  SF_ARCH=x86-64-vnni512 ;;
  *) printf 'material-eval: unknown arch %s (want sse41, avx2, vnni512)\n' "$ARCH" >&2; exit 2 ;;
esac
MCFISH_OUT=${MCFISH_OUT:-$ROOT/build/mcfish-material}
SF_OUT=${SF_OUT:-$ROOT/build/stockfish-material}

red() { printf '\033[31m%s\033[0m\n' "$*" >&2; }
green() { printf '\033[32m%s\033[0m\n' "$*"; }
info() { printf '\033[36m==>\033[0m %s\n' "$*"; }

info "material-eval: building the patched oracle at $SF_ARCH"
patched=$(ORACLE_ARCH=$SF_ARCH tools/upstream/upstream_oracle.sh --patch tools/material_eval.patch) || {
  red "material-eval: oracle build failed -- if the patch does not apply, upstream moved: update it"
  exit 1
}
mkdir -p "$(dirname "$SF_OUT")"
cp "$patched" "$SF_OUT"

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
