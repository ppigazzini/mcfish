#!/usr/bin/env python3
"""Bounded, seeded fuzz of the UCI front end against the sanitized engine.

The shell's parser and session layer face arbitrary bytes on stdin; the golden
transcripts only exercise the well-formed subset. This harness drives the
ASan+UBSan engine (build/mcfish-debug) with seeded pseudo-random command
streams -- well-formed commands, boundary values, truncated and mangled lines,
binary junk -- and requires every stream to end with a clean exit and a silent
sanitizer. The seed prints first, so any crash reproduces with one flag.

The generator is deliberately weighted toward ALMOST-valid input: a parser
dies on the input that looks right until one token, not on pure noise.

Usage:
  uci_fuzz.py --seconds N [--seed S] [--binary PATH]     (run from resources/)
"""

from __future__ import annotations

import argparse
import random
import subprocess
import sys
import time

FENS = [
    "startpos",
    "fen rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1",
    "fen 8/2k5/8/8/3N4/8/2P5/2K5 b - - 0 1",
    "fen r3k2r/p1ppqpb1/bn2pnp1/3PN3/1p2P3/2N2Q1p/PPPBBPPP/R3K2R w KQkq - 0 10",
    "fen 8/8/8/8/8/8/8/8 w - - 0 1",
    "fen invalid/board/here w KQkq - 0 1",
]
OPTIONS = ["Hash", "Threads", "MultiPV", "SyzygyPath", "Ponder", "Move Overhead",
           "NoSuchOption", ""]
MOVES = ["e2e4", "e7e5", "g1f3", "e1g1", "e7e8q", "a2a1n", "0000", "zzzz", "e2e9"]


def mangle(rng: random.Random, line: str) -> str:
    roll = rng.random()
    if roll < 0.70:
        return line
    if roll < 0.80:
        return line[: rng.randrange(len(line) + 1)]
    if roll < 0.90:
        pos = rng.randrange(len(line) + 1)
        return line[:pos] + rng.choice(["\t", "  ", "\x00", "\xff", "é"]) + line[pos:]
    return "".join(chr(rng.randrange(1, 256)) for _ in range(rng.randrange(1, 80)))


def stream(rng: random.Random) -> str:
    # (text, fuzzable) pairs: the closing `stop` after a search and the final
    # `quit` stay verbatim, so every generated stream terminates the engine --
    # an unstopped infinite analysis is correct engine behaviour, not a finding.
    lines: list[tuple[str, bool]] = [("uci", True), ("isready", True)]
    for _ in range(rng.randrange(3, 25)):
        kind = rng.random()
        if kind < 0.25:
            moves = " ".join(rng.choices(MOVES, k=rng.randrange(0, 6)))
            lines.append((f"position {rng.choice(FENS)}"
                          + (f" moves {moves}" if moves else ""), True))
        elif kind < 0.45:
            lines.append((f"setoption name {rng.choice(OPTIONS)} value "
                          + rng.choice(["1", "0", "-1", "99999999", "true", "x" * 300, ""]),
                          True))
        elif kind < 0.70:
            # Emit only go forms with an intrinsic bound. The shell runs the
            # search synchronously (docs/07-shell.md), so an unbounded go --
            # `infinite`, or a zero depth/nodes/movetime, which the parser reads
            # as "no limit" -- holds the loop past any stop this stream carries.
            # Widen to those forms when the asynchronous go lands. The verbatim
            # trailing `stop` is inert today for the same reason; it stays so
            # the streams already exercise the asynchronous contract. The go
            # line itself is verbatim as well: mangling can truncate any bounded
            # form down to a bare -- unbounded -- `go`.
            lines.append((rng.choice([
                f"go depth {rng.randrange(1, 6)}",
                f"go nodes {rng.choice([1, 1000, 10**6])}",
                f"go movetime {rng.randrange(1, 30)}",
                f"go perft {rng.randrange(1, 4)}",
            ]), False))
            lines.append(("stop", False))
        elif kind < 0.85:
            lines.append((rng.choice(["ucinewgame", "isready", "stop", "ponderhit", "d",
                                      "bench 1 1 2", "eval", "flip"]), True))
        else:
            # Mangle position/setoption commands only: a truncated go line can
            # degrade to a bare `go`, which is unbounded (see above).
            lines.append((mangle(rng, rng.choice(["position startpos",
                                                  "setoption name Hash value 1"])), True))
    lines.append(("quit", False))
    return "\n".join(mangle(rng, l) if fuzz else l for l, fuzz in lines) + "\n"


def main() -> None:
    ap = argparse.ArgumentParser()
    ap.add_argument("--seconds", type=int, default=600)
    ap.add_argument("--seed", type=int, default=int(time.time()))
    ap.add_argument("--binary", default="../build/mcfish-debug")
    args = ap.parse_args()

    print(f"seed {args.seed}  (reproduce: uci_fuzz.py --seed {args.seed})", flush=True)
    rng = random.Random(args.seed)
    deadline = time.monotonic() + args.seconds
    runs = 0

    while time.monotonic() < deadline:
        payload = stream(rng)
        try:
            proc = subprocess.run([args.binary], input=payload.encode("utf-8", "surrogateescape"),
                                  capture_output=True, timeout=120)
        except subprocess.TimeoutExpired:
            # Every stream ends with a verbatim `quit`, so a timeout is a real
            # hang -- report it like any other failure.
            sys.stderr.write(f"FUZZ HANG at run {runs} (seed {args.seed})\n"
                             "---- input ----\n" + payload)
            sys.exit(1)
        out = proc.stdout.decode(errors="replace")
        err = proc.stderr.decode(errors="replace")
        # Two clean outcomes: exit 0, or the documented CRITICAL ERROR contract --
        # an unusable position command terminates the process with exit(1) after
        # announcing itself (uci.c terminate_on_critical_error, upstream uci.cpp:684).
        # The sanitizer must stay silent on BOTH paths.
        ok_exit = proc.returncode == 0 or (proc.returncode == 1 and "CRITICAL ERROR" in out)
        bad = not ok_exit or "Sanitizer" in err or "runtime error" in err
        if bad:
            sys.stderr.write(f"FUZZ FAILURE at run {runs} (seed {args.seed})\n")
            sys.stderr.write("---- input ----\n" + payload + "\n---- stderr ----\n" + err)
            sys.exit(1)
        runs += 1

    print(f"clean: {runs} streams, seed {args.seed}")


if __name__ == "__main__":
    main()
