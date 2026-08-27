#!/usr/bin/env bash
# Every commit SHA the tracked docs cite still names a commit a reader can reach.
#
# WHAT THIS PROVES. For each backticked hex token of 7..12 digits in a tracked
# `.md`: it is a commit that is an ANCESTOR of HEAD, or a commit reachable from
# some ref here, or a commit in one of the sibling checkouts this tree cites by
# name. Nothing else.
#
# WHAT IT CANNOT SEE. Whether the commit a SHA names is the commit the sentence
# means. A rebase that rewrites a citation onto a reachable but WRONG commit
# passes this cleanly. The durable fix is not a gate at all -- it is a subject
# beside every SHA, `2e3dd920` "perf: lift the low-ply term out of the loop",
# which survives any rebase and is greppable where a SHA is a convenience. This
# gate prints the subject of everything it reports so a repair is a paste.
#
# WHY EXISTENCE IS THE WRONG TEST, and it is the reason this file exists:
#
#     git cat-file -e "$sha^{commit}"        # WRONG
#
# That asks whether the object is in THIS clone. A branch that has been rebased
# leaves its pre-rebase commits in the object store, and any backup ref pins them
# indefinitely -- so a citation to a pre-rebase identity resolves on the author's
# machine, forever, and resolves nowhere else. The reachability test is the one
# that transfers:
#
#     git merge-base --is-ancestor "$sha" HEAD
#
# THREE TIERS, because two would report the wrong thing here. Off-branch is not a
# defect in this tree: `tools/upstream/PORT_SOURCES.md` cites the upstream commit
# this port started from, which is reachable from a ref but is an ancestor of
# nothing on this branch. Only a SHA reachable from NOTHING is a finding -- that
# one is a single `git gc --prune` from unresolvable even here.
#
# AND A FOURTH TIER THIS TREE NEEDS AND REFISH DOES NOT. mcfish's pages cite the
# SIBLING PORTS by SHA -- rfish, zfish and refish commits appear in AGENTS.md by
# design -- and those are not objects in this repository at all. refish skips a
# non-commit silently, which here would mean the majority of citations are checked
# by nothing. So a token that resolves nowhere locally is looked up in the sibling
# checkouts and ATTRIBUTED to the one that holds it. A missing sibling narrows the
# gate and says which, the way `tb` and `malformed` narrow; it never fails for it.
#
# Exit codes:  0 clean   1 findings   2 rig fault
set -u
set -o pipefail

ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd) || exit 2
cd "$ROOT" || exit 2

git rev-parse --git-dir >/dev/null 2>&1 || {
  printf '\033[31mcite-check: not a git repository -- rig fault\033[0m\n'; exit 2; }

# A shallow clone cannot answer an ancestry question, and answering it anyway
# would report every citation as dangling. Narrow, and say so.
if [[ $(git rev-parse --is-shallow-repository 2>/dev/null) == true ]]; then
  printf '\033[36m==>\033[0m cite-check: SHALLOW clone -- ancestry is unanswerable here.\n'
  printf '\033[36m    NARROWED to nothing. Re-run with a full clone.\033[0m\n'
  exit 0
fi

# The siblings this tree cites, in the order tools/upstream/README.md names them.
SIBLINGS=(../Stockfish ../rfish ../zfish ../fcfish)

red=$'\033[31m'; green=$'\033[32m'; cyan=$'\033[36m'; off=$'\033[0m'

