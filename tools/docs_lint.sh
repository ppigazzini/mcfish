#!/usr/bin/env bash
# Check the mechanical half of the documentation: dead internal links, named
# source paths that do not exist, a bench signature quoted into prose, backticked
# symbols the tree does not contain, and a build.sh step no page mentions.
#
# WHAT THIS CANNOT DO: tell you a sentence is FALSE. Every name can resolve and the
# claim still be wrong -- a real symbol placed in the wrong file, a list with the
# wrong count or order, a flag described as absent from a build that sets it. None
# of those is reachable from here. This gate buys the mechanical half so review can
# spend its attention on the half that needs a reader. See docs/11-writing.md.
set -uo pipefail

cd "$(dirname "$0")/.."

# Docs here legitimately name paths in TWO repos: mcfish's own tree and the Stockfish
# golden. A path is a valid claim if it resolves in either. Checking only mcfish would
# flag every upstream citation -- the whole repo is about porting, so those citations
# are the common case, not the exception.
#
# ../zfish was a third root and is NOT one any more. Nothing in these pages needed it
# -- every claim resolves here or in the golden -- and carrying it meant a claim about
# THIS tree could be answered by a sibling that happens to hold the same path. That
# was live, not theoretical: docs/09 names `tools/instr_budget.golden`, which this
# repo gitignores as per-machine, and the gate was green on a box missing it purely
# because ../zfish had run its own budget. A sibling is not evidence about this tree,
# which is the rule AGENTS.md states for findings and holds for paths too.
SIBLING_ROOTS=(../Stockfish)

red()   { printf '\033[31m%s\033[0m\n' "$*"; }
green() { printf '\033[32m%s\033[0m\n' "$*"; }

fails=0
fail() { red "  $*"; fails=$((fails + 1)); }

