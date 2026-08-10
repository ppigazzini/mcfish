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

    def set_option(self, name, value):
        """Set one option and WAIT for it. `isready` is not politeness here: the
        Chess960 class changes how both engines parse the FEN that follows, so a
        `position` racing the `setoption` would be parsed under the old rule."""
        self._send(f"setoption name {name} value {value}")
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

    def fen_after_from(self, start_fen, moves):
        """fen_after, from an arbitrary start. The Chess960 class needs it: its
        positions do not descend from `startpos`, and asking upstream to replay the
        move list is what keeps the castling encoding upstream's rather than ours."""
        head = "position startpos" if start_fen == START else f"position fen {start_fen}"
        self._send(head + (" moves " + " ".join(moves) if moves else ""))
        self._send("d")
        for line in self._read_until("Key:"):
            if line.startswith("Fen:"):
                return line.split("Fen:", 1)[1].strip()
        return None

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


# --- position classes -----------------------------------------------------------
#
# The random walk below reaches exactly one kind of position: standard chess, a few
# plies from the start, with everything still on the board. That is one property, and
# an engine branches on many -- so a divergence in Chess960 castling, in a
# tablebase-range endgame, or near the 50-move boundary was unreachable by this probe
# no matter how many positions it drew. tools/fixture_properties.tsv is the list these
# classes come from.
#
# Each class returns (label, [fens], {options}). The options are applied to BOTH
# engines before the class runs and reset after it.


def _random_960_backrank(rng):
    """Draw a legal Chess960 back rank: bishops on opposite colours, king between
    the rooks. Rejection-sample rather than index into the 960 table -- the table
    would be a second implementation of the rule, and this file has no way to check
    one against upstream."""
    while True:
        rank = rng.sample("rnbqkbnr", 8)
        bishops = [i for i, c in enumerate(rank) if c == "b"]
        if (bishops[0] - bishops[1]) % 2 == 0:
            continue
        king = rank.index("k")
        rooks = [i for i, c in enumerate(rank) if c == "r"]
        if not (rooks[0] < king < rooks[1]):
            continue
        return "".join(rank)


def class_random(up, rng, count, plies):
    """Standard chess, `plies` random legal moves from the start position."""
    return _walk(up, rng, count, plies, START)


def class_chess960(up, rng, count, plies):
    """Chess960: a random legal back rank, then the same random walk. The castling
    field stays `KQkq`, which is the spelling tools/cases/chess960.uci uses and both
    engines accept in 960 mode."""
    fens = []
    while len(fens) < count:
        back = _random_960_backrank(rng)
        start = f"{back}/pppppppp/8/8/8/8/PPPPPPPP/{back.upper()} w KQkq - 0 1"
        fens += _walk(up, rng, 1, plies, start)
    return fens[:count]


def class_rule50(up, rng, count, plies):
    """The same walk, then the halfmove clock wound to just under the draw. This is
    the boundary of the rule50 class, and it is reachable no other way here: a
    12-ply walk from the start leaves the counter near zero every time."""
    out = []
    for fen in _walk(up, rng, count, plies, START):
        parts = fen.split()
        if len(parts) < 6:
            continue
        parts[4] = str(rng.choice([90, 96, 98, 99]))
        out.append(" ".join(parts))
    return out


def class_endgame(up, _rng, count, _plies):
    """Tablebase-range material, taken from the fixture that already presents it
    (tools/cases/tb.fens) rather than generated: a random walk does not trade down,
    and constructing few-man positions here would need a legality model this file
    has no way to check against upstream."""
    path = REPO / "tools" / "cases" / "tb.fens"
    if not path.exists():
        return []
    fens = []
    for line in path.read_text().splitlines():
        line = line.strip()
        if not line or line.startswith("#"):
            continue
        parts = line.split(None, 1)
        if len(parts) == 2 and up.legal_moves(parts[1]):
            fens.append(parts[1])
    return fens[:count]


CLASSES = {
    "random": (class_random, {}),
    "chess960": (class_chess960, {"UCI_Chess960": "true"}),
    "rule50": (class_rule50, {}),
    "endgame": (class_endgame, {}),
}


