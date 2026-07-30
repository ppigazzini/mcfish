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
import re
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
OPTIONS = [
    "MultiPV",
    "SyzygyPath",
    "Ponder",
    "Move Overhead",
    "NoSuchOption",
    "",
]
OPTION_VALUES = ["1", "0", "-1", "99999999", "true", "x" * 300, ""]

# Hash and Threads are the only two options whose value the engine turns straight
# into an allocation, so they are the only ones whose fuzzed value can exhaust the
# MACHINE rather than the process -- and that kill takes this harness with it,
# leaving a dead runner and no stream to read. Draw their values from a bounded
# pool and emit the line VERBATIM, for the same reason the go lines below stay
# whole: mangling defeats a bound. Truncation is the specific defeat, since it
# rewrites a value `accepts` refuses into one it honours (ucioption.c
# value_in_range rejects an out-of-range spin rather than clamping it, and rejects
# a trailing-junk insertion outright, so truncation is the only mangle that gets
# through) -- `Hash value 99999999` becomes `Hash value 9999`, a 9.7 GiB table
# that tt_resize maps and the load-bearing tt_clear then touches page by page, and
# `Threads value 99999999` becomes 999 worker blocks of 2.89 MB apiece.
# Bound each of the two in its own units -- megabytes of table against counts of
# worker, so one shared pool would read 16 as both. The below-min and non-numeric
# values take the same reject branch an over-max value would, allocating on no
# path. 33554432 is Hash's exact advertised max (engine_options.c max_hash_mb): in
# range, so it reaches ArenaAlloc and exercises on_hash's refusal report, yet 32
# TiB, so the mapping is refused without a page being touched; Threads refuses it
# as out of range on any real box. No value here lands in the band that is both in
# range and backable -- the band where the mapping succeeds and the clear that
# follows OOMs the box.
MEM_VALUES = {
    "Hash": ["1", "2", "16", "0", "-1", "true", "", "33554432"],
    "Threads": ["1", "2", "4", "0", "-1", "true", "", "33554432"],
}
MEM_OPTIONS = list(MEM_VALUES)
MOVES = ["e2e4", "e7e5", "g1f3", "e1g1", "e7e8q", "a2a1n", "0000", "zzzz", "e2e9"]

# A memory request the box CAN back is the dangerous one: it succeeds, the clear
# that follows touches every page, and the OOM kill takes the machine -- and this
# harness with it, so the stream that caused it is never reported. Check the
# payload rather than trust the generator, so a later edit to the pools above
# fails loudly here instead of silently killing a runner.
MEM_REQUEST = re.compile(
    r"^setoption[ \t]+name[ \t]+(Hash|Threads)[ \t]+value[ \t]+(\d+)[ \t]*$", re.M | re.I
)
SAFE_HASH_MB = 64
SAFE_THREADS = 4
# Past these the mapping is refused outright, without a page being touched: a
# reported failure, not a kill. Between them and the safe caps lies the band that
# OOMs the box.
UNBACKABLE_HASH_MB = 1 << 20
UNBACKABLE_THREADS = 100_000


def unbounded_request(payload: str) -> str | None:
    """Return the offending line if a payload asks for memory the box may back."""
    for match in MEM_REQUEST.finditer(payload):
        option = match.group(1).lower()
        size = int(match.group(2))
        if option == "hash" and not (size <= SAFE_HASH_MB or size >= UNBACKABLE_HASH_MB):
            return match.group(0)
        if option == "threads" and not (size <= SAFE_THREADS or size >= UNBACKABLE_THREADS):
            return match.group(0)
    return None


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
            lines.append(
                (f"position {rng.choice(FENS)}" + (f" moves {moves}" if moves else ""), True)
            )
        elif kind < 0.45:
            if rng.random() < 0.5:
                # Memory-shaped option: bounded value, unmangled (see MEM_VALUES).
                option = rng.choice(MEM_OPTIONS)
                lines.append(
                    (
                        f"setoption name {option} value " + rng.choice(MEM_VALUES[option]),
                        False,
                    )
                )
            else:
                lines.append(
                    (
                        f"setoption name {rng.choice(OPTIONS)} value " + rng.choice(OPTION_VALUES),
                        True,
                    )
                )
        elif kind < 0.70:
            # Emit any go form, bounded or not. The asynchronous go has landed
            # (docs/07-shell.md): the search runs off the UCI thread, so the
            # trailing `stop` interrupts an unbounded `go infinite` / bare `go`,
            # and any command that follows an un-stopped unbounded search ends it
            # (engine_end_search) rather than hanging behind it. The go line is
            # fuzzable now for the same reason -- mangling it down to a bare,
            # unbounded `go` is a case the contract must survive, not avoid.
            lines.append(
                (
                    rng.choice(
                        [
                            f"go depth {rng.randrange(1, 6)}",
                            f"go nodes {rng.choice([1, 1000, 10**6])}",
                            f"go movetime {rng.randrange(1, 30)}",
                            f"go perft {rng.randrange(1, 4)}",
                            "go infinite",
                            "go",
                        ]
                    ),
                    True,
                )
            )
            lines.append(("stop", True))
        elif kind < 0.85:
            lines.append(
                (
                    rng.choice(
                        [
                            "ucinewgame",
                            "isready",
                            "stop",
                            "ponderhit",
                            "d",
                            "bench 1 1 2",
                            "eval",
                            "flip",
                        ]
                    ),
                    True,
                )
            )
        else:
            lines.append(
                (
                    mangle(rng, rng.choice(["position startpos", "setoption name Hash value 1"])),
                    True,
                )
            )
    lines.append(("quit", False))
    return "\n".join(mangle(rng, text) if fuzz else text for text, fuzz in lines) + "\n"


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
        offender = unbounded_request(payload)
        if offender is not None:
            sys.stderr.write(
                f"HARNESS BUG at run {runs} (seed {args.seed}): generated a memory "
                f"request the box may back and then clear, which would OOM the machine "
                f"and take this harness with it: {offender!r}\n"
            )
            sys.exit(1)
        try:
            proc = subprocess.run(
                [args.binary],
                input=payload.encode("utf-8", "surrogateescape"),
                capture_output=True,
                timeout=120,
            )
        except subprocess.TimeoutExpired:
            # Every stream ends with a verbatim `quit`, so a timeout is a real
            # hang -- report it like any other failure.
            sys.stderr.write(
                f"FUZZ HANG at run {runs} (seed {args.seed})\n---- input ----\n" + payload
            )
            sys.exit(1)
        out = proc.stdout.decode(errors="replace")
        err = proc.stderr.decode(errors="replace")
        # Two clean outcomes: exit 0, or the documented CRITICAL ERROR contract --
        # an unusable position command terminates the process with exit(1) after
        # announcing itself (uci.c terminate_on_critical_error, upstream uci.cpp:684).
        # The sanitizer must stay silent on BOTH paths.
        # A refused `setoption name Hash` is neither: on_hash reports the size it
        # could not allocate through `info string` and leaves the one-cluster
        # fallback installed, so the session continues and still exits 0. Only the
        # STARTUP resize is fatal (engine.c), and the default 16 MB gets there.
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
