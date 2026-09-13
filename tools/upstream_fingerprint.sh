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
# -- a differing count, or a regex that matched nothing on a side -- is a divergence.
#
# A group named in tools/fingerprint_known.txt is ACCEPTED, each with an argued
# reason in that file. Anything else fails. Two rows are listed today and both are
# older than any gate that could have caught them, so without this the step can
# never pass and the lane reports nothing about the rows that CAN move.
KNOWN_FILE=$ROOT/tools/fingerprint_known.txt
known=()
if [[ -f $KNOWN_FILE ]]; then
  while read -r name _; do
    [[ -z ${name// } || ${name:0:1} == "#" ]] && continue
    known+=("$name")
  done < "$KNOWN_FILE"
fi
is_known() { local g; for g in ${known+"${known[@]}"}; do [[ $g == "$1" ]] && return 0; done; return 1; }

unexpected=0 accepted=0
while read -r group rest; do
  [[ -z ${group// } ]] && continue
  case $rest in
    *DIFFERS*|*"no symbol matched"*) ;;
    *) continue ;;
  esac
  if is_known "$group"; then
    accepted=$((accepted + 1))
    printf '  accepted  %s -- argued in tools/fingerprint_known.txt\n' "$group"
  else
    unexpected=$((unexpected + 1))
    printf '  UNKNOWN   %s\n' "$group"
  fi
done < <(printf '%s\n' "$out" | sed -n 's/^\([A-Za-z_][A-Za-z0-9_ ]*[A-Za-z0-9_]\)  */\1\t/p' \
          | awk -F'\t' '{print $1, $2}')

# A listed group that now reads EXACT is hiding nothing. Say so rather than accept it
# silently: an entry that outlives its cause turns the gate into decoration.
for g in ${known+"${known[@]}"}; do
  if printf '%s\n' "$out" | grep -qE "^$g +.*EXACT"; then
    printf '  RETIRABLE %s now reads EXACT -- delete its entry\n' "$g"
  fi
done

if [[ $unexpected -ne 0 ]]; then
  red "fingerprint: $unexpected group(s) diverge from upstream and are not argued"
  red "  A call-count divergence is an ALGORITHM difference and outranks any cost"
  red "  finding. Check the regex first -- an inlined-away symbol reads the same as a"
  red "  real divergence -- then attribute the count to its callers before concluding."
  red "  If it is genuinely accepted, add it to tools/fingerprint_known.txt WITH a"
  red "  reason and what would retire it."
  exit 1
fi
green "fingerprint: every group calls as often as upstream ($accepted argued exception(s))"
