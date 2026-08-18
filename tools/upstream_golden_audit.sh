#!/usr/bin/env bash
# Adjudicate every UCI golden against a PRISTINE upstream build.
#
# A golden regenerated from mcfish is a photograph of mcfish: it pins a defect exactly
# as faithfully as it pins correct behaviour, after which the gate passes BECAUSE the
# engine is wrong. tools/GOLDEN_PROVENANCE.md records that this already happened twice
# here -- board.golden pinned a `d` output with no `Checkers:` line, errors.golden
# pinned three invalid FENs producing no diagnostic at all -- and both gates were green
# throughout.
#
# The tree's answer has always been "derive it from the oracle", but that was an
# instruction with no tool behind it: `./build.sh golden-update` drives mcfish, so every
# resync silently converts oracle-derived goldens back into self-photographs while
# GOLDEN_PROVENANCE.md goes on claiming they are upstream's bytes. This is the tool.
# It drives the SAME case scripts through upstream's own binary and diffs the result
# against the committed golden, so `golden` becomes a differential rather than a
# memory of what mcfish last did.
#
# Ported from zfish's tools/upstream_golden_audit.sh (zfish 6ce3d141).
#
# Usage:
#   upstream_golden_audit.sh                    # audit every case
#   upstream_golden_audit.sh search netswap     # audit only these
#   upstream_golden_audit.sh --skip errors      # audit every case BUT these
#   upstream_golden_audit.sh --write            # re-derive the differing goldens FROM the oracle
#   upstream_golden_audit.sh --list             # print the case list and exit
#   ORACLE_SHA=<sha>                            # adjudicate against another commit
#   GO_SETTLE=<seconds>                         # per-`go` pause (default 5)
#
# --write is the regenerator `./build.sh golden-update` cannot be: it writes upstream's
# own bytes, so the golden it produces is adjudicated by construction. Use it in place
# of golden-update whenever the oracle can answer the case -- which, it turns out, is
# all of them. golden-update remains only for a case upstream cannot be driven through.
#
# Prefer --skip over naming cases positionally when the reason is a missing fixture
# rather than a narrowed run: a positional list silently stops covering every case
# added after it was written, where --skip keeps picking new ones up.
#
# TWO RIG DETAILS, both of which read as divergences when you get them wrong:
#
#   * CWD. Every mcfish gate runs the engine from resources/, which is where the net
#     lives. The oracle must run from there too, or it loads no net and every
#     evaluation-bearing case differs for a reason that is not a divergence.
#
#   * `go` IS ASYNCHRONOUS UPSTREAM. mcfish's `go` is synchronous, so the gate itself
#     may pipe a whole script in at once; upstream searches on another thread, and a
#     piped `go` is cut short by the next line and yields a depth-1 stub. Feeding the
#     script line by line with a settle after each `go` is what makes the two
#     comparable. GOLDEN_PROVENANCE.md records this producing a false result once
#     already: `search` was recorded as un-adjudicable when driving the oracle
#     properly shows it is byte-identical.
set -uo pipefail

cd "$(dirname "$0")/.." || exit 2
ROOT=$PWD
RESOURCES=$ROOT/resources
GO_SETTLE=${GO_SETTLE:-5}

red() { printf '\033[31m%s\033[0m\n' "$*" >&2; }
green() { printf '\033[32m%s\033[0m\n' "$*"; }
info() { printf '\033[36m==>\033[0m %s\n' "$*"; }

