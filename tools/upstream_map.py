#!/usr/bin/env python3
"""Derive the upstream<->mcfish file correspondence from the golden-reference
comments, and report the coverage both ways.

WHY COMMENTS. mcfish renames every symbol (Worker::search -> search_node), so a
symbol-table join has nothing to join on. What the tree does carry -- enforced by
the writing rules -- is the golden-reference convention: every ported mechanism
cites its upstream file ("Golden: Stockfish/src/search.cpp", "upstream
position.cpp:1038", "(tt.cpp:55)"). Those citations are the one ontology shared
across the rename boundary, so the map derives from them.

THE JOIN. For every tracked src/ and tests/ file, collect upstream citations:
  - any ".cpp" name           -- mcfish has no .cpp, so every one is upstream's
  - "name.h:123"              -- a file:line citation is upstream by convention,
                                 even when a mcfish header shares the basename
  - "name.h" with context     -- counted when the line also says upstream/golden/
                                 Stockfish AND no mcfish file shares the basename;
                                 a shared basename without :line stays ambiguous
                                 and is skipped rather than guessed
Basenames resolve to full paths in the pinned upstream tree (unique there today;
a duplicate basename would be reported, not guessed at). Upstream files that are
not-applicable by design carry a reason in tools/upstream_map.exceptions and
count as covered. Three outputs:
  - the map        upstream path -> owning mcfish files, weighted by citations
  - uncovered      upstream files no mcfish file cites: unported or unannotated
  - phantoms       citations naming files absent at the pin: drift or typos

A DECLARED blast-radius map lives beside this tool (tools/upstream_map.tsv):
upstream file -> the owners a change there lands in. --audit cross-checks it
against the derived reality: a derived owner missing from the declaration is
DRIFT (the declared radius under-estimates the blast), and a declared owner
with no citation or no file is ROT. Absorb drift by widening the row; never
trim a row to quiet the audit. --baseline ratchets the uncovered count against
tools/upstream_map.baseline -- lower it as citations land, never raise it.

The pinned upstream tree is read from this repo's own git objects when the pin
is present here (an `upstream` remote fetch), else from the sibling golden
checkout.

Usage:
  upstream_map.py            print the full map as TSV
  upstream_map.py --check    print only the coverage summary and both gap lists
  upstream_map.py --audit    check tools/upstream_map.tsv against the derived map
  upstream_map.py --baseline ratchet the uncovered count and fail on phantoms
"""

from __future__ import annotations

import argparse
import pathlib
import re
import subprocess
import sys

REPO = pathlib.Path(__file__).resolve().parent.parent
GOLDEN = REPO.parent / "Stockfish"
PIN_FILE = REPO / "tools" / "upstream" / "UPSTREAM_BASE"

CPP_REF = re.compile(r"\b([A-Za-z_][A-Za-z0-9_]*\.cpp)(?::\d+)?\b")
H_LINE_REF = re.compile(r"\b([A-Za-z_][A-Za-z0-9_]*\.h):\d+\b")
H_CTX_REF = re.compile(r"\b([A-Za-z_][A-Za-z0-9_]*\.h)\b")
CONTEXT = re.compile(r"upstream|golden|stockfish", re.I)


def run(cmd: list[str], cwd: pathlib.Path) -> str:
    return subprocess.run(cmd, cwd=cwd, check=True, capture_output=True, text=True).stdout


def pin() -> str:
    return PIN_FILE.read_text().strip()


def upstream_repo(sha: str) -> pathlib.Path:
    """Prefer this repo's own git objects for the pin; fall back to the sibling."""
    for repo in (REPO, GOLDEN):
        try:
            run(["git", "cat-file", "-e", f"{sha}^{{commit}}"], repo)
            return repo
        except subprocess.CalledProcessError, FileNotFoundError:
            continue
    sys.exit(f"pin {sha} found neither in this repo's objects nor in {GOLDEN}")


def upstream_files(sha: str) -> dict[str, str]:
    """basename -> full path at the pinned tree; a duplicate basename aborts."""
    out = run(["git", "ls-tree", "-r", "--name-only", sha, "--", "src/"], upstream_repo(sha))
    table: dict[str, str] = {}
    for path in out.splitlines():
        if not path.endswith((".cpp", ".h")):
            continue
        base = path.rsplit("/", 1)[-1]
        if base in table:
            sys.exit(f"duplicate upstream basename {base}: {table[base]} and {path}")
        table[base] = path
    return table


def mcfish_citations() -> dict[str, dict[str, int]]:
    """basename cited -> {mcfish file -> citation count}."""
    tracked = run(["git", "ls-files", "src", "tests"], REPO).splitlines()
    own_bases = {rel.rsplit("/", 1)[-1] for rel in tracked}
    cites: dict[str, dict[str, int]] = {}

    def add(base: str, owner: str) -> None:
        cites.setdefault(base, {})
        cites[base][owner] = cites[base].get(owner, 0) + 1

    for rel in tracked:
        if not rel.endswith((".c", ".h")):
            continue
        for line in (REPO / rel).read_text(errors="replace").splitlines():
            for m in CPP_REF.finditer(line):
                add(m.group(1), rel)
            line_cited = set()
            for m in H_LINE_REF.finditer(line):
                add(m.group(1), rel)
                line_cited.add(m.group(1))
            if CONTEXT.search(line):
                for m in H_CTX_REF.finditer(line):
                    if m.group(1) not in own_bases and m.group(1) not in line_cited:
                        add(m.group(1), rel)
    return cites


