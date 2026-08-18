#!/usr/bin/env python3
"""Decompose two callgrind profiles into components and compare their SELF cost.

WHAT THIS ANSWERS, AND WHAT THE NEIGHBOURS ANSWER. `perf_counters.sh` says
WHETHER the machine executed the program differently. `perf_fingerprint.py
compare --calls` says whether we run upstream's ALGORITHM, by call count, which is
immune to inlining at the callee. This says WHERE the cost is: per component,
instructions with the locality and branch columns beside them.

EVERY NUMBER HERE IS DETERMINISTIC, and that is what pays for callgrind's
order-of-magnitude slowdown. Two runs of one binary give identical counts, so a
component difference of any size is real rather than thermal. It is also why the
depth stays small.

AND EVERY NUMBER HERE IS A MODEL. The cache simulator is a fixed two-level
geometry: not this machine's cache, no prefetcher, nothing about out-of-order
execution. It RANKS locality; it does not predict time. Where this and
`perf_counters.sh` disagree, that one is measuring the hardware and this one a
model of it -- and a sim has already overstated an I-cache effect in this repo by
roughly 2x against direct measurement.

THE FILE IS PARSED DIRECTLY, not through `callgrind_annotate`. annotate shows one
event at a time by default and this needs three, and it wraps a long symbol across
output lines so its columns cannot be parsed for one. The callgrind format is
defined and stable.

SELF COST, NOT INCLUSIVE. The line following a `calls=` line carries the callee's
inclusive cost and is skipped -- summing it would count the whole NNUE evaluation
inside the search and again inside itself.

Exit codes:  0 reported   1 a component matched on one side only
             2 a profile could not be read or could not be grouped
"""

import argparse
import contextlib
import re
import sys
from pathlib import Path

EVENTS = [
    "Ir",
    "Dr",
    "Dw",
    "I1mr",
    "D1mr",
    "D1mw",
    "ILmr",
    "DLmr",
    "DLmw",
    "Bc",
    "Bcm",
    "Bi",
    "Bim",
]

# Symbols valgrind could not name: a raw address or an unresolved name-compression
# id. A profile where a large share of ONE side's cost carries no symbol cannot be
# decomposed at all -- every component then matches on the named side only, and a
# real cost divided by nothing reads as a total win.
UNNAMED = re.compile(r"^(0x[0-9a-fA-F]+|\?\d+|\?+)$")
UNNAMED_LIMIT = 5.0

# Below this the difference is code layout rather than work. Naming a winner inside
# it invites a refactor to be argued from the rounding.
TIE = 0.0005


def parse_callgrind(path):
    """Return {symbol: {event: total_self_cost}}."""
    text = Path(path).read_text(encoding="utf8", errors="replace")
    names, totals = {}, {}
    current = order = None
    skip_next_cost = False

    for line in text.splitlines():
        if line.startswith("events:"):
            order = line.split(":", 1)[1].split()
            continue
        # Name compression defines an id on its FIRST appearance, and that may be a
        # cfn= (called function) line rather than an fn=. Recording only fn= leaves
        # every such symbol as an unresolved id.
        if line.startswith(("fn=", "cfn=")):
            is_self = line.startswith("fn=")
            body = line.split("=", 1)[1]
            m = re.match(r"\((\d+)\)\s*(.*)$", body)
            if m:
                fid, name = m.group(1), m.group(2)
                if name:
                    names[fid] = name
                resolved = names.get(fid, f"?{fid}")
            else:
                resolved = body
            if is_self:
                current = resolved
                skip_next_cost = False
            continue
        if line.startswith("calls="):
            skip_next_cost = True  # the next cost line is the callee's, not ours
            continue
        if line[:1].isdigit() or line[:1] in "+-*":
            if skip_next_cost:
                skip_next_cost = False
                continue
            if current is None or order is None:
                continue
            costs = totals.setdefault(current, {})
            for i, tok in enumerate(line.split()[1:]):
                if i >= len(order):
                    break
                with contextlib.suppress(ValueError):
                    costs[order[i]] = costs.get(order[i], 0) + int(tok)
            continue
        skip_next_cost = False

    return totals


def load_components(path):
    rows = []
    for line in Path(path).read_text(encoding="utf8").splitlines():
        line = line.strip()
        if not line or line.startswith("#"):
            continue
        parts = line.split("\t")
        if len(parts) >= 2:
            rows.append((parts[0].strip(), re.compile(parts[1].strip())))
    return rows


def group(totals, components):
    """Sum each component's events. Rows are tried in order; the first match wins."""
    out = {name: dict.fromkeys(EVENTS, 0) for name, _ in components}
    for symbol, costs in totals.items():
        for name, rx in components:
            if rx.search(symbol):
                for ev in EVENTS:
                    out[name][ev] += costs.get(ev, 0)
                break
    return out


def fmt_m(v):
    return f"{v / 1e6:,.1f}" if v else "-"


def ratio(head, base):
    return head / base if base else None


def rf(r):
    return f"{r:.4f}" if r is not None else "-"


def unnamed_share(profile):
    total = sum(c.get("Ir", 0) for c in profile.values())
    if not total:
        return 0.0
    return 100.0 * sum(c.get("Ir", 0) for s, c in profile.items() if UNNAMED.match(s)) / total


def biggest(profile, predicate, n=4):
    rows = [(c.get("Ir", 0), s) for s, c in profile.items() if predicate(s) and c.get("Ir", 0)]
    rows.sort(reverse=True)
    return rows[:n]