CASES=()
for script in tools/cases/*.uci; do CASES+=("$(basename "$script" .uci)"); done

if [[ ${1:-} == --list ]]; then
  printf '%s\n' "${CASES[@]}"
  exit 0
fi

known() {
  local c
  for c in "${CASES[@]}"; do [[ $c == "$1" ]] && return 0; done
  # Reject an unknown name rather than quietly auditing nothing: a typo in a CI
  # --skip would otherwise read as a clean run over a set that never excluded what
  # it meant to.
  red "golden-audit: unknown case '$1' (see --list)"
  exit 2
}

SKIP=()
WANT=()
WRITE=0
while [[ $# -gt 0 ]]; do
  case "$1" in
    --skip)
      [[ $# -ge 2 ]] || { red "golden-audit: --skip needs a case name"; exit 2; }
      known "$2"; SKIP+=("$2"); shift 2 ;;
    --write) WRITE=1; shift ;;
    -*) red "golden-audit: unknown flag '$1'"; exit 2 ;;
    *)  known "$1"; WANT+=("$1"); shift ;;
  esac
done

SELECTED=()
for c in "${CASES[@]}"; do
  skip=0
  for s in ${SKIP+"${SKIP[@]}"}; do [[ $s == "$c" ]] && skip=1; done
  [[ $skip == 1 ]] && continue
  if [[ ${#WANT[@]} -gt 0 ]]; then
    keep=0
    for w in "${WANT[@]}"; do [[ $w == "$c" ]] && keep=1; done
    [[ $keep == 0 ]] && continue
  fi
  SELECTED+=("$c")
done
[[ ${#SELECTED[@]} -eq 0 ]] && { red "golden-audit: no cases selected"; exit 2; }

# SOURCE normalize() rather than restating or extracting it. It is the gate's own
# definition of what is volatile and what is a declared gap, and a second copy here
# would drift from it exactly when it matters -- when a gap closes and its line must
# stop being dropped. This used to lift the function out of build.sh with a `sed`
# keyed on one anchored line; the shared file makes that a plain import.
. "$ROOT/tools/lib/normalize.sh"
# Keep the check anyway: a source that fails under `set -e` stops the script, but a
# file that exists and defines something else does not, and unnormalized output makes
# every case differ on nps, time, the banner and the CPU list -- which reads as
# "mcfish diverged everywhere" rather than as a broken rig.
declare -F normalize > /dev/null || {
  red "golden-audit: tools/lib/normalize.sh defined no normalize() -- rig fault, not a divergence"
  exit 2
}

ORACLE=$("$ROOT/tools/upstream/upstream_oracle.sh" ${ORACLE_SHA:+"$ORACLE_SHA"}) || {
  red "golden-audit: oracle build failed"
  exit 2
}
# Absolutize before anything cds. upstream_oracle.sh prints a path relative to the
# repo root, and every case below runs the oracle from resources/ -- where that
# relative path resolves to nothing. The failure is a shell "No such file", captured
# as the case's output, so it reads as all eight goldens diverging at once rather
# than as a rig that never launched the engine.
ORACLE=$(cd "$(dirname "$ORACLE")" && printf '%s/%s' "$PWD" "$(basename "$ORACLE")")
[[ -x $ORACLE ]] || { red "golden-audit: no oracle binary at $ORACLE"; exit 2; }

# Feed the script line by line, settling after each `go`. See the header.
drive_oracle() {
  {
    while IFS= read -r line; do
      printf '%s\n' "$line"
      case "$line" in go*) sleep "$GO_SETTLE" ;; esac
    done < "$1"
    sleep 1
  } | ( cd "$RESOURCES" && "$ORACLE" ) 2>&1
}

info "golden-audit: adjudicating ${#SELECTED[@]} golden(s) against $ORACLE"
info "golden-audit: cwd = $RESOURCES (the net must resolve from here)"

# mcfish is not named Stockfish and normalize() rewrites the BANNER but not `id name`,
# so that one line cannot be compared. Substituting it is the whole exception, and it
# must stay one sed on one anchored line: widen it and the handshake gate stops
# comparing the option table, which is the only thing it exists to compare.
ID_NAME=$(printf 'uci\nquit\n' | ( cd "$RESOURCES" && "$ROOT/build/mcfish" ) 2>/dev/null \
  | grep '^id name ' || true)

agree=0
differ=0
missing=0
wrote=0
FAILED=()
for name in "${SELECTED[@]}"; do
  golden="tools/${name}.golden"
  printf '  %-12s ' "$name"
  # A case with no golden yet is MISSING when auditing and a NEW GOLDEN when
  # writing. Adding a case is the moment its golden should come from upstream
  # rather than from mcfish, so --write has to be able to create one -- otherwise
  # the only way to seed a new case is golden-update, which drives this engine and
  # produces exactly the self-photograph this tool exists to prevent.
  if [[ ! -f $golden && $WRITE != 1 ]]; then
    red "MISSING ($golden)"
    missing=$((missing + 1))
    continue
  fi

  # Record the exit status exactly as do_golden does: a critical error makes both
  # engines terminate non-zero on purpose, so the status is contract, not noise.
  actual=$({ drive_oracle "$ROOT/tools/cases/${name}.uci"; printf 'exit=%d\n' "$?"; } | normalize)
  if [[ $name == handshake && -n $ID_NAME ]]; then
    actual=$(printf '%s\n' "$actual" | sed "s|^id name Stockfish .*|$ID_NAME|")
  fi

  if [[ -f $golden ]] && diff -u "$golden" <(printf '%s\n' "$actual") > /dev/null; then
    green "AGREES"
    agree=$((agree + 1))
    continue
  fi

  if [[ $WRITE == 1 ]]; then
    # Refuse to write output the oracle plainly did not produce. A rig fault -- a
    # relative oracle path after the cd, a missing net, a worktree mid-checkout --
    # surfaces as the shell's own error captured as the case's output, and writing
    # THAT would replace a golden with two lines of noise while reporting success.
    # Every case's normalized output opens with the banner line; nothing else does.
    if [[ $(printf '%s\n' "$actual" | head -1) != "<engine banner>" ]]; then
      red "REFUSED (the oracle produced no banner -- rig fault, not a divergence)"
      printf '%s\n' "$actual" | head -4 | sed 's/^/      | /'
      differ=$((differ + 1))
      FAILED+=("$name")
      continue
    fi
    printf '%s\n' "$actual" > "$golden"
    green "WROTE"
    wrote=$((wrote + 1))
    continue
  fi

  red "DIFFERS"
  diff -u "$golden" <(printf '%s\n' "$actual") | head -12 | sed 's/^/      | /'
  differ=$((differ + 1))
  FAILED+=("$name")
done

echo ""
if [[ $WRITE == 1 ]]; then
  info "golden-audit: $agree already agreed, $wrote re-derived from the oracle, $missing missing"
  [[ $wrote -gt 0 ]] && red "Re-derived goldens are upstream's bytes, not mcfish's -- now run \`./build.sh golden\`
  and say in the commit body what mcfish behaviour had to change to match."
else
  info "golden-audit: $agree agree, $differ differ, $missing missing"
fi
if [[ $differ -gt 0 || $missing -gt 0 ]]; then
  red "golden-audit: NOT adjudicated: ${FAILED[*]-}"
  red "  Do NOT run golden-update on these. It drives mcfish, so it would pin the"
  red "  divergence rather than resolve it. Find out why upstream disagrees, then"
  red "  re-derive from the ORACLE -- tools/GOLDEN_PROVENANCE.md has the command."
  exit 1
fi
green "golden-audit: every golden matches what upstream itself produces"
