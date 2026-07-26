#!/usr/bin/env bash
# Check the mechanical half of the documentation: dead internal links, named
# source paths that do not exist, a bench signature quoted into prose, backticked
# symbols the tree does not contain, and a build.sh step no page mentions.
#
# WHAT THIS CANNOT DO: tell you a sentence is FALSE. A page can link cleanly, name
# only real paths and real symbols, quote no numbers, and still describe code that
# was replaced three commits ago -- wrong ORDER, wrong COUNT, wrong reason. An
# audit found `parity` documented as nine gates when it runs ten, and "there is no
# LTO in the build" on a page whose build has had `-flto` throughout; neither is
# reachable from here. This gate buys the mechanical half so review can spend its
# attention on the half that needs a reader. See docs/11-writing.md.
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

# Lint exactly the tracked pages: untracked scratch, build output and agent
# worktrees are not documentation and carry no claims this gate owns.
mapfile -t DOCS < <(git ls-files '*.md' | sort)

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

# The INVERSE of strip_noise's second step: emit the snake_case identifiers inside
# inline code spans. Fenced blocks are still dropped -- those are transcripts and
# examples, which may legitimately name things this tree does not have. Keep this
# next to strip_noise: the two must agree on what a fenced block is, and a symbol
# scan built on strip_noise silently matches nothing, because that function
# deletes the very spans this one reads.
code_spans() {
  awk 'BEGIN{f=0} /^```/{f=!f; next} !f{print}' "$1" \
    | grep -oE '`[a-z][a-z0-9]*(_[a-z0-9]+)+`' | tr -d '`'
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

# ------------------------------------------- backticked symbols must exist

# A `snake_case` token in backticks is a claim that the tree contains that name.
# Renames are what break it: the code moves, the prose keeps the old spelling, and
# nothing notices -- this gate was added after an audit found `tt_store` (now
# `tt_save`), `entry_relative_age` (`tt_entry_relative_age`), `uci_start_logger`
# (`uci_output_start_logger`) and `pos_see_ge` (a duplicate collapsed long ago)
# all still named in the pages.
#
# The corpus is every tracked source, script and workflow PLUS the tracked path
# list, so a doc may name a tool by its filename (`nps_ab`) or a build.sh function
# (`do_parity`) as well as a C symbol. Only tokens containing an underscore are
# checked: a bare word is prose far more often than it is an identifier.
symbol_corpus=$(
  git ls-files | grep -E '\.(c|h|sh|py|yml)$' | xargs cat 2>/dev/null
  git ls-files
)
for doc in "${DOCS[@]}"; do
  while read -r sym; do
    [[ -z $sym ]] && continue
    grep -qw -- "$sym" <<< "$symbol_corpus" \
      || fail "$doc: names a symbol that exists nowhere in the tree -> $sym"
  done < <(code_spans "$doc" | sort -u)
done

# ------------------------------------------- every build.sh step is documented

# The step table in 09-tooling-ci.md claims to say what each step proves. A step
# added without a row is a feature nobody can find; `net-fetch`, `pgo` and
# `perf-budget-update` were each absent from every page when this was added.
while read -r step; do
  [[ -z $step ]] && continue
  grep -qF -- "$step" "${DOCS[@]}" \
    || fail "docs: ./build.sh '$step' is a real step but no tracked page mentions it"
done < <(sed -n "$(grep -n '^case ' build.sh | tail -1 | cut -d: -f1),\$p" build.sh \
         | grep -oE '^\s*[a-z][a-z0-9|-]*\)' | tr -d ' )' | tr '|' '\n' \
         | grep -v '^-' | sort -u)

# ------------------------------------------------------------------ report

if [[ $fails -ne 0 ]]; then
  red "docs-lint: $fails problem(s)"
  exit 1
fi

green "docs-lint passed (${#DOCS[@]} files)"
