#!/usr/bin/env python3
"""Startup-subtracted, bias-cancelled counter standing from perf_counters `#R` lines.

WHY THIS EXISTS, AND WHY A RATIO CANNOT DO IT. `perf_counters` measures the whole
process, and process startup is a large, ENGINE-DEPENDENT share of any bench short
enough to iterate on: mcfish parses the ~95 MB net in roughly half upstream's time,
so a whole-process ratio hands mcfish a lead that has nothing to do with the search.
The difference of two ratios is not the ratio of two differences, so the correction
cannot be applied to the tool's own output -- it has to be done on absolute counts,
which is what the `#R` lines exist for.

The method: run the SAME pair over a deep workload and over a depth-1 workload, take
the median absolute per side, and subtract. Startup is identical in both runs of a
given binary, so it cancels exactly, and what is left is the search.

It is not a small correction. On this repo's spine comparison it moved the
instruction ratio from 0.940 to 1.002 -- that is, from "mcfish does 6% less work"
to "the two do the same work" -- and every published standing taken before it
carried the error.

BUT IT IS ONLY SOUND ON A DETERMINISTIC AXIS. The subtraction removes a quantity
whose own measurement error can be comparable to the difference being sought: on
that same comparison the two deep runs differed by 1.5% while the startup terms
removed differed by 67%, and cycles carry a 6-16% per-round spread on this host.
The instruction row survived and the cycle row did not -- the derived cycle ratio
read 1.043 where direct measurement of the engines' own search clock read 1.004
and 1.018. So: trust this for instructions and macro-ops; for CYCLES prefer a
workload that never contained startup at all, such as bench's `Total time`, which
both engines start after the `ucinewgame` clear.

BIAS CANCELLATION. A paired ratio on this class of host carries a multiplicative
position bias of a couple of percent (the A/A control reads 1.02 rather than 1.00
when the box is unsettled). Run the pair BOTH WAYS and pass all four files; the
geometric mean sqrt(fwd/swp) cancels a multiplicative bias exactly. With two files
the forward ratio is reported as-is and carries that bias.

Usage:
  perf_counters.sh A B N bench 16 1 13 > deep_fwd.txt     # deep, forward
  perf_counters.sh A B N bench 16 1 1  > shal_fwd.txt     # startup, forward
  perf_counters.sh B A N bench 16 1 13 > deep_swp.txt     # deep, swapped
  perf_counters.sh B A N bench 16 1 1  > shal_swp.txt     # startup, swapped
  perf_delta.py deep_fwd.txt shal_fwd.txt [deep_swp.txt shal_swp.txt]

DO NOT REACH FOR THIS FOR A SPEED QUESTION. For "which engine searches faster",
use tools/nps_ab.sh: it reads each engine's own bench clock, which excludes startup
by construction, so there is nothing to subtract and nothing to get wrong. This tool
exists for the WORK axes -- instructions and macro-ops -- where the whole-process
count really does need startup removed and the axis is deterministic enough to
survive the subtraction.
"""

import re
import statistics
import sys

# Transcripts from before the stall modes existed carry five columns; name only as
# many as the file actually holds rather than indexing off the end of a short row.
ALL_AXES = ["instructions", "cycles", "cache_misses", "branch_misses", "macro_ops", "raw6", "raw7"]


def axes_for(width):
    return ALL_AXES[:width]


def nodes_of(path):
    """The tree size perf_counters.sh recorded in its header, or None.

    A RATIO WITH NO BASE CANNOT SAY WHETHER IT MATTERS. "cache misses: 1.007" reads
    like a finding until the absolute turns out to be a fraction of a miss per node
    on a base of fifty, which the out-of-order engine hides entirely. Per NODE
    rather than per run, so two transcripts taken over DIFFERENT trees -- a
    material-eval spine run against a full-engine one -- are still comparable, which
    they are not on absolutes.
    """
    with open(path) as f:
        for line in f:
            m = re.match(r"#\s*tree:\s*([\d,]+)\s+nodes", line)
            if m:
                return int(m.group(1).replace(",", ""))
    return None


def read(path):
    """Split a perf_counters transcript into per-round absolutes for each side."""
    a, b = [], []
    with open(path) as fh:
        for line in fh:
            if not line.startswith("#R "):
                continue
            _, _, side, *vals = line.split()
            (a if side == "A" else b).append([int(v) for v in vals])
    if not a or not b:
        sys.exit(f"error: {path} holds no `#R` lines -- was it produced by perf_counters?")
    return a, b


def per_node(total, n):
    """Render TOTAL per node, or a dash when the transcript carried no tree size."""
    if not n:
        return "-"
    v = total / n
    return f"{v:,.1f}" if v < 1000 else f"{v:,.0f}"


def med(rows):
    return [statistics.median(col) for col in zip(*rows, strict=True)]


def search_only(deep, shallow):
    """Median deep counts minus median startup counts, per side."""
    da, db = read(deep)
    sa, sb = read(shallow)
    a = [d - s for d, s in zip(med(da), med(sa), strict=True)]
    b = [d - s for d, s in zip(med(db), med(sb), strict=True)]
    for label, side in (("A", a), ("B", b)):
        if side[0] <= 0:
            sys.exit(
                f"error: side {label} has non-positive search instructions after "
                f"subtraction -- the deep and shallow runs are not the same pair."
            )
    return a, b


def main(deep_fwd, shal_fwd, deep_swp=None, shal_swp=None):
    A, B = search_only(deep_fwd, shal_fwd)
    axes = axes_for(len(A))
    n = nodes_of(deep_fwd)
    fwd = [(x / y if y else 0.0) for x, y in zip(A, B, strict=True)]

    if deep_swp:
        SA, SB = search_only(deep_swp, shal_swp)
        swp = [(x / y if y else 0.0) for x, y in zip(SA, SB, strict=True)]
        print(
            f"{'axis':<16}{'A (search)':>18}{'B (search)':>18}"
            f"{'fwd':>8}{'swp':>8}{'A/B':>9}{'A/node':>12}{'B/node':>12}"
        )
        for i, ax in enumerate(axes):
            bc = (fwd[i] / swp[i]) ** 0.5 if swp[i] else 0.0
            print(
                f"{ax:<16}{A[i]:>18,}{B[i]:>18,}{fwd[i]:>8.3f}{swp[i]:>8.3f}{bc:>9.3f}"
                f"{per_node(A[i], n):>12}{per_node(B[i], n):>12}"
            )
    else:
        print(
            f"{'axis':<16}{'A (search)':>18}{'B (search)':>18}"
            f"{'A/B':>9}{'A/node':>12}{'B/node':>12}"
        )
        for i, ax in enumerate(axes):
            print(
                f"{ax:<16}{A[i]:>18,}{B[i]:>18,}{fwd[i]:>9.3f}"
                f"{per_node(A[i], n):>12}{per_node(B[i], n):>12}"
            )

    if A[1] and B[1]:
        ipc = (A[0] / A[1]) / (B[0] / B[1])
        head = f"{'IPC (derived)':<16}{'':>18}{'':>18}"
        print(head + f"{ipc:>{26 if deep_swp else 9}.3f}")


if __name__ == "__main__":
    if len(sys.argv) not in (3, 5):
        sys.exit(__doc__)
    main(*sys.argv[1:])
