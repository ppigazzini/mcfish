#!/usr/bin/env python3
"""Turn an upstream pin advance into a per-owner re-port worklist.

When the pin moves, the question is never "what changed upstream" -- git answers
that -- but "which mcfish files does each change land in". The correspondence
map (upstream_map.py) already knows who owns each upstream file; this asks it
once per changed file and hands back the plan, ranked so the hottest changes
land first.

Each changed upstream file produces one of three outcomes:
  change      modified between the pins -> its owners are the re-port worklist,
              ranked by churn (lines added+deleted)
  absence     added upstream -> new surface with no owner yet: port it
  divergence  deleted upstream -> its owners hold retired code: remove or rebase

The tool reads only; it never touches the tree or the pin. Run it BEFORE moving
tools/upstream/UPSTREAM_BASE, with the candidate SHA as NEW.

Usage:
  resync_worklist.py <old-sha> <new-sha>     (defaults: old = the current pin)
  resync_worklist.py <new-sha>
"""

from __future__ import annotations

import subprocess
import sys

from upstream_map import GOLDEN, build_map, pin, run


def changed(old: str, new: str) -> list[tuple[str, str, int]]:
    """(path, status, churn) for every upstream src/ file differing old..new."""
    numstat = run(["git", "diff", "--numstat", f"{old}..{new}", "--", "src/"], GOLDEN)
    status = run(["git", "diff", "--name-status", f"{old}..{new}", "--", "src/"], GOLDEN)

    churn: dict[str, int] = {}
    for line in numstat.splitlines():
        added, deleted, path = line.split("\t", 2)
        churn[path] = (0 if added == "-" else int(added)) + (0 if deleted == "-" else int(deleted))

    rows: list[tuple[str, str, int]] = []
    for line in status.splitlines():
        st, _, path = line.partition("\t")
        if not path.endswith((".cpp", ".h")):
            continue
        rows.append((path, st[:1], churn.get(path, 0)))
    return rows


def main() -> None:
    args = sys.argv[1:]
    if len(args) == 1:
        old, new = pin(), args[0]
    elif len(args) == 2:
        old, new = args
    else:
        sys.exit(__doc__)

    try:
        run(["git", "cat-file", "-e", f"{new}^{{commit}}"], GOLDEN)
    except subprocess.CalledProcessError:
        sys.exit(f"{new} is not a commit in {GOLDEN} -- fetch upstream there first")

    mapped, _, _ = build_map()
    rows = changed(old, new)
    if not rows:
        print(f"no upstream src/ changes between {old[:12]} and {new[:12]}")
        return

    print(f"upstream {old[:12]} -> {new[:12]}: {len(rows)} changed src files\n")

    def owners_of(path: str) -> str:
        owners = mapped.get(path)
        if not owners:
            return "(no owner recorded)"
        ranked = sorted(owners, key=lambda o: -owners[o])
        return ", ".join(ranked)

    for kind, title, note in (
        ("M", "CHANGE -- re-port into the owners, hottest first", ""),
        ("A", "ABSENCE -- new upstream surface, no owner yet", ""),
        ("D", "DIVERGENCE -- owners hold retired code", ""),
    ):
        block = sorted((r for r in rows if r[1] == kind), key=lambda r: -r[2])
        if not block:
            continue
        print(f"{title}:")
        for path, _, ch in block:
            print(f"  {ch:6d}  {path}  ->  {owners_of(path)}")
        print()


if __name__ == "__main__":
    main()
