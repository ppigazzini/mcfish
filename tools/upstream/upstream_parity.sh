#!/usr/bin/env bash
# The finish-line gate: ccfish's bench must equal a PRISTINE upstream build's bench
# at the pinned SHA.
#
# This is RED until the port completes, and that is correct — it is the definition
# of "done", not a regression check. `./build.sh signature` is the day-to-day
# anchor; this is the one that decides whether ccfish is a Stockfish clone.
#
# Usage:  upstream_parity.sh [ccfish-binary] [sha]
set -euo pipefail

cd "$(dirname "$0")/../.."

BIN=${1:-build/ccfish}
sha=${2:-$(cat tools/upstream/UPSTREAM_BASE)}

red()   { printf '\033[31m%s\033[0m\n' "$*"; }
green() { printf '\033[32m%s\033[0m\n' "$*"; }
info()  { printf '\033[36m==>\033[0m %s\n' "$*"; }

[[ -x $BIN ]] || { red "no ccfish binary at $BIN — run ./build.sh first"; exit 2; }

info "building the pristine upstream oracle at $sha"
oracle=$(bash tools/upstream/upstream_oracle.sh "$sha")
info "oracle: $oracle"

# Read the node total off stderr: upstream prints the bench banners there, and so
# does ccfish (deliberately — that stream choice is faithful, not a bug).
bench_nodes() {
  "$1" bench 2>&1 >/dev/null | grep 'Nodes searched' | awk '{print $NF}' | tail -1
}

info "running upstream bench"
want=$(bench_nodes "$oracle")
info "running ccfish bench"
got=$(bench_nodes "$BIN")

printf '\n  upstream @ %s : %s\n' "${sha:0:12}" "${want:-<none>}"
printf '  ccfish            : %s\n\n' "${got:-<none>}"

if [[ -n $want && $got == "$want" ]]; then
  green "BIT-EXACT — ccfish reproduces upstream's bench signature"
  exit 0
fi

red "NOT bit-exact yet."
red "This is the expected state until the port completes — see docs/PORTING.md."
red "Use ./build.sh port-status for the remaining work list."
exit 1