mapfile -t PAGES < <(git ls-files '*.md')
[[ ${#PAGES[@]} -gt 0 ]] || { printf '%scite-check: no tracked .md -- the file list went stale%s\n' "$red" "$off"; exit 2; }

# Backticked hex, 7..12 digits. The lower bound is git's own abbreviation floor;
# the upper stops short of 40 so a full-length net hash is not swept in.
# shellcheck disable=SC2016  # the backticks are markdown being MATCHED, not a substitution
mapfile -t SHAS < <(git grep -ohE '`[0-9a-f]{7,12}`' -- '*.md' | tr -d '`' | sort -u)

printf '%s==>%s cite-check: %d tracked pages, %d cited SHAs, HEAD %s\n' \
  "$cyan" "$off" "${#PAGES[@]}" "${#SHAS[@]}" "$(git rev-parse --short HEAD)"

missing_siblings=()
for s in "${SIBLINGS[@]}"; do
  [[ -d $s/.git ]] || missing_siblings+=("$s")
done
if [[ ${#missing_siblings[@]} -gt 0 ]]; then
  printf '%s    no checkout at %s -- a SHA only they hold cannot be attributed%s\n' \
    "$cyan" "${missing_siblings[*]}" "$off"
fi

# Where is this SHA cited? Used only in the report, so a repair is a paste.
pages_citing() { git grep -l -F "\`$1\`" -- '*.md' | tr '\n' ' '; }

ancestor=0 offbranch=0 sibling=0 dangling=0 unresolved=0 narrowed=0
for sha in "${SHAS[@]}"; do
  if [[ $(git cat-file -t "$sha" 2>/dev/null) == commit ]]; then
    if git merge-base --is-ancestor "$sha" HEAD 2>/dev/null; then
      ancestor=$((ancestor + 1))
      continue
    fi
    if [[ -n $(git for-each-ref --contains "$sha" --format='%(refname)' 2>/dev/null | head -1) ]]; then
      offbranch=$((offbranch + 1))
      continue
    fi
    printf '  %sDANGLING%s  %s  %s\n' "$red" "$off" "$sha" "$(git log -1 --format=%s "$sha" 2>/dev/null)"
    printf '            reachable from no ref -- one gc --prune from unresolvable\n'
    printf '            cited by: %s\n' "$(pages_citing "$sha")"
    dangling=$((dangling + 1))
    continue
  fi

  found=""
  for repo in "${SIBLINGS[@]}"; do
    [[ -d $repo/.git ]] || continue
    if [[ $(git -C "$repo" cat-file -t "$sha" 2>/dev/null) == commit ]]; then
      found=$repo
      break
    fi
  done
  if [[ -n $found ]]; then
    sibling=$((sibling + 1))
    continue
  fi

  # A SHA that resolves nowhere is a FINDING only when every sibling was
  # available to answer for it. With one missing, "not here" and "in the
  # checkout you do not have" are the same observation, so this narrows and
  # says which -- the way `tb` and `malformed` narrow rather than fail.
  if [[ ${#missing_siblings[@]} -gt 0 ]]; then
    printf '  %sNARROWED%s    %s  unresolved, but %s %s absent\n' "$cyan" "$off" "$sha" \
      "${missing_siblings[*]}" "$([[ ${#missing_siblings[@]} -eq 1 ]] && echo is || echo are)"
    narrowed=$((narrowed + 1))
    continue
  fi

  printf '  %sUNRESOLVED%s  %s  is a commit in no tracked repository\n' "$red" "$off" "$sha"
  printf '              cited by: %s\n' "$(pages_citing "$sha")"
  unresolved=$((unresolved + 1))
done

printf '  %d ancestor of HEAD, %d off-branch but reachable, %d in a sibling\n' \
  "$ancestor" "$offbranch" "$sibling"
[[ $narrowed -eq 0 ]] || printf '  %s%d unchecked -- a sibling checkout is absent, so this run NARROWED%s\n' \
  "$cyan" "$narrowed" "$off"

if [[ $((dangling + unresolved)) -gt 0 ]]; then
  printf '%scite-check: %d dangling, %d unresolved -- fix the SHA, or quote the subject beside it%s\n' \
    "$red" "$dangling" "$unresolved" "$off"
  exit 1
fi
if [[ $narrowed -gt 0 ]]; then
  printf '%scite-check: %d of %d cited SHAs resolve; %d NOT CHECKED (sibling absent)%s\n' \
    "$green" "$((ancestor + offbranch + sibling))" "${#SHAS[@]}" "$narrowed" "$off"
else
  printf '%scite-check: every cited SHA resolves (%d of %d)%s\n' \
    "$green" "$((ancestor + offbranch + sibling))" "${#SHAS[@]}" "$off"
fi
