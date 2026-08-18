#!/usr/bin/env bash
# How two binaries SCALE across thread counts, base against head.
#
# THE GAP THIS FILLS. Every other speed axis here runs ONE thread:
# tools/nps_ab.sh, perf-budget, perf-budget-tb, perf_counters.sh and perf-decomp
# are all single-threaded, and the Elo matches default to Threads 1. A player runs
# eight or sixteen. So nothing in this tree has ever measured the thing that decides
# real speed on a real machine -- whether a change contends worse on the shared last
# level, on the transposition table, or on the counters the manager polls.
#
# WHY THE OTHER AXES CANNOT SIMPLY BE POINTED AT MORE THREADS. Every one of them
# refuses a comparison whose node counts differ, and is right to: a count taken over
# a different tree is not comparable. But a multi-threaded search at a FIXED DEPTH is
# not reproducible even against ITSELF. Measured on this tree, three runs of
# `bench 128 8 10`:
#
#     3,773,312    6,144,045    4,460,748        <- a 62.8% spread
#
# against a 0.05% tolerance. Every existing gate reports VOID, correctly, and learns
# nothing. The single-threaded run beside it repeats exactly.
#
# WHAT MAKES IT MEASURABLE: make the node count the INPUT instead of the output.
# `bench <hash> <threads> <N> default nodes` gives every position the same node
# budget, and the same three runs then read
#
#     9,849,966    9,854,910    9,852,636        <- a 0.05% spread
#
# Lazy-SMP threads still overshoot a budget slightly and by a different amount each
# run, which is why the tolerance below is a BAND rather than equality -- the one
# place in this tree where a node-count check is not exact, and it is a property of
# threaded search rather than a concession.
#
# READ r(T)/r(1), NOT THE nps COLUMN. The A/B ratio at T threads carries the
# single-thread speed difference inside it, so a binary that is simply faster looks
# like it scales better. Dividing by the ratio at one thread removes exactly that,
# and what is left is the only column that answers whether the two SCALE differently.
#
# NO CORE PINNING, deliberately, where tools/nps_ab.sh pins to one: the subject here
# is what happens when N threads share a machine. Run it on an idle box.
#
# Usage: nps_threads.sh <binA> <binB> [rounds] [nodes-per-position] [thread-list]
#        nps_threads.sh ./old ./new 3 300000 "1 2 4 8 16"
#        (CWD must hold the net, as with nps_ab.sh)
set -u
set -o pipefail

A="${1:?usage: nps_threads.sh <binA> <binB> [rounds] [nodes] [threads]  (run from the dir holding the net)}"
B="${2:?usage: nps_threads.sh <binA> <binB> [rounds] [nodes] [threads]}"
ROUNDS="${3:-3}"
NODES="${4:-300000}"
THREADS="${5:-1 2 4 8 16}"
HASH="${HASH:-256}"
TOL="${TOL:-2.0}"   # percent; the band a threaded node budget is allowed to overshoot by

for f in "$A" "$B"; do
  [ -x "$f" ] || { echo "error: $f not executable" >&2; exit 2; }
done

# `Total time` is the search-only clock: bench takes its start AFTER the ucinewgame
# clear, so no process startup is inside it.
run() { # <bin> <threads> -> "<ms> <nodes>"
  "$1" bench "$HASH" "$2" "$NODES" default nodes 2>&1 |
    sed -n 's/^Total time (ms)  *: *\([0-9]*\)/T\1/p; s/^Nodes searched  *: *\([0-9]*\)/N\1/p' |
    { t=""; n=""
      while read -r line; do
        case $line in T*) t=${line#T} ;; N*) n=${line#N} ;; esac
      done
      echo "$t $n"; }
}

median() { tr ' ' '\n' | grep -v '^$' | sort -g | awk '{v[NR]=$1} END{ if(NR==0) exit 1;
  print (NR%2) ? v[(NR+1)/2] : (v[NR/2]+v[NR/2+1])/2 }'; }

echo "# bench $HASH <threads> $NODES default nodes | $ROUNDS rounds | hash ${HASH}MB"
echo "# order alternates each round; no core pinning -- contention is the subject"
echo

declare -A RATIO
void=0
for t in $THREADS; do
  ratios=""
  for r in $(seq 1 "$ROUNDS"); do
    if [ $((r % 2)) -eq 1 ]; then
      read -r ta na <<< "$(run "$A" "$t")"; read -r tb nb <<< "$(run "$B" "$t")"
    else
      read -r tb nb <<< "$(run "$B" "$t")"; read -r ta na <<< "$(run "$A" "$t")"
    fi
    if [ -z "$ta" ] || [ -z "$tb" ] || [ -z "$na" ] || [ -z "$nb" ]; then
      echo "  threads $t round $r: a side produced no result -- rig fault" >&2
      exit 2
    fi
    # The node counts must be CLOSE, not equal: see the header.
    off=$(awk -v a="$na" -v b="$nb" 'BEGIN{ printf "%.3f", (a>b? a-b : b-a) * 100.0 / a }')
    if awk -v o="$off" -v l="$TOL" 'BEGIN{ exit !(o > l) }'; then
      echo "  threads $t round $r: VOID -- nodes $na vs $nb differ by $off% (band $TOL%)" >&2
      void=1
    fi
    ratios="$ratios $(awk -v a="$ta" -v b="$tb" 'BEGIN{ printf "%.4f", a/b }')"
    printf '  threads %-3s round %-2s A=%-8s ms  B=%-8s ms  A/B=%s   (%s / %s nodes)\n' \
      "$t" "$r" "$ta" "$tb" "$(awk -v a="$ta" -v b="$tb" 'BEGIN{printf "%.4f", a/b}')" "$na" "$nb"
  done
  RATIO[$t]=$(echo "$ratios" | median)
done

echo
printf '# %-8s %-10s %-12s\n' threads 'A/B time' 'r(T)/r(1)'
base=${RATIO[$(echo "$THREADS" | awk '{print $1}')]}
for t in $THREADS; do
  printf '  %-8s %-10s %-12s\n' "$t" "${RATIO[$t]}" \
    "$(awk -v r="${RATIO[$t]}" -v b="$base" 'BEGIN{ printf "%.4f", (b? r/b : 0) }')"
done
echo
echo "# READ THE LAST COLUMN. A/B carries the single-thread speed difference inside it;"
echo "# r(T)/r(1) divides that out and is the only column that answers whether the two"
echo "# binaries SCALE differently. A spread with no trend is 'nothing changed'."
[ "$void" -eq 0 ] || { echo "# VOID rows above: a node budget was missed by more than $TOL%." >&2; exit 1; }