# Lint exactly the tracked pages: untracked scratch, build output and agent
# worktrees are not documentation and carry no claims this gate owns.
mapfile -t DOCS < <(git ls-files '*.md' | sort)
# Every check below iterates DOCS, so an empty list is a green run over nothing --
# the same vacuum the step floor guards. `git ls-files` answers empty rather than
# failing outside a work tree, which is exactly when this would go quiet.
if [[ ${#DOCS[@]} -eq 0 ]]; then
  red "docs-lint: no tracked .md files found -- refusing to report on an empty set"
  exit 2
fi

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

# The same, for CamelCase. `BenchFens` survived in TWO shipped files -- a header
# comment and docs/07-shell.md -- describing a table that had not existed for weeks,
# because the scan above holds snake_case only and every type, table and struct name
# in this tree is CamelCase. Require TWO capitalised words: one capitalised word is
# prose far more often than an identifier ("Position", "Move", "Hash"), and upstream's
# own single-word type names would flag on every citation.
camel_spans() {
  awk 'BEGIN{f=0} /^```/{f=!f; next} !f{print}' "$1" \
    | grep -oE '`[A-Z][a-z0-9]+([A-Z][A-Za-z0-9]*)+`' | tr -d '`'
}

# Answer "does this path exist" against the TREE, not the working directory. `[[ -e ]]`
# reads local state, so the verdict depended on what a checkout happened to hold, in two
# opposite and silent directions: a file a rename leaves behind is green here and DEAD in
# a fresh clone, and a path .gitignore deliberately excludes could never be green anywhere.
# ../rfish took a red CI run on the first class (its 3ac1b08); ../zfish closed both before
# either bit (25976c37).
#
# EXEMPT AN IGNORED PATH. One the repository decided not to carry is a doc naming the tool
# that WRITES it -- `tools/instr_budget.golden` is per-machine by design -- rather than a
# claim about a tracked file. That exemption is a hole and is stated rather than hidden: a
# dead IGNORED path is checked by nothing here.
#
# Take the index ONCE into a set. This answers every link target and every path claim, and
# a `git` per lookup is the difference between a gate you run and one you skip. The two git
# calls below are the MISS path only, where `--error-unmatch` normalises a pathspec so a
# link written `docs/../AGENTS.md` still resolves.
#
# NO FILESYSTEM FALLBACK, because there is no case that could reach one. ../zfish's
# version carries a "not a git checkout" arm that resolves against the filesystem and says
# so; here DOCS comes from `git ls-files` too, so a tarball export refuses at exit 2 on the
# empty-page guard above before any path is resolved. Verified by running this gate inside
# `git archive HEAD | tar -x`: "no tracked .md files found", exit 2. An arm that cannot run
# is not a safety net, it is a claim nobody checks.
declare -A TRACKED=()
while IFS= read -r tracked_path; do TRACKED["$tracked_path"]=1; done < <(git ls-files)

in_this_tree() {
  local p=${1#./}
  [[ -n ${TRACKED[$p]+set} ]] && return 0
  git ls-files --error-unmatch -- "$p" >/dev/null 2>&1 && return 0
  git check-ignore -q -- "$p"
}

# This tree answers first, and a sibling only for what this tree does not own -- an
# upstream citation. The golden is a foreign checkout, so it stays on the filesystem:
# its index is not this gate's business, and a golden mid-rebase must not redden mcfish.
path_exists() {
  local p=$1 root
  in_this_tree "$p" && return 0
  for root in "${SIBLING_ROOTS[@]}"; do
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

    # Same resolver as the path claims: a link into the tree is a path claim that a
    # reader clicks, and it rots on a rename the same way.
    in_this_tree "$dir/$path" || fail "$doc: dead link -> $target"
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

# ------------------------------------ named paths IN BACKTICKS must exist too
#
# The check above reads PROSE only, because strip_noise deletes inline code spans
# before it runs -- and a path in these pages is written `like/this.c` far more often
# than bare, so the commoner spelling was the unheld one. Read the spans DIRECTLY,
# the way the symbol check below does. Do NOT route this through strip_noise: that
# helper deletes the very spans this reads, and the check would then scan nothing
# and pass everything. ../zfish 26d197dd is the sibling's version of this widening.
#
# Require an EXTENSION. That is what confines this to FILE claims: a directory
# (`src/engine/board/`) resolves against no file list, and every claim of that shape
# in these pages names a family rather than a thing that can be renamed out from
# under the prose. It is also why this needs no separate directory rule.
#
# THE SENTINEL: docs/11-writing.md has to be able to SPELL a dead reference in order
# to rule on one. It uses the path below, which this repository guarantees never
# exists -- and the guard under the loop enforces that guarantee, so the exemption
# cannot quietly grow to cover a real file.
PATH_SENTINEL=src/does/not/exist.c
path_claims=0
for doc in "${DOCS[@]}"; do
  while IFS= read -r path; do
    [[ -z $path ]] && continue
    path_claims=$((path_claims + 1))
    [[ $path == "$PATH_SENTINEL" ]] && continue
    path_exists "$path" || fail "$doc: names a path that exists in no repo -> $path"
  done < <(grep -ohE '`(src|tools|tests|verify|scripts|docs|build|resources|\.github)/[A-Za-z0-9_/.-]+\.[A-Za-z0-9]+`' \
             "$doc" | tr -d '`' | sort -u)
done

# GUARD THE EXTRACTION, NOT ONLY ITS VERDICT -- the step floor's lesson, applied to
# the subject it was learnt on. A typo in the pattern above finds nothing, and a
# check that found nothing reports OK over everything.
PATH_CLAIM_FLOOR=24
if [[ $path_claims -lt $PATH_CLAIM_FLOOR ]]; then
  red "docs-lint: extracted only $path_claims backticked path claims (floor $PATH_CLAIM_FLOOR)"
  red "  the prose or the pattern changed shape -- refusing to report OK over nothing"
  exit 2
fi

# An exemption that covers a real file is not an exemption. If the sentinel ever
# resolves, the loop above has been skipping a live claim.
path_exists "$PATH_SENTINEL" \
  && fail "docs-lint: the dead-path sentinel $PATH_SENTINEL EXISTS -- its exemption is now a hole"

# -------------------------- no shipped file names the gitignored dev area
#
# That directory is working state, not repository surface. It is gitignored, so a path
# under it resolves for its author and for nobody else -- and a shipped file naming one
# sends every other reader to something their checkout does not contain. That is not
# hypothetical: the finish-line gate spent two weeks telling a failing porter to read a
# page that had been moved there, and the move's own dewiring could not see the citation
# because it lived in a .sh. Only .gitignore may say the word.
#
# Two files are exempt and both are structural: .gitignore declares the directory, and
# this one carries the pattern that finds it.
while IFS= read -r hit; do
  [[ -z $hit ]] && continue
  fail "$hit: names __DEV -- no shipped file may cite the gitignored dev area"
done < <(git grep -In '__DEV' -- . ':!.gitignore' ':!tools/docs_lint.sh' | cut -d: -f1,2 | sort -u)

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
# nothing else notices -- every other gate reads the code, not the pages.
#
# The corpus is every tracked source, script and workflow PLUS the tracked path
# list, so a doc may name a tool by its filename (`nps_ab`) or a build.sh function
# (`do_parity`) as well as a C symbol. Only tokens containing an underscore are
# checked: a bare word is prose far more often than it is an identifier.
# Symbols the docs may name that the tree does not contain -- a platform API named
# while explaining a port, and nothing else. Each is argued in the file, and the list
# EXPIRES in its own direction: an entry that turns out to exist fails below, because
# a stale exemption silently starts covering the rename it was never meant to cover.
declare -A EXTERNAL_OK=()
if [[ -f tools/docs_lint.external ]]; then
  while IFS= read -r ext; do
    [[ -z $ext || $ext == \#* ]] && continue
    EXTERNAL_OK["$ext"]=1
  done < <(grep -vE '^\s*(#|$)' tools/docs_lint.external)
fi

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

  while read -r sym; do
    [[ -z $sym ]] && continue
    [[ -n ${EXTERNAL_OK[$sym]+set} ]] && continue
    grep -qw -- "$sym" <<< "$symbol_corpus" \
      || fail "$doc: names a CamelCase symbol that exists nowhere in the tree -> $sym"
  done < <(camel_spans "$doc" | sort -u)
done

# The exemption list expires in its own direction: an entry that EXISTS in the tree is
# no longer an exemption, it is a hole aimed at a live symbol.
for ext in "${!EXTERNAL_OK[@]}"; do
  grep -qw -- "$ext" <<< "$symbol_corpus" \
    && fail "tools/docs_lint.external exempts '$ext', which now EXISTS in the tree -- delete the line"
done

# ------------------------------------------- every build.sh step is documented

# The step table in 09-tooling-ci.md claims to say what each step proves, so a step
# added without a row is a feature nobody reading the docs can find.
#
# GUARD THE EXTRACTION, NOT ONLY THE VERDICT. This reads build.sh as TEXT, keyed on
# the last `case` and on one-line arms, so a reformat that puts a step name on its
# own line, or a dispatcher moved out of this file, yields an EMPTY list -- and an
# empty list means the loop below runs zero times and the gate reports OK while
# covering nothing. That is not hypothetical: ../zfish's equivalent matched 5 of its
# 77 steps for as long as it existed, green throughout, because its build file spells
# most steps across three lines (zfish 108e7af6). A floor turns that silence into a
# failure. Keep it just under the real count so adding steps never trips it and
# losing the extraction always does.
mapfile -t STEPS < <(sed -n "$(grep -n '^case ' build.sh | tail -1 | cut -d: -f1),\$p" build.sh \
                     | grep -oE '^\s*[a-z][a-z0-9|-]*\)' | tr -d ' )' | tr '|' '\n' \
                     | grep -v '^-' | sort -u)
STEP_FLOOR=35
if [[ ${#STEPS[@]} -lt $STEP_FLOOR ]]; then
  red "docs-lint: parsed only ${#STEPS[@]} build.sh steps (floor $STEP_FLOOR)"
  red "  the step extraction reads build.sh as text -- it has gone stale, not clean"
  exit 2
fi

# ------------------------------- a cited ./build.sh step must be a real step
#
# The reverse of the check below, and the one that would have caught `port-status`:
# that step was deleted when the port apparatus was retired, and two shipped surfaces
# went on telling a reader to run it -- one of them the finish-line gate's own failure
# message, which is read at exactly the moment somebody needs it to be true. Prose is
# checked against the dispatch table rather than against a second list.
#
# Read the citations from the same pages the rest of this gate owns, plus the tool and
# workflow comments, because that is where both survivors lived.
# ONLY BACKTICKED OR QUOTED CITATIONS. `./build.sh` also appears mid-sentence --
# "every ./build.sh step runs the engine from here", "run ./build.sh first" -- and a
# bare scan reads the next English word as a step name. A citation someone is meant to
# TYPE is written in backticks in a page or inside a quoted string in a script, which
# is exactly where both survivors of the port-apparatus retirement lived.
mapfile -t CITED_STEPS < <(
  { cat "${DOCS[@]}"; git ls-files 'tools/*.sh' 'tools/**/*.sh' '.github/workflows/*.yml' | xargs cat 2>/dev/null; } \
    | grep -oE '[`"]\./build\.sh [a-z][a-z0-9-]*' | awk '{print $2}' | sort -u
)
for step in "${CITED_STEPS[@]}"; do
  [[ -z $step ]] && continue
  printf '%s\n' "${STEPS[@]}" | grep -qxF -- "$step" \
    || fail "a shipped file says './build.sh $step', which is not a step build.sh dispatches"
done

for step in "${STEPS[@]}"; do
  [[ -z $step ]] && continue
  grep -qF -- "$step" "${DOCS[@]}" \
    || fail "docs: ./build.sh '$step' is a real step but no tracked page mentions it"
done

# ------------------------------------------------------------------ report

if [[ $fails -ne 0 ]]; then
  red "docs-lint: $fails problem(s)"
  exit 1
fi

# Print the denominator, so coverage is legible in the pass line rather than
# inferable from it -- the same reason the transcript gate prints its case count.
green "docs-lint passed (${#DOCS[@]} files, $path_claims backticked path claims)"
