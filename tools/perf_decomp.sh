#!/usr/bin/env bash
# Decompose the cost of two binaries per component, deterministically.
#
# tools/perf_counters.sh says WHETHER the machine executed the program
# differently. This says WHERE. It runs both binaries under callgrind with the
# cache and branch simulators, sums SELF cost per symbol, groups the symbols into
# components (tools/perf_components.tsv) and prints instructions, D1 read misses
# and branch mispredicts per component with the winner named.
#
# EVERY NUMBER HERE IS DETERMINISTIC. callgrind simulates, so two runs of one
# binary give identical counts and a component difference of any size is real
# rather than thermal noise. That is what pays for the simulator's
# order-of-magnitude slowdown, and it is why the depth stays small.
#
# AND EVERY NUMBER HERE IS A MODEL -- see the header of tools/perf_decomp.py. It
# ranks locality; it does not predict time. Gate on the clock (tools/nps_ab.sh);
# use this to find out which component moved.
#
# BOTH SIDES MUST SEARCH THE SAME TREE, and that is asserted before anything is
# profiled: a different node count is a different workload, and every row would be
# noise wearing a number. The run is VOID rather than reported when they differ.
#
# CALLGRIND IMPLEMENTS NO AVX-512, so profile a binary built at avx2 or below;
# tools/perf_counters.sh is the axis for the higher tiers.
#
# Usage:  perf_decomp.sh <base-binary> <head-binary> [bench-args...]
#         perf_decomp.sh /tmp/mcfish-before build/mcfish 16 1 8
#
# Exit codes:  0 reported   1 VOID (node counts differ) or a one-sided component
#              2 a profile could not be taken or could not be grouped
set -u
set -o pipefail

ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd) || exit 2
cd "$ROOT" || exit 2

red=$'\033[31m'; green=$'\033[32m'; cyan=$'\033[36m'; off=$'\033[0m'

BASE=${1:?usage: perf_decomp.sh <base-binary> <head-binary> [bench-args...]}
HEAD=${2:?usage: perf_decomp.sh <base-binary> <head-binary> [bench-args...]}
shift 2
BENCH_ARGS=("$@")
[[ ${#BENCH_ARGS[@]} -eq 0 ]] && BENCH_ARGS=(16 1 8)

COMPONENTS=${COMPONENTS:-tools/perf_components.tsv}

# ABSOLUTE, both of them. Every run below happens from resources/ (that is where the
# net is), so a relative path handed in from the root resolves to nothing there -- and
# the failure looks like "a side produced no node count" rather than "no such file",
# which is how it was found.
for var in BASE HEAD; do
  f=${!var}
  [[ -x $f ]] || { printf '%sperf-decomp: %s is not executable%s\n' "$red" "$f" "$off"; exit 2; }
  printf -v "$var" '%s' "$(cd "$(dirname "$f")" && pwd)/$(basename "$f")"
done
command -v valgrind >/dev/null 2>&1 || {
  printf '%sperf-decomp: valgrind not found -- this did NOT run%s\n' "$red" "$off"; exit 2; }
[[ -f $COMPONENTS ]] || {
  printf '%sperf-decomp: no components file at %s%s\n' "$red" "$COMPONENTS" "$off"; exit 2; }

WORK=$(mktemp -d) || exit 2
# shellcheck disable=SC2329  # invoked by the trap below, which shellcheck cannot follow.
# (0.9.0 reports this as SC2317 and 0.11.0 as SC2329, which is why the version is pinned.)
cleanup() { rm -rf "$WORK"; }
trap cleanup EXIT

printf '%s==>%s perf-decomp: bench %s, base=%s head=%s\n' \
  "$cyan" "$off" "${BENCH_ARGS[*]}" "$(basename "$BASE")" "$(basename "$HEAD")"

# THE TREE FIRST. Cheap, and it decides whether anything below is comparable.
nodes_of() {
  ( cd "$ROOT/resources" && "$1" bench "${BENCH_ARGS[@]}" 2>&1 ) \
    | sed -n 's/^Nodes searched *: *\([0-9]*\)/\1/p' | tail -1
}
nb=$(nodes_of "$BASE"); nh=$(nodes_of "$HEAD")
if [[ -z $nb || -z $nh ]]; then
  printf '%sperf-decomp: a side produced no node count -- it did not run a bench%s\n' "$red" "$off"
  exit 2
fi
if [[ $nb != "$nh" ]]; then
  printf '  %sVOID%s -- base searched %s nodes, head searched %s.\n' "$red" "$off" "$nb" "$nh"
  printf '  A different tree is not a cheaper one. Fix the behaviour change first.\n'
  exit 1
fi
printf '  nodes %s on both sides\n' "$nb"

for side in base head; do
  bin=$BASE; [[ $side == head ]] && bin=$HEAD
  printf '  profiling %s under callgrind (this is slow) ...\n' "$side"
  ( cd "$ROOT/resources" && OUT=$WORK/cg.$side "$ROOT/tools/perf_callgrind.sh" \
      "$bin" "${BENCH_ARGS[@]}" ) > "$WORK/$side.log" 2>&1 || {
    printf '%sperf-decomp: %s profile failed%s\n' "$red" "$side" "$off"; exit 2; }
  # A profile of a run that never searched looks plausible and is worthless.
  grep -q 'Nodes searched' "$WORK/$side.log" || {
    printf '%sperf-decomp: the %s profile carries no "Nodes searched" -- it did not bench%s\n' \
      "$red" "$side" "$off"; exit 2; }
done

echo
python3 "$ROOT/tools/perf_decomp.py" "$WORK/cg.base" "$WORK/cg.head" "$COMPONENTS" \
  --base-label base --head-label head
rc=$?

echo
# Three outcomes, and they are not interchangeable. Collapsing them all to one
# tells a reader "the tool was absent" when what happened is "the two profiles are
# not comparable", and hides a table that WAS printed.
case $rc in
  0) printf '%sperf-decomp: reported -- every figure is deterministic and every figure is a model%s\n' \
       "$green" "$off" ;;
  1) printf '%sperf-decomp: FINDINGS -- a component matched on one side only; that row is marked X%s\n' \
       "$cyan" "$off"
     printf '%sperf-decomp: above and excluded from the verdict. The rest stands.%s\n' "$cyan" "$off" ;;
  *) printf '%sperf-decomp: VOID -- the profiles could not be grouped; see the reason above%s\n' \
       "$red" "$off" ;;
esac
exit $rc
