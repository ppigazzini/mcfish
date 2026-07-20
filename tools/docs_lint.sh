#!/usr/bin/env bash
# Check the mechanical half of the documentation: dead internal links, named
# source paths that do not exist, and a bench signature quoted into prose.
#
# WHAT THIS CANNOT DO: tell you a sentence is FALSE. A page can link cleanly,
# name only real paths, quote no numbers, and still describe code that was
# replaced three commits ago. This gate buys the mechanical half so review can
# spend its attention on the half that needs a reader. See docs/11-writing.md.
set -uo pipefail

cd "$(dirname "$0")/.."

# Docs here legitimately name paths in THREE repos: mcfish's own tree, the zfish
# port source, and the Stockfish golden. A path is a valid claim if it resolves in
# any of them. Checking only mcfish would flag every upstream citation -- and the
# whole repo is about porting, so those citations are the common case, not the
# exception.
SEARCH_ROOTS=(. ../Stockfish ../zfish)

red()   { printf '\033[31m%s\033[0m\n' "$*"; }
green() { printf '\033[32m%s\033[0m\n' "$*"; }

fails=0
fail() { red "  $*"; fails=$((fails + 1)); }

mapfile -t DOCS < <(find . -name '*.md' \
  -not -path './build/*' -not -path './.git/*' -not -path './.claude/*' \
  -not -path './resources/*' -not -path './__DEV/*' | sort)

# Strip what must not be scanned, in this order:
#   1. fenced code blocks  -- shell transcripts and examples, not prose claims
#   2. inline code spans   -- `[text](target)` in 11-writing.md is a SYNTAX EXAMPLE,
#                             not a link; scanning it reports a dead link to "target"
#   3. URLs                -- github.com/.../src/nnue is a link, not a local path
strip_noise() {
  awk 'BEGIN{f=0} /^```/{f=!f; next} !f{print}' "$1" \
    | sed 's/`[^`]*`//g' \
    | sed -E 's|https?://[^ )]*||g'
}

path_exists() {
  local p=$1 root
  for root in "${SEARCH_ROOTS[@]}"; do
    [[ -e "$root/$p" ]] && return 0
  done
  return 1
}

# ---------------------------------------------------------------- dead links

for doc in "${DOCS[@]}"; do
  dir=$(dirname "$doc")

  while IFS= read -r target; do
    [[ -z $target ]] && continue
    [[ $target =~ ^(https?|mailto): ]] && continue
    [[ $target == \#* ]] && continue

    path=${target%%#*}
    [[ -z $path ]] && continue

    [[ -e "$dir/$path" ]] || fail "$doc: dead link -> $target"
  done < <(strip_noise "$doc" | grep -oE '\]\([^)]+\)' | sed -E 's/^\]\(//; s/\)$//')
done

# ------------------------------------------------- named paths must exist

# A path spelled out in prose is a claim about a tree. A BARE filename (`uci.c`)
# is not checked -- write the path if you want this gate to hold it.
for doc in "${DOCS[@]}"; do
  while IFS= read -r path; do
    [[ -z $path ]] && continue

    # A path containing a glob is a PATTERN, not a claim about one file. Docs use
    # these to describe a family ("src/engine/eval/network.*") -- often precisely
    # to say it does NOT exist yet.
    [[ $path == *'*'* ]] && continue

    path=${path%%[,.:;\`\)]}
    path_exists "$path" || fail "$doc: names a path that exists in no repo -> $path"
  done < <(strip_noise "$doc" \
           | grep -oE '\b(src|tools|tests|verify|scripts)/[A-Za-z0-9_.*/-]+' | sort -u)
done

# --------------------------------------------- no pinned signature in prose

# The anchor moves on every intended behaviour change. A doc that quotes it is
# wrong the next time it moves, and nobody thinks to grep the docs for a number.
if [[ -f tools/signature.golden ]]; then
  sig=$(grep -v '^#' tools/signature.golden | tr -d '[:space:]')
  if [[ -n $sig ]]; then
    for doc in "${DOCS[@]}"; do
      grep -q "\b$sig\b" "$doc" \
        && fail "$doc: quotes the bench signature ($sig) -- cite './build.sh signature' instead"
    done
  fi
fi

# ------------------------------------------------------------------ report

if [[ $fails -ne 0 ]]; then
  red "docs-lint: $fails problem(s)"
  exit 1
fi

green "docs-lint passed (${#DOCS[@]} files)"
