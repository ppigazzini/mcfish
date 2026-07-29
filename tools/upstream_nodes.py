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

Refuses to run if the two engines loaded different nets (or one loaded none) --
see net_identity_or_die. That is not a search bug, and reporting node diffs as
if it were is worse than not running at all.

Usage:
    upstream_nodes.py [--positions N] [--depth D] [--seed S] [--plies P]

Env:
    ORACLE_DIR   where the pristine upstream build lives, same variable and
                 default as tools/upstream/upstream_oracle.sh (../.mcfish-
                 upstream-oracle, relative to the repo root). This script does
                 not build the oracle; run that script first, or ./build.sh
                 upstream-nodes will name the missing binary and exit.
"""

import argparse
import os
import random
import re
import subprocess
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent
MCFISH = REPO / "build" / "mcfish"

# Same variable, same default, as tools/upstream/upstream_oracle.sh: a relative
# override there resolves against the repo root, so mirror that here rather than
# against this script's own cwd, and let an absolute override pass through as-is.
_oracle_dir_env = os.environ.get("ORACLE_DIR")
ORACLE_DIR = Path(_oracle_dir_env) if _oracle_dir_env else Path("../.mcfish-upstream-oracle")
if not ORACLE_DIR.is_absolute():
    ORACLE_DIR = (REPO / ORACLE_DIR).resolve()
ORACLE = ORACLE_DIR / "src" / "stockfish"

# Both engines print this verbatim (network.c's network_verify, upstream's own
# Network::verify) before the first go/perft/eval -- it is the one line that
# names which net a running engine actually loaded.
NET_LINE_RE = re.compile(r"NNUE evaluation using (\S+\.nnue)")

# Run mcfish from resources/, not from the binary's own directory: the net lives
# there (build.sh RESOURCES_DIR), and an engine started where it cannot find one
# falls back to the classical evaluation and diverges from the oracle on every
# position -- which reads as a catastrophic search bug rather than a missing file.
MCFISH_CWD = REPO / "resources"

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
            [str(binary)],
            stdin=subprocess.PIPE,
            stdout=subprocess.PIPE,
            stderr=subprocess.DEVNULL,
            text=True,
            bufsize=1,
            cwd=str(cwd),
        )
        # Popen types the pipes Optional; both were requested two lines up.
        # Hold them as attributes of their own so every method sees them narrowed.
        assert self.p.stdin is not None and self.p.stdout is not None
        self.stdin = self.p.stdin
        self.stdout = self.p.stdout
        # Set the first time a go/perft/eval response carries the net-status
        # line; stays None if the engine never reports one (no net, or a build
        # too old to). See net_identity_or_die.
        self.net_name = None
        self._send("uci")
        self._read_until("uciok")

    def _send(self, s):
        self.stdin.write(s + "\n")
        self.stdin.flush()

    def _read_until(self, needle):
        lines = []
        while True:
            line = self.stdout.readline()
            if not line:
                return lines
            lines.append(line)
            if self.net_name is None:
                m = NET_LINE_RE.search(line)
                if m:
                    self.net_name = m.group(1)
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


def net_identity_or_die(cc, up):
    """Refuse to compare two engines that loaded different nets, or no net at all.

    A stale ORACLE_DIR built at a different SHA -- the exact case
    upstream_oracle.sh's own .built-sha check guards against when THIS script
    calls it, but not when it is skipped -- carries a different default net.
    Every position then diverges from move one, which reads as a catastrophic
    search bug and is actually a setup mistake. `cc` (mcfish, run from
    MCFISH_CWD) falling back to the classical evaluation because resources/ has
    no net produces the identical symptom: net_name stays None. Catch both
    before spending a single position on a differential that cannot mean
    anything.
    """
    cc.legal_moves(START)
    up.legal_moves(START)
    if cc.net_name is None or up.net_name is None or cc.net_name != up.net_name:
        sys.exit(
            "NET IDENTITY MISMATCH -- refusing to compare node counts.\n"
            f"  mcfish : {cc.net_name or '<none loaded -- classical fallback>'}\n"
            f"  oracle : {up.net_name or '<none loaded -- classical fallback>'}\n"
            "A divergence below this point would be the net, not the search. Check "
            "resources/ for mcfish and ORACLE_DIR/src/ for the oracle, and that the "
            "oracle was built at tools/upstream/UPSTREAM_BASE (upstream_oracle.sh "
            "refuses to reuse a binary built at a different sha; this script does "
            "not build the oracle itself, so a stale one left over from a manual "
            "run is not caught until here)."
        )


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
    cc = Engine(MCFISH, MCFISH_CWD)
    up = Engine(ORACLE, ORACLE.parent)
    cc.setup()
    up.setup()
    net_identity_or_die(cc, up)

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
            print(
                f"  shallowest divergence: depth {shallow} -- fix that one first,"
                f" it is the simplest reproducer"
            )
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
