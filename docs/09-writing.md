# Writing these docs

How this set is organised, what a page here must be true about, and what
`./build.sh docs-lint` does and does not check. Read it before adding or editing
a page.

Audience: anyone editing these docs.

## The set

`README.md` is the index — GitHub renders it for the folder, so it is what a
reader lands on. [PORTING.md](PORTING.md) sits outside the numbering because it
is not a subsystem page: it is the statement of the goal, and every other page
must be consistent with it. The rest are `00-`…`09-`, numbered by **reading
order**, not importance: a contributor works down from the architecture into a
zone. The prefix is the only ordinal.

Each page owns one subsystem and names its **audience** in the index table. A page
describes **what this codebase does** — not what upstream does, not what a chess
engine does in general. Anything a reader could get from the Chess Programming
Wiki belongs in [08-references.md](08-references.md) as a link.

### Naming a module that does not exist here yet

Pages routinely need to name code that lives in zfish or in Stockfish rather than
in this tree. Both are outside this repository, and writing either as a bare
`src/...` path invites a reader to look for it here.

The convention:

- A zfish module, relative to zfish's own `src/`: *zfish `engine/eval/nnue_ft.zig`*.
- A Stockfish golden, relative to Stockfish's `src/`: *upstream `nnue/network.cpp`*.
- The mapping between them and the ccfish owner: cite the row in
  `tools/upstream/port_map.tsv`, and cite `./build.sh port-status` for counts.

Never write a reserved ccfish path — a file the port map names but nobody has
written — as if it existed. Name the zfish source instead. The port map is the one
place a reserved path is allowed to appear, because it is a work list, not a claim
about the tree.

## The rules

Each one is here because breaking it shipped a defect in this project or in zfish.

**Describe a gap as a gap, never as a design.** This is the rule this set was
rewritten to fix. *"ccfish does not aim to match Stockfish"* and *"the evaluation
is a classical placeholder"* read as architecture. They were not: NNUE, Syzygy,
Lazy-SMP and NUMA are **required**, and the classical evaluation is scaffolding
scheduled for deletion. Framing a hole as a decision is what keeps it alive —
nobody fixes a design. If something is unimplemented, say unimplemented, name the
zfish module and Stockfish golden that own it, and say what its absence costs
today.

**A ported module outside `build.sh`'s `SOURCES` is unwired, not "deferred".** This
is the same rule at the stage the tree is actually at, and it is the easier one to
get wrong, because the code *is there*. It is not in the binary, no gate reads it,
and it rots against the files that do move. Say which array a module is in, or say
plainly that it is in neither and what that costs. Never write "available" or
"ready" for a file nothing compiles.

**Never rationalise a defect into a convention.** The sibling of the rule above,
one level down. When you find yourself writing the sentence that makes the odd
thing sound intended, stop and check whether it is. In zfish that sentence — "the
engine routes UCI output to stderr (same convention as the bench signature)" —
kept a P0 alive for months.

**Name the owner and the invariant, not just the mechanism.** Say which file and
symbol owns the behaviour and what must stay true about it. `depth8` in the TT is
the local example: "stores a depth" is accurate and useless. What a reader needs
is that `depth8 != 0` is the **occupancy test**, so `tt_store`'s `+1` bias is
load-bearing and a wrapping decrement would turn a penalised shallow entry into
the deepest entry in the table. Write the sentence a reader needs before they
delete your line.

**Verify the claim against the tree; drive the binary when it is behavioural.**
Not "read it carefully" — run it. `grep -n` for a symbol, `printf 'uci\n' |
./build/ccfish` for a handshake. Several claims in the first draft of this set
were false and each took seconds to disprove.

**Separate upstream fact from ccfish state.** "Upstream does X" is checkable
against the SHA in `tools/upstream/UPSTREAM_BASE`. "ccfish does Y" is a claim
about a tree mid-port, and the reader needs to know whether Y is the target or the
scaffolding. Blur them and nobody can tell what they are allowed to change.

**Never pin a number a gate computes.** The bench signature, node counts, nps,
module counts, how far the port has got. Cite `./build.sh signature` and
`./build.sh port-status`. Every figure written into prose goes stale, and nobody
thinks to grep the docs for a number.

**State the limit.** A doc that omits its own boundary invites over-trust. Say
what the thing does *not* cover: `zone-check` cannot see the engine→platform
edge; a 127 is a skipped gate; the golden-diff's
`normalize()` elides four fields that no golden then guards.

**Show the command.** "It is faster" is not a claim; `./build.sh bench 8` output
before and after is. A performance or behaviour claim ships with what produced it.

**No history.** "Used to be X", "fixed in Y", "previously a stub" is out of date
the day after and tells a reader nothing about the code in front of them. The
before/after belongs in the commit message.

**One example beats three paragraphs**, and **pair every prohibition with an
alternative**. "Don't open-code `(Square)(s + d)`" leaves a reader stuck; "use
`sq_add`, or `safe_step` when the step may leave the board" does not.

**Cut anything that does not help implement or verify.** Length is not
thoroughness; it is where rot hides.

## Hot and cold

These pages do not age alike, and treating them the same is why they rot. A page
is **hot** when it describes code that moves. It is **cold** when what it
describes barely moves.

**The whole engine set is hot right now**, and more so than in a finished project:
the port is replacing these modules wholesale, not tweaking them. A page that
describes an unwired module is describing code that will be edited on its way into
the build, and a page that describes a `PARTIAL` one is describing code with a
scheduled demolition date.

**Change hot code, re-read its page in the same commit.** A doc is wrong from the
moment the code lands, and nobody knows which claim broke better than the person
who broke it.

