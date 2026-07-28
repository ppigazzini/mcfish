#!/usr/bin/env bash
# Paired A/B search speed between two engine binaries, over an identical tree.
#
# THE HEADLINE SPEED TOOL, and the one that has to be right: this is the number that
# predicts Elo. It needs no instrumentation -- both binaries run as shipped -- and it
# reads each engine's OWN `Total time`, which bench starts after the `ucinewgame`
# clear and which therefore contains no process startup. That last property matters
# more than it sounds: startup is engine-dependent here (mcfish parses the net in
# about half upstream's time), and every attempt to remove it arithmetically from a
# whole-process counter has produced a wrong answer.
#
# THE PROTOCOL, AND WHY EACH PART EXISTS. Every rule below was paid for by a wrong
# result, and the last two were paid for by publishing "the spine is at parity" while
# a 13% deficit sat in front of it.
#
#  * INTERLEAVED, NOT BATCHED. Absolute speed is thermally void: the same binary
#    reads wildly different numbers between batches. Only same-run pairs count, and a
#    number from a previous session is NEVER comparable.
#  * ALTERNATE WHICH ENGINE RUNS FIRST. Within a round the second slot runs on a
#    hotter core, which is a systematic bias in whichever direction the order is
#    fixed. Alternating cancels it; without this the tool cannot resolve a few percent
#    and will confidently report the wrong sign of a small effect.
#  * SUM OVER A POSITION SET -- NEVER MEDIAN PER-POSITION TIMES. Search sizes vary by
#    orders of magnitude across positions, so a median is decided by which positions
#    land in the middle. The same binaries measured that way read 1.091 at depth 12
#    and 0.906 at depth 14. `bench` sums, which is the whole reason to drive this
#    through bench rather than through repeated `go` commands.
#  * MEASURE THE BINARIES THAT PLAY THE GAMES. A tier or a build mode changes the
#    answer: on one pair here the deficit doubled between sse41-plain and icl-PGO.
#    Never generalise a ratio from one build to another.
#  * NEVER BUILD IN THE SAME COMMAND. Compiling first leaves the machine hot. Build,
#    let it idle, then measure. This script only runs.
#  * BUILD BOTH SIDES AT THE SAME ARCH, or the comparison measures the ISA.
#
# CHOOSE THE POSITION SET DELIBERATELY. bench's 4th argument takes a FEN file, and
# the default list is not a game-like sample -- it is 51 fixed positions that every
# measurement in this repo has been taken on, and that the PGO profile is trained on.
# Quote which set a ratio came from; measure on the openings the matches actually
# play when the question is about Elo.
#
# Node counts are asserted equal up front and on every round: a different tree is a
# different workload and the ratio would be meaningless.
#
# Usage: nps_ab.sh <binA> <binB> [rounds] [bench-args...]  (CWD must hold the net)
#        nps_ab.sh ./mcfish <oracle>/src/stockfish 9 16 1 13
#        nps_ab.sh ./mcfish <oracle>/src/stockfish 9 16 1 13 /path/to/openings.fen
set -u

A="${1:?usage: nps_ab.sh <binA> <binB> [rounds] [bench-args...]  (run from the dir holding the net)}"
B="${2:?usage: nps_ab.sh <binA> <binB> [rounds] [bench-args...]}"
ROUNDS="${3:-9}"
shift 3 2>/dev/null || shift $#
BENCH="${*:-16 1 13}"
CORE="${CORE:-0}"

for f in "$A" "$B"; do [ -x "$f" ] || { echo "error: $f not executable" >&2; exit 1; }; done

# `Total time` is the search-only clock: bench takes its start AFTER the ucinewgame
# clear, so no process startup is inside it. Nodes come back for the tree assertion.
run() { taskset -c "$CORE" "$1" bench $BENCH 2>&1 |
        sed -n 's/^Total time (ms)  *: *\([0-9]*\)/T\1/p; s/^Nodes searched  *: *\([0-9]*\)/N\1/p'; }

read_pair() { # -> "<ms> <nodes>"
  local t n
  while read -r line; do
    case $line in T*) t=${line#T} ;; N*) n=${line#N} ;; esac
  done < <(run "$1")
  echo "$t $n"
}

echo "# bench $BENCH | $ROUNDS rounds | core $CORE | order alternates each round"
RATIOS=()
for i in $(seq "$ROUNDS"); do
  # Alternate the order so the hotter second slot does not always fall on one engine.
  if [ $((i % 2)) -eq 1 ]; then
    read -r ta na <<<"$(read_pair "$A")"; read -r tb nb <<<"$(read_pair "$B")"
  else
    read -r tb nb <<<"$(read_pair "$B")"; read -r ta na <<<"$(read_pair "$A")"
  fi
  if [ "$na" != "$nb" ]; then
    echo "error: node counts differ (A=$na, B=$nb) on round $i." >&2
    echo "       Different trees = different workloads; the ratio would be meaningless." >&2
    exit 1
  fi
  r=$(awk -v x="$ta" -v y="$tb" 'BEGIN{printf "%.4f", (y>0)? x/y : 0}')
  RATIOS+=("$r")
  printf "round %-3s A=%-7s ms  B=%-7s ms  A/B=%s   (%s nodes)\n" "$i" "$ta" "$tb" "$r" "$na"
done

printf '%s\n' "${RATIOS[@]}" | sort -n | awk '
  {v[NR]=$1}
  END{
    m = (NR%2) ? v[(NR+1)/2] : (v[NR/2]+v[NR/2+1])/2
    printf "\n# MEDIAN PAIRED A/B search time = %.4f  (A is %+.1f%% vs B)\n", m, (m-1)*100
    printf "# spread %.4f..%.4f over %d rounds\n", v[1], v[NR], NR
    if (v[1] <= 1.0 && v[NR] >= 1.0)
      printf "# The spread STRADDLES 1.000 -- this run does not establish a direction.\n"
    else
      printf "# The spread excludes 1.000 -- the direction is established at this sample size.\n"
    printf "# Time is search-only (bench starts its clock after ucinewgame), so no startup is in it.\n"
  }'
