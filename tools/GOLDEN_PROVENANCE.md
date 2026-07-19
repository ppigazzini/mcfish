# Where each golden came from

A golden is not automatically a reference. When it is regenerated from ccfish, it
is a **photograph of ccfish** — and it will pin a defect exactly as faithfully as it
pins correct behaviour, after which the gate passes *because* the engine is wrong.

That is not hypothetical here. `board.golden` and `errors.golden` were both
generated from ccfish and both pinned real divergences for as long as they existed:

- `board.golden` recorded a `d` output with **no `Checkers:` line**, which upstream
  always prints, and recorded ccfish silently accepting the illegal move `b4b6`
  and continuing.
- `errors.golden` recorded three invalid FENs each producing an identical start
  position and **no diagnostic at all**, where upstream names a reason and exits 1.

Both gates were green throughout.

## Provenance

| golden | derived from | meaning |
| --- | --- | --- |
| `board` | **oracle** | byte-for-byte upstream |
| `chess960` | **oracle** | byte-for-byte upstream |
| `errors` | **oracle** | byte-for-byte upstream, including `exit=1` |
| `perft` | **oracle** | byte-for-byte upstream |
| `eval` | **oracle** | byte-for-byte upstream |
| `handshake` | ccfish | self-photograph — the option table is a hand-written subset |
| `search` | ccfish | self-photograph — carries the `<engine banner>` identity line |

Regenerate an oracle-derived golden from the oracle. Never from ccfish: that
converts a red gate into a recorded bug.

```sh
NORM=$(sed -n '/^normalize()/,/^}/p' build.sh)
cd ../.ccfish-upstream-oracle/src && eval "$NORM"
{ ./stockfish < /home/usr00/_git/ccfish/tools/cases/<case>.uci 2>&1; printf 'exit=%d\n' "$?"; } \
  | normalize > /home/usr00/_git/ccfish/tools/<case>.golden
```

## Moving a self-photograph to the oracle

Each remaining self-photograph is self-generated because of a *named* gap, not
because upstream cannot be reached. When the gap closes, re-derive it from the
oracle and let the gate tell you whether it really closed. To see how far one
currently is — hold the worktree path in `W` first, because `$PWD` inside the
subshell is already the oracle's directory, not this tree's:

```sh
W=$PWD; NORM=$(sed -n '/^normalize()/,/^}/p' build.sh); eval "$NORM"
diff <(./build/ccfish < tools/cases/search.uci 2>&1 | normalize) \
     <(cd ../.ccfish-upstream-oracle/src && ./stockfish < "$W/tools/cases/search.uci" 2>&1 | normalize)
```

`handshake` will not reach zero on the `id name` line — ccfish is not named
Stockfish, and `normalize` replaces that line with `<engine banner>` for exactly
that reason. Every other line of it should reach zero.

## What `normalize` hides, and why that is dangerous

`normalize` elides volatile fields (time, nps) and **drops** upstream lines ccfish
does not yet emit because the subsystem is unwired — the thread-pool and
shared-memory `info string`s. Those drops are the only thing keeping a gap out of
the goldens, so when the subsystem lands, delete its line from `normalize` FIRST
and let the gate go red. A filter that outlives its gap silently stops comparing
real output.
