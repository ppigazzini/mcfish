#!/usr/bin/env python3
"""Compare mcfish against a pristine upstream build node-for-node.

WHY RANDOM POSITIONS
--------------------
The bench signature is one number over a FIXED position set. A port can be nudged
toward that number without becoming faithful -- tune a constant until the total
lands, or special-case whatever the bench happens to exercise -- and the number
then says nothing about the engine.

This removes that possibility. It reaches positions by playing random legal moves
from the start, so they appear in no bench list, no golden and no test, then
drives BOTH engines over them with identical commands and compares node counts
per depth. Matching upstream on positions nobody tuned against is evidence of a
faithful search. Matching only on the bench set is evidence of the opposite.

An exact match here is the real claim; the bench total alone is not.

Usage:
    upstream_nodes.py [--positions N] [--depth D] [--seed S] [--plies P]
"""

import argparse
import random
import re
import subprocess
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent
MCFISH = REPO / "build" / "mcfish"
ORACLE = REPO.parent / ".mcfish-upstream-oracle" / "src" / "stockfish"

START = "rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1"


class Engine:
    """Drive a UCI engine.

    Upstream's `go` is ASYNCHRONOUS: it returns to the input loop and searches on
    another thread, so sending the next command without waiting for `bestmove`
    aborts the search and yields a zero-node result that reads as a catastrophic
    divergence. Always read to `bestmove`.
    """

    def __init__(self, binary, cwd):
        self.p = subprocess.Popen(
            [str(binary)], stdin=subprocess.PIPE, stdout=subprocess.PIPE,
            stderr=subprocess.DEVNULL, text=True, bufsize=1, cwd=str(cwd))
        self._send("uci")
        self._read_until("uciok")

    def _send(self, s):
        self.p.stdin.write(s + "\n")
        self.p.stdin.flush()

    def _read_until(self, needle):
        lines = []
        while True:
            line = self.p.stdout.readline()
            if not line:
                return lines
            lines.append(line)
            if needle in line:
                return lines

    def setup(self, hash_mb=16):
        self._send(f"setoption name Hash value {hash_mb}")
        self._send("setoption name Threads value 1")
        self._send("ucinewgame")
        self._send("isready")
        self._read_until("readyok")

    def legal_moves(self, fen):
        """Read the legal move list off `go perft 1`, which both engines print."""
        self._send(f"position fen {fen}")
        self._send("go perft 1")
        moves = []
        for line in self._read_until("Nodes searched"):
            m = re.match(r"^([a-h][1-8][a-h][1-8][qrbn]?)\s*:\s*\d+", line.strip())
            if m:
                moves.append(m.group(1))
        return moves

    def nodes_by_depth(self, fen, depth):
        """Return {depth: nodes}, so a divergence is localisable to one iteration.

        ISOLATE EACH POSITION. Without the ucinewgame the transposition table and
        the history block carry over from whatever was searched before, so the two
        engines are compared from different starting states and report divergences
        that vanish the moment either position is run alone. Every "divergence"
        this tool found before the reset was that artifact.
        """
        self._send("ucinewgame")
        self._send("isready")
        self._read_until("readyok")
        self._send(f"position fen {fen}")
        self._send(f"go depth {depth}")
        out = {}
        for line in self._read_until("bestmove"):
            d = re.search(r"^info depth (\d+)\b", line)
            n = re.search(r"\bnodes (\d+)", line)
            if d and n:
                out[int(d.group(1))] = int(n.group(1))
        return out

    def fen_after(self, moves):
        self._send("position startpos" + (" moves " + " ".join(moves) if moves else ""))
        self._send("d")
        for line in self._read_until("Key:"):
            if line.startswith("Fen:"):
                return line.split("Fen:", 1)[1].strip()
        return None

    def quit(self):
        try:
            self._send("quit")
            self.p.wait(timeout=10)
        except Exception:
            self.p.kill()


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--positions", type=int, default=20)
    ap.add_argument("--depth", type=int, default=10)
    ap.add_argument("--seed", type=int, default=20260719)
    ap.add_argument("--plies", type=int, default=12)
    args = ap.parse_args()

    if not MCFISH.exists():
        sys.exit(f"no mcfish binary at {MCFISH} -- run ./build.sh first")
    if not ORACLE.exists():
        sys.exit(f"no oracle at {ORACLE} -- run tools/upstream/upstream_oracle.sh")

    rng = random.Random(args.seed)
    cc = Engine(MCFISH, MCFISH.parent)
    up = Engine(ORACLE, ORACLE.parent)
    cc.setup()
    up.setup()

    # Generate positions with the ORACLE, so the sample cannot be biased by
    # anything mcfish does. A position mcfish cannot reach is a bug in mcfish.
    fens = []
    while len(fens) < args.positions:
        moves = []
        for _ in range(args.plies):
            legal = up.legal_moves(up.fen_after(moves) or START)
            if not legal:
                break
            moves.append(rng.choice(legal))
        fen = up.fen_after(moves)
        if fen and up.legal_moves(fen):
            fens.append(fen)

    exact = 0
    diverged = []
    for fen in fens:
        a = cc.nodes_by_depth(fen, args.depth)
        b = up.nodes_by_depth(fen, args.depth)
        # Compare the depth SETS, not their intersection. Intersecting lets a run
        # that stopped early match on its short prefix and print EXACT, while the
        # summary line -- the number anyone quotes -- still claims N/N identical.
        shared = sorted(set(a) & set(b))
        bad = [d for d in shared if a[d] != b[d]]
        if a.keys() != b.keys():
            bad = bad or [min(set(a) ^ set(b))]
        if not bad and shared and args.depth in a:
            exact += 1
            print(f"  EXACT   d{args.depth} {a.get(args.depth, '?'):>9}  {fen[:44]}")
        else:
            d0 = bad[0] if bad else None
            diverged.append((fen, d0, a.get(d0), b.get(d0)))
            print(f"  DIFF    depth {d0}: cc={a.get(d0)} up={b.get(d0)}  {fen}")

    cc.quit()
    up.quit()

    print(f"\n  {exact} / {len(fens)} random positions node-for-node identical to upstream")
    if diverged:
        shallow = min((d for _, d, _, _ in diverged if d), default=None)
        if shallow is not None:
            print(f"  shallowest divergence: depth {shallow} -- fix that one first,"
                  f" it is the simplest reproducer")
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
