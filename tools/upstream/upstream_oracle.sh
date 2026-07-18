#!/usr/bin/env bash
# Build a PRISTINE upstream Stockfish at a given SHA into a throwaway worktree and
# print the binary path.
#
# Pristine is the point. The oracle must be upstream's own code, built by upstream's
# own Makefile, with no ccfish edit anywhere near it — otherwise the differential
# gate compares ccfish against something ccfish influenced, and a shared bug cancels
# out on both sides and passes.
#
# Usage:  upstream_oracle.sh [sha]        # defaults to tools/upstream/UPSTREAM_BASE
#         upstream_oracle.sh <sha> --verify
set -euo pipefail

cd "$(dirname "$0")/../.."

UPSTREAM_REPO=${UPSTREAM_REPO:-../Stockfish}
ORACLE_DIR=${ORACLE_DIR:-../.ccfish-upstream-oracle}

sha=${1:-$(cat tools/upstream/UPSTREAM_BASE)}
verify=${2:-}

red()   { printf '\033[31m%s\033[0m\n' "$*" >&2; }
info()  { printf '\033[36m==>\033[0m %s\n' "$*" >&2; }

[[ -d $UPSTREAM_REPO/.git ]] || {
  red "no upstream Stockfish checkout at $UPSTREAM_REPO"
  red "clone it, or set UPSTREAM_REPO to where it lives"
  exit 2
}

# Use a detached worktree rather than checking out in the main clone: the sync
# workflow reads the upstream tree while ccfish is mid-edit, and a checkout in the
# shared clone would yank the tree out from under whoever is reading it.
if [[ ! -d $ORACLE_DIR ]]; then
  info "creating oracle worktree at $ORACLE_DIR"
  git -C "$UPSTREAM_REPO" worktree add --detach "$(cd "$(dirname "$ORACLE_DIR")" \
    && pwd)/$(basename "$ORACLE_DIR")" "$sha" >&2
else
  info "reusing oracle worktree at $ORACLE_DIR"
  git -C "$ORACLE_DIR" checkout --detach "$sha" >&2
fi

bin="$ORACLE_DIR/src/stockfish"

if [[ ! -x $bin || -n $verify ]]; then
  info "building pristine upstream at $sha (this takes a few minutes)"
  # profile-build would be faster at runtime but is far slower to produce and its
  # PGO run is nondeterministic in wall-clock terms; `build` is enough for a node
  # count, which is what the oracle is asked for.
  make -C "$ORACLE_DIR/src" -j"$(nproc)" build ARCH=x86-64-sse41-popcnt >&2
fi

[[ -x $bin ]] || { red "oracle build produced no binary at $bin"; exit 1; }

echo "$bin"