def _walk(up, rng, count, plies, start_fen):
    """Generate positions with the ORACLE, so the sample cannot be biased by
    anything mcfish does. A position mcfish cannot reach is a bug in mcfish."""
    fens = []
    guard = 0
    while len(fens) < count and guard < count * 20:
        guard += 1
        moves = []
        cur = start_fen
        for _ in range(plies):
            legal = up.legal_moves(cur)
            if not legal:
                break
            moves.append(rng.choice(legal))
            cur = up.fen_after_from(start_fen, moves)
            if not cur:
                break
        if cur and up.legal_moves(cur):
            fens.append(cur)
    return fens


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--positions", type=int, default=20)
    ap.add_argument("--depth", type=int, default=10)
    ap.add_argument("--seed", type=int, default=20260719)
    ap.add_argument("--plies", type=int, default=12)
    ap.add_argument(
        "--classes",
        default="random,chess960,rule50,endgame",
        help="comma-separated position classes (see CLASSES); default is all of them",
    )
    args = ap.parse_args()

    wanted = [c.strip() for c in args.classes.split(",") if c.strip()]
    unknown = [c for c in wanted if c not in CLASSES]
    if unknown:
        sys.exit(f"unknown position class(es): {', '.join(unknown)} -- have {', '.join(CLASSES)}")
    if not wanted:
        sys.exit("no position classes selected -- refusing to compare nothing")

    if not MCFISH.exists():
        sys.exit(f"no mcfish binary at {MCFISH} -- run ./build.sh first")
    if not ORACLE.exists():
        sys.exit(f"no oracle at {ORACLE} -- run tools/upstream/upstream_oracle.sh")

    # Echo the seed this run actually used. The weekly lane passes a wall-clock seed on
    # purpose -- a fixed one explores the same positions forever -- but that left a red
    # run unreproducible from its own log, and the chess960 castling crash of 2026-08-10
    # had to be chased from the single FEN that printed before the engine died. Any run
    # here replays with --seed <the number below>.
    print(
        f"  seed {args.seed}  positions {args.positions}  depth {args.depth}"
        f"  plies {args.plies}  classes {','.join(wanted)}"
    )
    rng = random.Random(args.seed)
    cc = Engine(MCFISH, MCFISH_CWD)
    up = Engine(ORACLE, ORACLE.parent)
    cc.setup()
    up.setup()
    net_identity_or_die(cc, up)

    exact = 0
    total = 0
    diverged = []
    per_class = []

    for cls in wanted:
        make, options = CLASSES[cls]
        # Both engines take the class's options, and both give them back. A class
        # that leaked UCI_Chess960 into the next one would compare two engines that
        # agree with each other and with nothing upstream does.
        for k, v in options.items():
            cc.set_option(k, v)
            up.set_option(k, v)

        fens = make(up, rng, args.positions, args.plies)
        print(f"\n  class {cls}: {len(fens)} position(s)")
        if not fens:
            print(f"  {cls} produced NO positions -- this class compared nothing")
            per_class.append((cls, 0, 0))
            for k in options:
                cc.set_option(k, "false")
                up.set_option(k, "false")
            continue

        cls_exact = 0
        for fen in fens:
            total += 1
            a = cc.nodes_by_depth(fen, args.depth)
            b = up.nodes_by_depth(fen, args.depth)
            # Compare the depth SETS, not their intersection. Intersecting lets a
            # run that stopped early match on its short prefix and print EXACT,
            # while the summary line -- the number anyone quotes -- still claims
            # N/N identical.
            shared = sorted(set(a) & set(b))
            bad = [d for d in shared if a[d] != b[d]]
            if a.keys() != b.keys():
                bad = bad or [min(set(a) ^ set(b))]
            if not bad and shared and args.depth in a:
                exact += 1
                cls_exact += 1
                print(f"  EXACT   d{args.depth} {a.get(args.depth, '?'):>9}  {fen[:44]}")
            else:
                d0 = bad[0] if bad else None
                diverged.append((fen, d0, a.get(d0), b.get(d0)))
                print(f"  DIFF    depth {d0}: cc={a.get(d0)} up={b.get(d0)}  {fen}")

        per_class.append((cls, cls_exact, len(fens)))
        for k in options:
            cc.set_option(k, "false")
            up.set_option(k, "false")

    cc.quit()
    up.quit()

    # Name every class and its count. A summary that totals across classes cannot
    # say which one was empty, and an empty class is exactly the failure this
    # widening exists to make visible -- it reads as coverage and compared nothing.
    print("")
    for cls, ok, n in per_class:
        state = "compared NOTHING" if n == 0 else f"{ok} / {n} identical"
        print(f"  {cls:<10} {state}")
    print(
        f"\n  {exact} / {total} positions node-for-node identical to upstream"
        f" across {len(per_class)} class(es)"
    )

    empty = [c for c, _, n in per_class if n == 0]
    if empty:
        print(f"  REFUSED: {', '.join(empty)} produced no positions -- a class that")
        print("  compared nothing must not be summed into a passing run")
        return 2

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
