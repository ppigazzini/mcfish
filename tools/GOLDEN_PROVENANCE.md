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
| `eval` | ccfish | self-photograph — the NNUE trace block is unported |
| `handshake` | **oracle**, one line substituted | byte-for-byte upstream except `id name` |
| `search` | ccfish | self-photograph — carries the `<engine banner>` identity line |

Regenerate an oracle-derived golden from the oracle. Never from ccfish: that
converts a red gate into a recorded bug.

```sh
NORM=$(sed -n '/^normalize()/,/^}/p' build.sh)
cd ../.ccfish-upstream-oracle/src && eval "$NORM"
{ ./stockfish < /home/usr00/_git/ccfish/tools/cases/<case>.uci 2>&1; printf 'exit=%d\n' "$?"; } \
  | normalize > /home/usr00/_git/ccfish/tools/<case>.golden
```

## `handshake`: the one legitimate substitution

`handshake` is derived from the oracle and then has exactly one line rewritten,
because exactly one line cannot be compared: ccfish is not named Stockfish, and
`normalize` rewrites the *banner* but not `id name`. Every other line — the option
order, every type, default and bound, the blank line upstream emits before the
first option — is upstream's own bytes.

```sh
W=$PWD
NORM=$(sed -n '/^normalize()/,/^}/p' build.sh)
CC_ID=$(printf 'uci\nquit\n' | ./build/ccfish | grep '^id name ')
( cd ../.ccfish-upstream-oracle/src && eval "$NORM"
  { ./stockfish < "$W/tools/cases/handshake.uci" 2>&1; printf 'exit=%d\n' "$?"; } | normalize ) \
  | sed "s|^id name Stockfish .*|$CC_ID|" > tools/handshake.golden
```

The substitution is the whole exception, and it must stay one `sed` on one
anchored line. Widen it and the gate stops comparing the option table, which is
the only thing it exists to compare.

## Moving a self-photograph to the oracle

Each remaining self-generated golden is so because of a *named* gap, not because
upstream cannot be reached. When the gap closes, re-derive it from the oracle and let the
gate tell you whether it really closed. To see how far one currently is:

```sh
NORM=$(sed -n '/^normalize()/,/^}/p' build.sh); eval "$NORM"
diff <(./build/ccfish < tools/cases/eval.uci 2>&1 | normalize) \
     <(cd ../.ccfish-upstream-oracle/src && ./stockfish < "$PWD/tools/cases/eval.uci" 2>&1 | normalize)
```

`handshake` reaches zero on every line but `id name`, which is why it is
oracle-derived with that one line substituted rather than self-generated.

## What `normalize` hides, and why that is dangerous

`normalize` elides volatile fields (time, nps) and **drops** upstream lines ccfish
does not yet emit because the subsystem is unwired — the thread-pool and
shared-memory `info string`s. Those drops are the only thing keeping a gap out of
the goldens, so when the subsystem lands, delete its line from `normalize` FIRST
and let the gate go red. A filter that outlives its gap silently stops comparing
real output.
