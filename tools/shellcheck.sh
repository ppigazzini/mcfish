#!/usr/bin/env bash
# Lint the shell the gates are written in.
#
# WHAT THIS PROVES. Every shell script in this tree is free of the defect classes
# the lint knows, at `style` severity, with every suppression carrying a written
# reason at the site. Nothing else.
#
# (A comment line here may not BEGIN with the tool's own name followed by a word:
# a directive is exactly that shape, and this gate parsed its own prose as one on
# its first self-lint. The wording above is the fix, and this note is the record.)
#
# WHAT IT CANNOT SEE. Whether a gate checks the thing it claims to. shellcheck
# reads only syntax and idiom; `negative-control` is what proves a gate can fail, and
# the two are not substitutes. A script can be shellcheck-clean and assert nothing.
#
# WHY IT EXISTS. build.sh alone is over three thousand lines of hand-written bash
# and it decides every claim this tree makes -- and until this landed no tool had
# read a line of it. The pre-commit config lints, formats and type-checks the
# Python; the language the gates are actually written in had nothing. `do_fmt`
# already carries the comment naming the exact failure mode -- a gate reached as
# the left operand of `||` runs with `set -e` disabled for its whole body, so a
# missing `|| return 1` prints "all gates passed" over real violations -- and an
# unread gate script is that one level down.
#
# SCOPE IS EVERY SHELL FILE, and here that needs no rule. refish, where this comes
# from, scopes by authorship because its tests/ holds upstream's scripts beside its
# own; this tree is a from-scratch port whose every `.sh` is its own, so the scope
# is `git ls-files '*.sh'` and nothing has to be maintained.
#
# NO BASELINE. The other debt registers here (engine_platform.baseline,
# upstream_map.baseline) hold a surface that shrinks over time. A shellcheck
# baseline would be the only one that could not expire, in the one place where the
# findings are cheapest to fix, so the set is held at ZERO instead and a
# suppression is a `# shellcheck disable=SCxxxx` with the reason beside it.
#
# Exit codes:  0 clean   1 findings   2 rig fault   127 shellcheck unavailable
set -u
set -o pipefail

ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd) || exit 2
cd "$ROOT" || exit 2

SEVERITY=style
read -r PKG_PIN BIN_PIN < <(grep -v '^#' tools/shellcheck.version | grep -v '^$')
[[ -n ${PKG_PIN:-} && -n ${BIN_PIN:-} ]] || {
  printf '\033[31mtools/shellcheck.version does not carry both fields -- rig fault\033[0m\n'
  exit 2
}

# PINNED, AND THE PIN IS ASSERTED RATHER THAN HOPED FOR. A lint's finding set is
# version-dependent: 0.9.0 reports a trap-invoked cleanup as SC2317 and 0.11.0
# reports it as SC2329, so a suppression written against one version is not a
# suppression under the other. refish records running this unpinned and getting 0
# findings on a developer box and 65 on a runner from the same tree -- "the gate
# went red and nobody changed a script", which is the most expensive false
# positive a lint can produce.
#
# Resolved through uvx from shellcheck-py, which is how this tree already gets
# ruff and ty: pinned by version, cached after the first fetch, and no package
# manager state to install. A shellcheck already on PATH is used only if its
# version matches the pin exactly.
resolve() {
  if command -v shellcheck >/dev/null 2>&1 \
     && [[ $(shellcheck --version | awk '/^version:/{print $2}') == "$BIN_PIN" ]]; then
    SC=(shellcheck)
    return 0
  fi
  command -v uvx >/dev/null 2>&1 || return 1
  SC=(uvx --quiet --from "shellcheck-py==$PKG_PIN" shellcheck)
  "${SC[@]}" --version >/dev/null 2>&1 || return 1
}

if ! resolve; then
  printf '\033[31mshellcheck %s not resolvable (no matching binary, no uvx) --\033[0m\n' "$BIN_PIN"
  printf '\033[31mthe shell lint did NOT run. This is a SKIPPED gate, not a passing one.\033[0m\n'
  exit 127
fi

have=$("${SC[@]}" --version | awk '/^version:/{print $2}')
if [[ $have != "$BIN_PIN" ]]; then
  printf '\033[31mshellcheck %s resolved, tools/shellcheck.version pins %s -- refusing\033[0m\n' \
    "$have" "$BIN_PIN"
  printf '\033[31mto report a finding set the pin does not describe.\033[0m\n'
  exit 2
fi

mapfile -t FILES < <(git ls-files '*.sh')
if [[ ${#FILES[@]} -eq 0 ]]; then
  printf '\033[31mshellcheck: matched no .sh files -- the file list went stale\033[0m\n'
  exit 2
fi

printf '\033[36m==>\033[0m shellcheck %s, severity %s, %d files\n' "$BIN_PIN" "$SEVERITY" "${#FILES[@]}"

out=$("${SC[@]}" --severity="$SEVERITY" --format=gcc "${FILES[@]}" 2>&1)
rc=$?

if [[ -n $out ]]; then
  printf '%s\n' "$out"
  n=$(printf '%s\n' "$out" | grep -c ':' || true)
  printf '\033[31mshellcheck: %s finding(s). Fix them, or suppress AT THE SITE with a reason.\033[0m\n' "$n"
  exit 1
fi
[[ $rc -eq 0 ]] || { printf '\033[31mshellcheck: exited %d with no output -- rig fault\033[0m\n' "$rc"; exit 2; }

printf '\033[32mshell clean (shellcheck %s, %d files, severity %s)\033[0m\n' \
  "$BIN_PIN" "${#FILES[@]}" "$SEVERITY"
