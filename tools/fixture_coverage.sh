#!/usr/bin/env bash
# Hold tools/fixture_properties.tsv to the tree, in BOTH directions.
#
# Direction 1 -- every ROW is still true: its owner exists, its fixture exists, and its
# witness still matches inside that fixture. A fixture that stops presenting its
# property is the failure this catches: the option line deleted, the position
# rewritten, the test renamed.
#
# Direction 2 -- every FIXTURE is classified. Each tools/cases/*.uci and
# tools/cases/transcript/*.uci must appear in at least one row. This is the direction
# that catches a fixture arriving without anybody answering "a representative of
# WHAT?", which is how a partition ends up exhaustive in one dimension and empty in
# another. The fixture universe is globbed from the tree, never listed here -- a
# second list would rot exactly like the first.
#
# WHAT THIS CANNOT DO: prove that presenting a property exercises the owner's branch.
# That needs coverage data, which this tree does not collect. A green run says the
# fixtures still present what the table claims, not that the branches are tested.
set -uo pipefail

cd "$(dirname "$0")/.."

TABLE=tools/fixture_properties.tsv

red() { printf '\033[31m%s\033[0m\n' "$*"; }
green() { printf '\033[32m%s\033[0m\n' "$*"; }
info() { printf '\033[36m==>\033[0m %s\n' "$*"; }

fails=0
fail() { red "  $*"; fails=$((fails + 1)); }

[[ -f $TABLE ]] || { red "fixture-coverage: no $TABLE"; exit 2; }

mapfile -t ROWS < <(grep -vE '^\s*(#|$)' "$TABLE")

# GUARD THE EXTRACTION. A table read as empty passes every per-row check below and
# reports OK over nothing, which is the shape the step floor and the path-claim floor
# already guard elsewhere in this tree.
ROW_FLOOR=30
if [[ ${#ROWS[@]} -lt $ROW_FLOOR ]]; then
  red "fixture-coverage: parsed only ${#ROWS[@]} rows (floor $ROW_FLOOR) -- the table"
  red "  or this extraction changed shape. Refusing to report OK over nothing."
  exit 2
fi

info "fixture-coverage: ${#ROWS[@]} property rows"

declare -A CLASSIFIED=()
props=0

for row in "${ROWS[@]}"; do
  IFS=$'\t' read -r property owner fixture witness <<< "$row"

  if [[ -z ${property:-} || -z ${owner:-} || -z ${fixture:-} || -z ${witness:-} ]]; then
    fail "malformed row (needs 4 tab-separated fields): $row"
    continue
  fi
  props=$((props + 1))

  [[ -e $owner ]] || fail "$property: owner does not exist -> $owner"

  if [[ ! -e $fixture ]]; then
    fail "$property: fixture does not exist -> $fixture"
    continue
  fi

  CLASSIFIED["$fixture"]=1

  grep -qE -- "$witness" "$fixture" \
    || fail "$property: $fixture no longer presents it -- witness /$witness/ matches nothing"
done

# A .uci fixture IS engine input. Both drivers pipe the file at the engine raw, so a
# `#` line is not a comment -- it is a command, the engine answers "Unknown command",
# and the case diverges for a reason that has nothing to do with what it tests. Every
# case in the tree already follows this; the convention was just never written down
# anywhere a newcomer would meet it before losing an hour to it.
for f in tools/cases/*.uci tools/cases/transcript/*.uci; do
  [[ -e $f ]] || continue
  if grep -qE '^\s*#' "$f"; then
    fail "$f has a '#' line -- a .uci file is piped RAW, so that is engine input, not a comment"
  fi
done

# Direction 2: every case file must be spoken for.
unclassified=0
for f in tools/cases/*.uci tools/cases/transcript/*.uci; do
  [[ -e $f ]] || continue
  if [[ -z ${CLASSIFIED[$f]+set} ]]; then
    fail "$f is a fixture no property row claims -- a representative of WHAT?"
    unclassified=$((unclassified + 1))
  fi
done

if [[ $fails -ne 0 ]]; then
  red "fixture-coverage: $fails problem(s)"
  exit 1
fi

green "fixture-coverage: $props properties, every fixture classified (${#CLASSIFIED[@]} fixture files)"
