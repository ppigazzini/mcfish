#!/usr/bin/env bash
# Report how far the port is from bit-exactness, straight from the port map.
#
# This is the number to quote — never a figure written into prose, which goes
# stale the next time a module lands. See docs/PORTING.md.
set -euo pipefail

cd "$(dirname "$0")/.."

MAP=tools/upstream/port_map.tsv

[[ -f $MAP ]] || { echo "missing $MAP" >&2; exit 1; }

# Strip comments and blanks; the remaining rows are the work list.
rows=$(grep -v '^#' "$MAP" | grep -v '^[[:space:]]*$')

count() { printf '%s\n' "$rows" | awk -F'\t' -v s="$1" '$4 == s' | wc -l | tr -d ' '; }

ported=$(count PORTED)
partial=$(count PARTIAL)
todo=$(count TODO)
skip=$(count SKIP)
total=$(printf '%s\n' "$rows" | wc -l | tr -d ' ')
live=$((total - skip))
done_pct=$(( (ported * 100) / (live > 0 ? live : 1) ))

printf '\n\033[1mccfish port status\033[0m — target: bit-exact 1:1 Stockfish clone\n\n'
printf '  upstream base : %s\n' "$(cat tools/upstream/UPSTREAM_BASE 2>/dev/null || echo '<unpinned>')"
printf '  port source   : ../zfish (Zig, already bit-exact)\n'
printf '  golden        : ../Stockfish\n\n'
printf '  \033[32mPORTED \033[0m %3d\n' "$ported"
printf '  \033[33mPARTIAL\033[0m %3d   (compiles, not upstream-faithful)\n' "$partial"
printf '  \033[31mTODO   \033[0m %3d\n' "$todo"
printf '  SKIP     %3d\n' "$skip"
printf '  %s\n' '-------------------'
printf '  %d modules in scope, %d%% claimed ported\n' "$live" "$done_pct"

# VALIDATE THE CLAIM AGAINST THE TREE AND THE BUILD.
#
# Every figure above comes from a hand-edited TSV that nothing checks. A row can
# name a path that was renamed away, or a file that exists but is absent from
# build.sh's source list -- never compiled, never warned on, never linked, never
# tested. Both read as PORTED and inflate the percentage. The number that means
# something is the one backed by code the binary actually contains, so compute it
# here and print it as the headline; the claimed figure above is context for it.
compiled=$(sed -n '/^SOURCES=(/,/^)/p;/^ENGINE_SOURCES=(/,/^)/p' build.sh \
           | grep -oE 'src/[A-Za-z0-9_/]+\.c' | sort -u)

n_compiled=0; n_uncompiled=0; n_missing=0
missing_list=""; uncompiled_list=""
while IFS=$'\t' read -r up cc _rest; do
  [[ -n ${cc:-} ]] || continue
  if [[ ! -e $cc ]]; then
    n_missing=$((n_missing + 1)); missing_list+="    $cc"$'\n'
  elif printf '%s\n' "$compiled" | grep -qxF "$cc"; then
    n_compiled=$((n_compiled + 1))
  else
    n_uncompiled=$((n_uncompiled + 1)); uncompiled_list+="    $cc"$'\n'
  fi
done < <(printf '%s\n' "$rows" | awk -F'\t' '$4 == "PORTED"')

real_pct=$(( (n_compiled * 100) / (live > 0 ? live : 1) ))
printf '\n  \033[1mBacked by compiled code: %d of %d PORTED rows (%d%% of scope)\033[0m\n' \
  "$n_compiled" "$ported" "$real_pct"

if [[ $n_missing -gt 0 ]]; then
  printf '  \033[31m%d PORTED rows name a path that does not exist:\033[0m\n' "$n_missing"
  printf '%s' "$missing_list" | head -8
  [[ $n_missing -gt 8 ]] && printf '    ... and %d more\n' "$((n_missing - 8))"
fi
if [[ $n_uncompiled -gt 0 ]]; then
  printf '  \033[33m%d PORTED rows exist but are NOT compiled (dead code):\033[0m\n' "$n_uncompiled"
  printf '%s' "$uncompiled_list" | head -8
  [[ $n_uncompiled -gt 8 ]] && printf '    ... and %d more\n' "$((n_uncompiled - 8))"
fi
printf '\n'

# Break the remaining work down by risk, so the next slice can be chosen on cost.
printf '  Remaining (TODO + PARTIAL) by risk tier:\n'
for tier in HIGH MED LOW; do
  n=$(printf '%s\n' "$rows" | awk -F'\t' -v t="$tier" '($4=="TODO"||$4=="PARTIAL") && $5==t' | wc -l | tr -d ' ')
  printf '    %-5s %3d\n' "$tier" "$n"
done

printf '\n  Next unstarted HIGH-risk modules:\n'
next_high=$(printf '%s\n' "$rows" \
  | awk -F'\t' '$4=="TODO" && $5=="HIGH" {printf "    %s -> %s\n", $1, $2}')
printf '%s\n' "$next_high" | awk 'NR <= 8'

printf '\n  Read docs/PORTING.md for the milestone order.\n\n'
