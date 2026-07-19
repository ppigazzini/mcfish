#!/usr/bin/env bash
# The finish-line gate: mcfish's bench must equal a PRISTINE upstream build's bench
# at the pinned SHA.
#
# This is RED until the port completes, and that is correct — it is the definition
# of "done", not a regression check. `./build.sh signature` is the day-to-day
# anchor; this is the one that decides whether mcfish is a Stockfish clone.
#
# Usage:  upstream_parity.sh [mcfish-binary] [sha]
set -euo pipefail

cd "$(dirname "$0")/../.."

BIN=${1:-build/mcfish}
sha=${2:-$(cat tools/upstream/UPSTREAM_BASE)}

red()   { printf '\033[31m%s\033[0m\n' "$*"; }
green() { printf '\033[32m%s\033[0m\n' "$*"; }
info()  { printf '\033[36m==>\033[0m %s\n' "$*"; }

[[ -x $BIN ]] || { red "no mcfish binary at $BIN — run ./build.sh first"; exit 2; }

info "building the pristine upstream oracle at $sha"
oracle=$(bash tools/upstream/upstream_oracle.sh "$sha")
info "oracle: $oracle"

# Read the node total off stderr: upstream prints the bench banners there, and so
# does mcfish (deliberately — that stream choice is faithful, not a bug).
# Run each engine from a directory that HOLDS ITS NET. The oracle keeps one beside
# its binary; mcfish keeps one in resources/ (build.sh RESOURCES_DIR). Running
# mcfish from the repo root instead benched the classical fallback and reported a
# false divergence -- and once mcfish started refusing to run without a net, the
# same mistake became an empty result and a silent exit 1.
bench_nodes() {
  local bin=$1 dir=$2
  ( cd "$dir" && "$bin" bench ) 2>&1 >/dev/null | grep 'Nodes searched' | awk '{print $NF}' | tail -1
}

info "running upstream bench"
oracle_abs=$(readlink -f "$oracle")
want=$(bench_nodes "$oracle_abs" "$(dirname "$oracle_abs")")
info "running mcfish bench"
got=$(bench_nodes "$PWD/$BIN" "$PWD/resources")

printf '\n  upstream @ %s : %s\n' "${sha:0:12}" "${want:-<none>}"
printf '  mcfish            : %s\n\n' "${got:-<none>}"

if [[ -n $want && $got == "$want" ]]; then
  green "BIT-EXACT — mcfish reproduces upstream's bench signature"
  exit 0
fi

red "NOT bit-exact yet."
red "This is the expected state until the port completes — see docs/PORTING.md."
red "Use ./build.sh port-status for the remaining work list."
exit 1
