#!/usr/bin/env bash
# Assert mcfish CALLS the same things as upstream, as many times.
#
# This is the algorithm gate. Every other differential here compares VALUES -- the
# bench anchor, the goldens, the node differential -- and each of them passes over a
# state divergence that happens not to move a node count on the positions it drives.
# Two real defects were found by this comparison and by nothing else:
#
#   * `ucinewgame` discarded the position, where upstream leaves it alone. Every
#     value gate was green; no case in the tree issued `ucinewgame` at all.
#   * a mate or stalemate root skipped the per-worker reset upstream runs
#     unconditionally, leaving worker 0 on the previous `go`'s limits and counters.
#
# Both showed up as ONE call of difference in set_check_info.
#
# It is inlining-immune by construction: a call count does not care how the callee
# was reached, only that it was. That is why it can compare a C23 tree against a C++
# one at all, where any cost comparison has to argue about attribution first.
#
# DETERMINISTIC, so it is worth running on a loaded box -- callgrind simulates,
# it does not sample. It is also SLOW (callgrind is ~50x), which is why this is a
# separate step and not part of `parity`.
#
# BOTH SIDES MUST BE x86-64-sse41-popcnt. callgrind SIGILLs above the tier it
# understands, and comparing tiers measures the ISA rather than the code. The oracle
# builds at sse41 by default; this builds mcfish to match.
#
# Usage:  ./build.sh fingerprint [bench-args...]     (default: 16 1 8)
#         ORACLE_SHA=<sha> ./build.sh fingerprint
set -uo pipefail

cd "$(dirname "$0")/.." || exit 2
ROOT=$PWD
BENCH=${*:-16 1 8}
# NOT `GROUPS`: that is a bash BUILT-IN array holding the caller's group ids, so the
# assignment is silently ignored and `$GROUPS` expands to the primary gid. It read
# "no groups in 1000" -- the only symptom, and it names no variable.
GROUP_FILE=$ROOT/tools/fingerprint_groups.tsv
WORK=${TMPDIR:-/tmp}/mcfish-fingerprint.$$
mkdir -p "$WORK"
trap 'rm -rf "$WORK"' EXIT

red() { printf '\033[31m%s\033[0m\n' "$*" >&2; }
green() { printf '\033[32m%s\033[0m\n' "$*"; }
info() { printf '\033[36m==>\033[0m %s\n' "$*"; }

command -v valgrind >/dev/null || { red "fingerprint: valgrind not installed"; exit 127; }

ORACLE=$("$ROOT/tools/upstream/upstream_oracle.sh" ${ORACLE_SHA:+"$ORACLE_SHA"}) || {
  red "fingerprint: oracle build failed"; exit 2; }
ORACLE=$(cd "$(dirname "$ORACLE")" && printf '%s/%s' "$PWD" "$(basename "$ORACLE")")

info "fingerprint: building mcfish at sse41 to match the oracle's tier"
MCFISH_ARCH=sse41 "$ROOT/build.sh" build >/dev/null 2>&1 || {
  red "fingerprint: mcfish build failed"; exit 2; }
cp "$ROOT/build/mcfish" "$WORK/mcfish"

# Assert the two engines search the SAME TREE before comparing anything about how
# they searched it. A different tree is a different workload and every row below
# would be noise wearing a number.
# shellcheck disable=SC2086  # BENCH is a bench ARGUMENT LIST; the split is the point
mc_nodes=$(cd "$ROOT/resources" && "$WORK/mcfish" bench $BENCH 2>&1 | grep -oP 'Nodes searched\s*:\s*\K[0-9]+')
# shellcheck disable=SC2086  # BENCH is a bench ARGUMENT LIST; the split is the point
or_nodes=$(cd "$ROOT/resources" && "$ORACLE" bench $BENCH 2>&1 | grep -oP 'Nodes searched\s*:\s*\K[0-9]+')
if [[ -z $mc_nodes || $mc_nodes != "$or_nodes" ]]; then
  red "fingerprint: node counts differ (mcfish ${mc_nodes:-none}, upstream ${or_nodes:-none})"
  red "  Fix that first -- it is a bigger finding than anything this step reports."
  exit 1
fi
info "fingerprint: both engines search $mc_nodes nodes on bench $BENCH"

for side in mcfish oracle; do
  bin=$WORK/mcfish; [[ $side == oracle ]] && bin=$ORACLE
  info "fingerprint: profiling $side (callgrind, this is slow)"
  # shellcheck disable=SC2086  # BENCH is a bench ARGUMENT LIST; the split is the point
  ( cd "$ROOT/resources" && OUT=$WORK/$side.out "$ROOT/tools/perf_callgrind.sh" "$bin" $BENCH ) \
    >"$WORK/$side.log" 2>&1 || { red "fingerprint: $side profile failed"; exit 2; }
  # A profile of a run that never searched looks plausible and is worthless.
  grep -q "Nodes searched" "$WORK/$side.log" || {
    red "fingerprint: $side profile carries no 'Nodes searched' -- it did not run a bench"; exit 2; }
done

args=()
while IFS=$'\t' read -r name regex; do
  [[ -z ${name// } || ${name:0:1} == "#" ]] && continue
  args+=(--group "$name=$regex")
done < "$GROUP_FILE"
[[ ${#args[@]} -eq 0 ]] && { red "fingerprint: no groups in $GROUP_FILE"; exit 2; }

out=$(python3 "$ROOT/tools/perf_fingerprint.py" compare "$WORK/mcfish.out" "$WORK/oracle.out" \
      --calls "${args[@]}" 2>&1)
printf '%s\n' "$out"

# EXACT is the tool's own word for a group whose two counts are equal. Anything else
# -- a differing count, or a regex that matched nothing on a side -- fails.
bad=$(printf '%s\n' "$out" | grep -cE 'DIFFERS|no symbol matched' || true)
if [[ $bad -ne 0 ]]; then
  red "fingerprint: $bad group(s) diverge from upstream"
  red "  A call-count divergence is an ALGORITHM difference and outranks any cost"
  red "  finding. Check the regex first -- an inlined-away symbol reads the same as a"
  red "  real divergence -- then attribute the count to its callers before concluding."
  exit 1
fi
green "fingerprint: every group calls exactly as often as upstream"