def main():
    ap = argparse.ArgumentParser(add_help=True)
    ap.add_argument("base", help="callgrind output for the reference side")
    ap.add_argument("head", help="callgrind output for the side under test")
    ap.add_argument("components", help="TSV of NAME <TAB> REGEX")
    ap.add_argument("--base-label", default="base")
    ap.add_argument("--head-label", default="head")
    args = ap.parse_args()

    try:
        base = parse_callgrind(args.base)
        head = parse_callgrind(args.head)
    except OSError as exc:
        print(f"perf-decomp: cannot read a profile: {exc}", file=sys.stderr)
        return 2

    components = load_components(args.components)
    if not components:
        print(f"perf-decomp: no components in {args.components}", file=sys.stderr)
        return 2

    tot_b = sum(c.get("Ir", 0) for c in base.values())
    tot_h = sum(c.get("Ir", 0) for c in head.values())

    # REFUSE BEFORE PRINTING ANYTHING. A table is not a safe way to report either
    # failure below: the reader sees plausible per-row numbers over a hole.
    if not tot_b or not tot_h:
        print(
            f"perf-decomp: VOID -- a profile carries no cost at all ({args.base_label} {tot_b} Ir,"
            f" {args.head_label} {tot_h} Ir). A truncated or empty callgrind file reads as a"
            f" total win; it is a dead run, not a result.",
            file=sys.stderr,
        )
        return 2

    un_b, un_h = unnamed_share(base), unnamed_share(head)
    if max(un_b, un_h) > UNNAMED_LIMIT:
        print(
            f"perf-decomp: VOID -- {un_b:.1f}% of {args.base_label} and {un_h:.1f}% of"
            f" {args.head_label} instructions carry no symbol (limit {UNNAMED_LIMIT:.0f}%).",
            file=sys.stderr,
        )
        print(
            "perf-decomp: cost that carries no symbol cannot be attributed to any component,"
            " so the grouping below it would be arithmetic on a hole. When only one side is"
            " affected, every component matches on the other side alone.",
            file=sys.stderr,
        )
        for label, prof in ((args.base_label, base), (args.head_label, head)):
            rows = biggest(prof, lambda s: bool(UNNAMED.match(s)))
            if rows:
                print(f"  largest unnamed symbols ({label}):", file=sys.stderr)
                for ir, sym in rows:
                    print(f"    {fmt_m(ir):>10}  {sym}", file=sys.stderr)
        return 2

    gb, gh = group(base, components), group(head, components)
    bl, hl = args.base_label[:8], args.head_label[:8]

    print(
        f"  {'component':<24} {bl + ' Ir':>11} {hl + ' Ir':>11} {'Ir':>7} "
        f"{'D1mr':>7} {'Bcm':>7}  winner"
    )
    print(f"  {'-' * 24} {'-' * 11} {'-' * 11} {'-' * 7} {'-' * 7} {'-' * 7}  {'-' * 10}")

    missing, one_sided = [], []
    for name, _ in components:
        b, h = gb[name], gh[name]
        if b["Ir"] == 0 and h["Ir"] == 0:
            missing.append(name)
            continue
        if (b["Ir"] == 0) != (h["Ir"] == 0):
            one_sided.append(name)

        r_ir = ratio(h["Ir"], b["Ir"])
        if name in one_sided:
            # A real cost divided by nothing. Name it as an artifact rather than
            # ranking it, and keep every other row: asymmetric inlining is the
            # expected outcome of the very refactors this axis exists to measure.
            winner = "X one side"
        elif r_ir is None:
            winner = "-"
        elif r_ir < 1 - TIE:
            winner = args.head_label
        elif r_ir > 1 + TIE:
            winner = args.base_label
        else:
            winner = "tie"

        print(
            f"  {name:<24} {fmt_m(b['Ir']):>11} {fmt_m(h['Ir']):>11} {rf(r_ir):>7} "
            f"{rf(ratio(h['D1mr'], b['D1mr'])):>7} {rf(ratio(h['Bcm'], b['Bcm'])):>7}  {winner}"
        )

    cb = sum(gb[n]["Ir"] for n, _ in components)
    ch = sum(gh[n]["Ir"] for n, _ in components)
    print(f"  {'-' * 24}")
    print(f"  {'grouped':<24} {fmt_m(cb):>11} {fmt_m(ch):>11} {rf(ratio(ch, cb)):>7}")
    r_whole = rf(ratio(tot_h, tot_b))
    print(f"  {'whole program':<24} {fmt_m(tot_b):>11} {fmt_m(tot_h):>11} {r_whole:>7}")
    print(
        f"  coverage: {100 * cb / tot_b:.1f}% of {args.base_label} Ir,"
        f" {100 * ch / tot_h:.1f}% of {args.head_label} Ir is grouped"
    )

    # A component matching nothing on BOTH sides is reported BY NAME, never printed
    # as a zero: a zero row reads as a total win forever, and the real cause is a
    # symbol that stopped surviving inlining.
    if missing:
        print(f"\n  matched nothing on either side: {', '.join(missing)}")
        print("  NEVER a free component -- it is a row this workload did not exercise, or a")
        print("  STALE row whose symbol stopped surviving inlining. The tablebase rows are")
        print("  the first case on a bench-list run and the components file says so; anything")
        print("  else is the second, and the regex wants fixing rather than the row deleting.")

    if one_sided:
        print(
            f"\nperf-decomp: FINDINGS -- {', '.join(one_sided)} matched on one side only;"
            " those rows are marked X and excluded from the verdict. The rest stands.",
            file=sys.stderr,
        )
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
