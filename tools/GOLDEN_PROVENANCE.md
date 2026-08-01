# Where each golden came from

A golden is not automatically a reference. When it is regenerated from mcfish, it
is a **photograph of mcfish** — and it will pin a defect exactly as faithfully as it
pins correct behaviour, after which the gate passes *because* the engine is wrong.

That is not hypothetical here. `board.golden` and `errors.golden` were both
generated from mcfish and both pinned real divergences for as long as they existed:

- `board.golden` recorded a `d` output with **no `Checkers:` line**, which upstream
  always prints, and recorded mcfish silently accepting the illegal move `b4b6`
  and continuing.
- `errors.golden` recorded three invalid FENs each producing an identical start
  position and **no diagnostic at all**, where upstream names a reason and exits 1.

Both gates were green throughout.

## The audit, not this table, is the claim

This page used to end with a table asserting each golden's provenance, and that
table was **a claim no tool checked**. `./build.sh golden-update` drives mcfish, so
every resync quietly converted oracle-derived goldens back into self-photographs
while the table went on saying otherwise. It did exactly that during the c5aef2bf1
sync, to six of the eight.

`./build.sh golden-audit` ([`upstream_golden_audit.sh`](upstream_golden_audit.sh))
is the check. It drives the same `cases/*.uci` scripts through a **pristine upstream
build** and diffs the result against the committed golden, so the provenance is
re-derived on demand instead of remembered:

```
./build.sh golden-audit
  golden-audit: 8 agree, 0 differ, 0 missing
  golden-audit: every golden matches what upstream itself produces
```

**Every golden in this directory is upstream's own bytes**, `handshake` excepted on
one line (below). Standing the audit up found a real divergence on its first run:
mcfish emitted the `go` line's processor and thread info strings only on the search
path, because the perft arm returned from inside the argument loop — so `go perft`
was missing two lines upstream prints. `chess960` and `perft` were green over that
for as long as it existed, because both were photographs.

**LOCAL**: the audit needs the pinned upstream tree and a built oracle, which a CI
clone of this repo alone does not carry. The weekly `mcfish upstream check` workflow
runs it, since that lane already clones the golden and builds the oracle.

## Regenerating one

Use `./build.sh golden-audit --write`. It writes what upstream produced, so the
golden it leaves behind is adjudicated by construction:

```sh
./build.sh golden-audit --write        # every case that differs
./build.sh golden-audit --write perft  # just this one
```

Then run `./build.sh golden` and read what it says: a golden re-derived from the
oracle turns red exactly where mcfish still has to change to match, which is the
information the regeneration exists to produce.

`./build.sh golden-update` still exists and still drives **mcfish**. It is for a
case upstream cannot be driven through, and there are none today. Reaching for it
in a resync is what put six self-photographs in this directory.

`tb.golden` has its own regenerator, `./build.sh tb-update`, which runs the oracle
and refuses without the full 3-man set — there is no mcfish-derived path to it.

## `handshake`: the one legitimate substitution

`handshake` is derived from the oracle and then has exactly one line rewritten,
because exactly one line cannot be compared: mcfish is not named Stockfish, and
`normalize` rewrites the *banner* but not `id name`. Every other line — the option
order, every type, default and bound, the blank line upstream emits before the
first option — is upstream's own bytes. The audit performs the same substitution.

The substitution is the whole exception, and it must stay one `sed` on one
anchored line. Widen it and the gate stops comparing the option table, which is
the only thing it exists to compare. It is a **substitution and not a drop** on
purpose: the line's absence is still a diff.

## What `normalize` hides, and why that is dangerous

`normalize` elides volatile fields (time, nps) and **drops** upstream lines mcfish
does not yet emit because the subsystem is unwired — the NUMA network-replica
`info string`. Those drops are the only thing keeping a gap out of the goldens, so
when the subsystem lands, delete its line from `normalize` FIRST and let the gate go
red. A filter that outlives its gap silently stops comparing real output.

The audit shares `normalize` with the gate rather than restating it, by extracting
the function from `build.sh`. A second copy would drift from the first exactly when
it matters — when a gap closes and its line must stop being dropped.

## Driving the oracle: a case containing `go` needs pauses

The audit feeds each script to the oracle **line by line, with a settle after every
`go`**, and that is not incidental. mcfish's `go` is synchronous, so the gate itself
may pipe a whole script in at once; upstream searches on another thread, and a piped
`go` is cut short by the next command and yields a depth-1 stub — so the comparison
records a truncated search on upstream's side and reads as a divergence.

This produced a false result here once already: `search` appeared to differ from the
oracle by two lines and was recorded as a self-photograph, when driving the oracle
properly shows it is byte-identical.

**A case whose `go` is UNBOUNDED cannot be represented here, and the failure looks
like an engine bug.** The two halves of the comparison drive differently on purpose:
the audit feeds the oracle line by line with a settle after each `go`, because
upstream searches on another thread; `./build.sh golden` pipes the whole script at
mcfish, because it does not need one. For a BOUNDED `go depth N` that asymmetry is
invisible — `engine_end_search` waits a bounded search out. For an unbounded one
(`go infinite`, or a `go mate N` with no other limit) the oracle gets its settle and
finishes while mcfish is stopped by the immediately following `quit`, so the golden
records a full search and the gate produces `nodes 0` and an empty `pv`.

That is the harness, not a divergence: driven with a settle, mcfish produces the
oracle's bytes exactly. Keep a case's `go` bounded — `go depth 5 mate 1` still
exercises the mate limit, stopping at depth 1 — or the case is not answerable by
this pair of drivers.

The other rig detail is **cwd**. Every gate runs the engine from `resources/`, where
the net lives; run the oracle from its own worktree instead and it loads no net, and
every evaluation-bearing case differs for a reason that is not a divergence.