| page | owns | temperature |
| --- | --- | --- |
| [PORTING.md](PORTING.md) | the goal, the zfish/Stockfish roles, the M1..M6 sequence | warm — the milestones outlive a commit; the status does not |
| [00-architecture.md](00-architecture.md) | the three zones, the zone rule, the composition root, what is in the build | hot |
| [01-engine-board.md](01-engine-board.md) | `src/engine/board/` | hot |
| [02-engine-search.md](02-engine-search.md) | `src/engine/search/` | hot — the live search is scheduled for replacement by the decomposition beside it |
| [03-engine-eval.md](03-engine-eval.md) | `src/engine/eval/` | hot — and scheduled for wholesale replacement at M3 |
| [04-platform.md](04-platform.md) | `src/platform/` | hot — one module of the zone is in the build |
| [05-shell.md](05-shell.md) | `src/shell/` | hot — `uci.c` is scheduled for replacement by the decomposition beside it |
| [06-idiomatic-c.md](06-idiomatic-c.md) | the C23 patterns, the Zig→C translation patterns, the measurement discipline | cold |
| [07-tooling-ci.md](07-tooling-ci.md) | `build.sh` steps, `tools/`, `.github/workflows/` | hot |
| [08-references.md](08-references.md) | external links | cold |
| this page | the rules | cold |

Cold does not mean unowned. It means the claim outlives a release, so when it *is*
wrong it has usually been wrong for a long time.

## Code comments

Same rules, plus these. C gives you fewer places to state an invariant than Zig
does — no `comptime` assertion, no error set, no slice length — so the comment
carries more weight here, not less.

**Imperative mood, leading with a verb.** "Resolve the path", not "Returns the
path", "This resolves…", or "Function to resolve…". A comment is an order to the
reader, not a description of the author.

```c
// Clear the king's origin square from the occupancy before testing the
// destination.
// Read the deadline resolved at search start, never re-derive it here.
```

**Write only the constraint the code cannot show.** Never restate the next line.
Never say where the code came from or why your change is right — that is the
commit message's job and it is noise the moment the commit merges. If the line
reads plainly, say nothing.

**Name the invariant, and what breaks without it.**

```c
// Bias by +1 (upstream tt.cpp). depth8 == 0 IS the occupancy test, so an
// unbiased depth-0 store is indistinguishable from an empty slot and is
// silently unreadable.
```

That comment survives a refactor; "store the depth" does not.

**Cite upstream as `file:line`.** `search.cpp:2088` is checkable against the SHA
in `tools/upstream/UPSTREAM_BASE`. "upstream does this too" is not. When a port
decision came from zfish rather than from upstream, say which — a reader must be
able to tell a translated line from an invented one.

**Carry across the integer-semantics comments.** Where zfish notes that a
computation relies on wrapping, on a truncation, or on a conversion boundary, that
note is the whole reason the line looks the way it does. C, Zig and C++ differ at
exactly those edges. See [06-idiomatic-c.md](06-idiomatic-c.md).

**No history, no meta.** Not "was a stub", not "changed in M3", not "the following
block does". A comment describes the code as it is, to someone who has never seen
it before.

**Never explain an oddity into a convention.** If you are writing a sentence that
makes a strange thing sound intended, stop and check whether it is a bug — that
sentence is load-bearing for the next reader who might have fixed it.

## The gate

```bash
./build.sh docs-lint      # also runs inside ./build.sh parity
```

[`../tools/docs_lint.sh`](../tools/docs_lint.sh) finds every `*.md` outside
`build/`, `.git/`, `tests/` and `scripts/` — tracked or not — and fails on:

- **A dead internal link.** Any `[text](target)` whose target is not an external
  URL, a `mailto:`, or a bare `#anchor` must resolve as a path relative to the
  linking file. A trailing `#anchor` is stripped before the check, so the anchor
  itself is **not** verified — a link to a heading that no longer exists passes.
- **A named path that exists in no repo.** Any `src/…`, `tools/…`, `tests/…`,
  `verify/…` or `scripts/…` spelled out in prose is a claim about *a* tree, and
  must resolve under one of three roots: `.`, `../Stockfish`, or `../zfish`.
  **That is the limit to hold in mind**: a `src/…` path that exists only in
  Stockfish passes this gate while reading, in a ccfish page, as a claim about
  ccfish. The naming convention above is what keeps the two apart, and nothing
  mechanical enforces it.
- **A quoted bench signature.** The current value of `tools/signature.golden`
  appearing anywhere in a doc is a failure, with the message pointing at
  `./build.sh signature`. Note the limit: it only catches *that* number. A node
  count, an nps figure or a module count written into prose passes cleanly and is
  just as stale.

Three more things it does not see, each of which will let a false claim through:

- **Anything inside backticks or a fenced block is stripped before scanning.** An
  inline `` `src/does/not/exist.c` `` is invisible to the path check. Write a path
  bare, or as a markdown link target, if you want the gate to hold it.
- **A path containing `*` is treated as a pattern**, not a claim, and skipped.
- **A bare filename** like `uci.c` is not checked at all.

### It cannot tell you a sentence is false

This is the whole point of the section. A page can link cleanly, name only real
paths, quote no signature — and still describe code that was replaced three
commits ago, or frame an unported subsystem as a design decision. Both failures
have happened in this set, and neither is mechanically detectable.

The gate buys the mechanical half so review can spend its attention on the half
that needs a reader. That is the failure mode to write against: docs here are
accurate when written and rot where the code moves under them, and in a repository
mid-port the code moves a lot. Prefer the claim that stays true — name the owner
and the invariant, name the zfish module for what is missing, and point at the
gate for the number.