def exceptions() -> dict[str, str]:
    """upstream path -> reason, from tools/upstream_map.exceptions."""
    path = REPO / "tools" / "upstream_map.exceptions"
    table: dict[str, str] = {}
    if not path.exists():
        return table
    for line in path.read_text().splitlines():
        line = line.strip()
        if not line or line.startswith("#"):
            continue
        name, _, reason = line.partition("\t")
        table[name.strip()] = reason.strip()
    return table


def build_map() -> tuple[dict[str, dict[str, int]], list[str], dict[str, dict[str, int]]]:
    """Return (map by upstream path, uncovered upstream paths, phantom citations)."""
    table = upstream_files(pin())
    cites = mcfish_citations()

    mapped: dict[str, dict[str, int]] = {}
    phantoms: dict[str, dict[str, int]] = {}
    for base, owners in cites.items():
        if base in table:
            mapped[table[base]] = owners
        else:
            phantoms[base] = owners

    excused = exceptions()
    uncovered = sorted(p for p in table.values() if p not in mapped and p not in excused)
    return mapped, uncovered, phantoms


def declared() -> dict[str, list[str]]:
    """upstream path -> declared owners, from tools/upstream_map.tsv."""
    table: dict[str, list[str]] = {}
    for line in (REPO / "tools" / "upstream_map.tsv").read_text().splitlines():
        line = line.strip()
        if not line or line.startswith("#"):
            continue
        path, _, owners = line.partition("\t")
        table[path.strip()] = [o for o in owners.strip().split(",") if o]
    return table


def audit(mapped: dict[str, dict[str, int]]) -> int:
    """ROT: a declared upstream path or owner that no longer exists.
    DRIFT: a derived owner the declaration does not carry."""
    decl = declared()
    at_pin = set(upstream_files(pin()).values())
    tracked = set(run(["git", "ls-files"], REPO).splitlines())
    fails = 0

    for path, owners in decl.items():
        if path not in at_pin:
            print(f"ROT: declared upstream file absent at the pin: {path}")
            fails += 1
        for o in owners:
            if o not in tracked:
                print(f"ROT: declared owner is not a tracked file: {path} -> {o}")
                fails += 1

    for path, owners in mapped.items():
        row = set(decl.get(path, []))
        if not row:
            print(f"DRIFT: cited upstream file has no declared row: {path}")
            fails += 1
            continue
        for o in owners:
            if o not in row:
                print(f"DRIFT: {path} blast radius grew: absorb {o}")
                fails += 1

    print(
        f"audit: {'FAIL' if fails else 'OK'} ({fails} finding(s), "
        f"{len(decl)} declared rows, {len(mapped)} derived)"
    )
    return 1 if fails else 0


def ratchet(uncovered: list[str], phantoms: dict[str, dict[str, int]]) -> int:
    base_file = REPO / "tools" / "upstream_map.baseline"
    base = int(base_file.read_text().strip()) if base_file.exists() else 0
    fails = 0
    if phantoms:
        for name, owners in sorted(phantoms.items()):
            print(f"phantom citation: {name} in {', '.join(sorted(owners))}")
        fails += 1
    if len(uncovered) > base:
        print(f"uncovered surface grew: {len(uncovered)} > baseline {base}")
        for p in uncovered:
            print(f"  {p}")
        fails += 1
    else:
        print(
            f"ratchet: uncovered {len(uncovered)} <= baseline {base}"
            + (" -- lower the baseline" if len(uncovered) < base else "")
        )
    return 1 if fails else 0


def main() -> None:
    ap = argparse.ArgumentParser()
    ap.add_argument("--check", action="store_true", help="coverage summary only")
    ap.add_argument("--audit", action="store_true", help="declared-vs-derived audit")
    ap.add_argument("--baseline", action="store_true", help="ratchet uncovered count")
    args = ap.parse_args()

    mapped, uncovered, phantoms = build_map()
    total = len(mapped) + len(uncovered)

    if args.audit or args.baseline:
        rc = 0
        if args.audit:
            rc |= audit(mapped)
        if args.baseline:
            rc |= ratchet(uncovered, phantoms)
        sys.exit(rc)

    if not args.check:
        for path in sorted(mapped):
            owners = mapped[path]
            ranked = sorted(owners, key=lambda o: -owners[o])
            cells = ",".join(f"{o}({owners[o]})" for o in ranked)
            print(f"{path}\t{cells}")
        print()

    print(f"coverage: {len(mapped)}/{total} upstream files cited by src/ or tests/")
    if uncovered:
        print("uncovered (unported, or ported without a golden citation):")
        for p in uncovered:
            print(f"  {p}")
    if phantoms:
        print("phantom citations (no such file at the pin -- drift or typo):")
        for base, owners in sorted(phantoms.items()):
            print(f"  {base}  cited by {', '.join(sorted(owners))}")


if __name__ == "__main__":
    main()
