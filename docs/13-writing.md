# Writing these docs

How this set is organised, what a page here must be true about, and what
`./build.sh docs-lint` does and does not check. Read it before adding or editing
a page.

Audience: anyone editing these docs.

## The set

`README.md` is the index — GitHub renders it for the folder, so it is what a
reader lands on. The rest are `00-`…`14-`, numbered by **reading
order**, not importance: a contributor works down from the architecture into a
zone. The prefix is the only ordinal, and the last page is a lookup rather than a
stop on that walk: [14-glossary.md](14-glossary.md) defines the words the other
pages use without pausing on them.

Each page owns one subsystem and names its **audience** in the index table. A page
describes **what this codebase does** — not what upstream does, not what a chess
engine does in general. Anything a reader could get from the Chess Programming
Wiki belongs in [12-references.md](12-references.md) as a link.

### Naming a module that does not exist here yet

Pages routinely need to name code that lives in Stockfish rather than in this
tree. It is outside this repository, and writing it as a bare `src/...` path
invites a reader to look for it here.

The convention:

- A Stockfish golden, relative to Stockfish's `src/`: *upstream `nnue/network.cpp`*.

Never write a reserved mcfish path — a file the port map names but nobody has
written — as if it existed. Name the upstream golden instead. The port map is the
one place a reserved path is allowed to appear, because it is a work list, not a
claim about the tree.

## The rules

Each one is here because breaking it shipped a defect in this project.

**Describe a gap as a gap, never as a design.** This is the rule this set was
rewritten to fix. *"mcfish does not aim to match Stockfish"* and *"the evaluation
is a classical placeholder"* read as architecture. They were not: NNUE, Syzygy,
Lazy-SMP and NUMA are **required**, and the classical evaluation is scaffolding
scheduled for deletion. Framing a hole as a decision is what keeps it alive —
nobody fixes a design. If something is unimplemented, say unimplemented, name the
Stockfish golden that owns it, and say what its absence costs today.

**A ported module outside `build.sh`'s `SOURCES` is unwired, not "deferred".** This
is the same rule at the stage the tree is actually at, and it is the easier one to
get wrong, because the code *is there*. It is not in the binary, no gate reads it,
and it rots against the files that do move. Say which array a module is in, or say
plainly that it is in neither and what that costs. Never write "available" or
"ready" for a file nothing compiles.

**Never rationalise a defect into a convention.** The sibling of the rule above,
one level down. When you find yourself writing the sentence that makes the odd
thing sound intended, stop and check whether it is. One such sentence — "the
engine routes UCI output to stderr (same convention as the bench signature)" —
kept a P0 alive for months.

**Name the owner and the invariant, not just the mechanism.** Say which file and
symbol owns the behaviour and what must stay true about it. `depth8` in the TT is
the local example: "stores a depth" is accurate and useless. What a reader needs
is that `depth8 != 0` is the **occupancy test**, so the `DEPTH_ENTRY_OFFSET` bias
`tt_save` applies is load-bearing and a wrapping decrement would turn a penalised
shallow entry into the deepest entry in the table. Write the sentence a reader
needs before they delete your line.

**Verify the claim against the tree; drive the binary when it is behavioural.**
Not "read it carefully" — run it. `grep -n` for a symbol, `printf 'uci\n' |
./build/mcfish` for a handshake. Several claims in the first draft of this set
were false and each took seconds to disprove.

`docs-lint` holds four classes of this automatically — a backticked `snake_case`
**or CamelCase** symbol must exist somewhere in the tree, a cited `./build.sh <step>`
must be a step the dispatch table actually has, a named path must resolve in this repo's
**index** or in the Stockfish golden beside it, and every `build.sh` step must be
mentioned by some page — so a rename or a new step cannot rot quietly. Resolving
against the index rather than the working directory is what makes the verdict a
fact about the tree a reader clones: a file left behind by a rename is dead to
them however present it is to you. Its one hole is stated rather than hidden — a
path `.gitignore` names is exempt, because it documents the tool that writes it,
so a dead *ignored* path is checked by nothing. The path check reads
both spellings, prose and backticked, but only where the claim carries a file
**extension**: `src/engine/board/` names a family and resolves against nothing,
so write the file if you want the gate to hold it. `src/does/not/exist.c` is the
one path this repository guarantees never exists, reserved so this page can spell
a dead reference; the gate fails if it ever becomes real.

**Three classes stay out of its reach, and they are the common ones:** a real
symbol attributed to the wrong *file*; a list with the wrong *count* or *order*,
such as the gates `parity` runs; and a flag or behaviour described as absent from
a build that has it. Each of those lints perfectly clean. When you change
behaviour, re-read the pages that describe it in the same commit — the gate will
not do it for you.

**Separate upstream fact from mcfish state.** "Upstream does X" is checkable
against the SHA in `tools/upstream/UPSTREAM_BASE`. "mcfish does Y" is a claim
about a tree mid-port, and the reader needs to know whether Y is the target or the
scaffolding. Blur them and nobody can tell what they are allowed to change.

**Never pin a number a gate computes.** The bench signature, node counts, nps.
Cite `./build.sh signature`. Every figure written into prose goes stale, and nobody
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
thoroughness; it is where rot hides. This binds a generated page exactly as it binds
a hand-written one — match the length to what the change needs, and add no section
that exists to look complete: a summary restating the section above it, a recap of
what a gate prints, a next-steps list nobody asked for.

## Hot and cold

These pages do not age alike, and treating them the same is why they rot. A page
is **hot** when it describes code that moves. It is **cold** when what it
describes barely moves.

**The whole engine set is hot**, and more so than in a finished project: the port
still replaces modules wholesale rather than tweaking them, and every perf campaign
rewrites hot bodies without moving a node count. A page describing scaffolding — the
classical evaluation is the standing example — is describing code with a demolition
date, so say so rather than documenting it as the design.

**Change hot code, re-read its page in the same commit.** A doc is wrong from the
moment the code lands, and nobody knows which claim broke better than the person
who broke it.

| page | owns | temperature |
| --- | --- | --- |
| [00-architecture.md](00-architecture.md) | the three zones, the zone rule, the composition root, what is in the build | hot |
| [01-engine-board.md](01-engine-board.md) | `src/engine/board/` | hot |
| [02-engine-search.md](02-engine-search.md) | `src/engine/search/` | hot — the decomposition is where per-node work is tuned |
| [03-engine-eval.md](03-engine-eval.md) | `src/engine/eval/` | hot — the classical fallback under NNUE is scaffolding awaiting deletion |
| [04-multithreading.md](04-multithreading.md) | Lazy-SMP, the pool, per-worker state, NUMA | hot — NUMA net replication is still open |
| [05-tablebases.md](05-tablebases.md) | the Syzygy prober and its gates | warm — the prober is wired and gated; the open items are coverage, not code |
| [06-platform.md](06-platform.md) | `src/platform/` | hot — the engine→platform edge moves as seams land |
| [07-shell.md](07-shell.md) | `src/shell/` | hot — `engine.c` owns the session, `uci.c` the transport over it |
| [08-idiomatic-c.md](08-idiomatic-c.md) | the C23 patterns, the porting patterns, the measurement discipline | cold |
| [09-type-design.md](09-type-design.md) | what each quantity denotes, and which of them are types | cold — the refutations outlive the code they were measured on |
| [10-tooling-ci.md](10-tooling-ci.md) | `build.sh` steps, `tools/`, `.github/workflows/` | hot |
| [11-performance.md](11-performance.md) | the nine instruments, and where this port stands against the golden | hot — a standing dies to the next sync |
| [12-references.md](12-references.md) | external links | cold |
| this page | the rules | cold |
| [14-glossary.md](14-glossary.md) | the words, and which tier each belongs to | warm — an entry names an owner, so a rename dates it |

Cold does not mean unowned. It means the claim outlives a release, so when it *is*
wrong it has usually been wrong for a long time.

## Code comments

Same rules, plus these. C gives you very few places to state an invariant — a
buffer carries no length, a return carries no error set, a table has no
compile-time assertion of its own shape — so the comment carries more weight
here, not less.

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
in `tools/upstream/UPSTREAM_BASE`. "upstream does this too" is not. A reader must
be able to tell a translated line from an invented one, so cite the golden
whenever the shape of the code came from it.

**Keep the integer-semantics comments.** Where a computation relies on wrapping,
on a truncation, or on a conversion boundary, that note is the whole reason the
line looks the way it does. C and C++ differ at exactly those edges. See
[08-idiomatic-c.md](08-idiomatic-c.md).

**No history, no meta.** Not "was a stub", not "changed in M3", not "the following
block does". A comment describes the code as it is, to someone who has never seen
it before.

**Never explain an oddity into a convention.** If you are writing a sentence that
makes a strange thing sound intended, stop and check whether it is a bug — that
sentence is load-bearing for the next reader who might have fixed it.

## Commit messages

**The one surface where history is the subject rather than the contamination.**
"No history" above sends the before/after here, and here it is not a courtesy —
it is the only searchable record this port has of what was tried. AGENTS.md tells
an agent to run `git log --grep=rfish` and `--grep=zfish` before acting on a
sibling finding, and to search the log before re-measuring anything. That only
answers because the bodies wrote it down.

**Subject: a conventional type, an optional scope, and the claim. 72 characters.**
The scope is the zone or module (`docs(tb)`, `fix(search)`, `perf(nnue)`). State
what is now true, not which area was touched.

**Body wrapped at 80, in three parts:** what the change is and why it is right,
the witness, then the gates.

**Record the refusal, not only the change.** A sibling behaviour probed and found
already correct here, a measurement that transferred as flat or negative, a patch
declined with its reason — those belong in the body of the commit that refused
them, and several of this tree's commits carry nothing else. A refutation deleted
is a session the next contributor spends again, and the sweep discipline in
AGENTS.md is built on being able to find them.

**The witness has to be specific.** A behaviour claim is adjudicated against the
oracle, quoted both sides. A gate claim names the mutation that reddened it. A
cost claim carries the tool, the rounds, the ratio and the node count, and it says
which arch tier produced them — a number without its tier is reproducible nowhere.

**The gates block names each gate and the exit code, read from the gate itself.**
Not a pipe. And the exit codes here are not two-valued: **0 passes, 1 is drift, 2
is a rig fault, and 127 is a gate that never ran** because a tool was missing.
`parity` names the ones it skipped; a body that folds a 127 into "all green" is
reporting a pass over a check that did not happen. Say "no gate skipped" only when
that is what the gate said.

**A commit body MAY quote the bench anchor, and is the one place that may.**
`docs_lint.sh` refuses a page that quotes it, because prose is read as current and
the anchor moves on every intended behaviour change. A commit is timestamped by
construction, so `signature <count> UNCHANGED` in a gates block — with the real
count — is a fact about that commit and cannot rot. Do not carry it back into a
page: this paragraph cannot spell the number either, and the gate is what stopped
the draft that did.

**One logical change per commit.** A commit touching three modules cannot be
bisected when the node count moves. **No `Co-authored-by`, no generated-by
trailers, and do not `git push`** — commit locally and stop unless asked.

Upstream's own convention does not apply here and should not be copied across: its
`Bench:` / "No functional change" trailer and its SPRT blocks exist because that
project decides functional changes on fishtest, which this port does not use. The
evidence here is a gate on one machine, and it belongs in the gates block where
the next reader can re-run it.

## The gates

Three steps decide prose rather than code, and all three are this page's subject.

| step | what it proves here | owned by |
|---|---|---|
| `docs-lint` | the mechanical half of documentation rot: links, named paths, a quoted signature, a symbol that no longer exists, and a step no page mentions | this page |
| `cite-check` | that a cited commit SHA still names a commit a reader can REACH — ancestry, not existence | this page |
| `shellcheck` | the gates themselves are sound shell, which is the language most of the claims on these pages are enforced in | [10-tooling-ci.md](10-tooling-ci.md) |

### `./build.sh docs-lint`

```bash
./build.sh docs-lint      # also runs inside ./build.sh parity
```

[`../tools/docs_lint.sh`](../tools/docs_lint.sh) lints exactly the **tracked**
`*.md` files — `git ls-files`, so untracked scratch, build output and agent
worktrees carry no claims it owns — and fails on:

- **A dead internal link.** Any `[text](target)` whose target is not an external
  URL, a `mailto:`, or a bare `#anchor` must resolve as a path relative to the
  linking file. A trailing `#anchor` is stripped before the check, so the anchor
  itself is **not** verified — a link to a heading that no longer exists passes.
- **A named path that exists in no repo**, in prose *and* in backticks. The prose
  half reads `src/…`, `tools/…`, `tests/…`, `verify/…`, `scripts/…` and `docs/…`;
  the backticked half reads the same set plus `build/`, `resources/` and
  `.github/`, and requires a file **extension**, which is what confines it to file
  claims rather than to directories. Paths resolve against this repo's **index**
  or the golden checkout beside it, never the working directory, so an untracked
  local file cannot green a claim a fresh clone would fail.
- **A reference into the gitignored dev area.** A tracked file that names it sends
  every other reader to something their checkout does not contain.
- **A quoted bench signature.** The current value of `tools/signature.golden`
  appearing anywhere in a doc is a failure, with the message pointing at
  `./build.sh signature`.
- **A backticked `snake_case` symbol absent from the whole tree.**
- **A `build.sh` step no tracked page mentions**, and its reverse: a shipped file
  that says `./build.sh <step>` for a step `build.sh` does not dispatch.
- **A page under `docs/` with no `## The gates` section**, a step in no page's
  gates table, or a row routing a step to a page whose own table does not name
  it. Mentioning a step somewhere in the prose is not the same as being routed
  to, which is what the check above settles for and this one does not.

**Two extractions are floored.** The step list and the backticked path claims are
both read out of the tree by pattern, so a pattern that goes stale finds nothing
and the loop over it runs zero times — a green report over nothing. Each is held to
a floor just under its real count and exits 2 rather than 0 when it falls through.
That is not hypothetical: `../zfish`'s equivalent matched 5 of its 77 steps for as
long as it existed, green throughout, because its build file spells most steps
across three lines.

Three things it does not see, each of which will let a false claim through:

- **A `.gitignore`d path is exempt** and therefore checked by nothing. A page
  legitimately describes the tool that writes an ignored artifact; the dev-area
  check above exists separately for exactly this reason.
- **A path containing `*` is treated as a pattern**, not a claim, and skipped.
- **A bare filename** like `uci.c` is not checked at all. Write the path if you
  want the gate to hold it.

**The limit to hold in mind**: a `src/…` path that exists only in Stockfish passes
this gate while reading, in a mcfish page, as a claim about mcfish. The naming
convention above is what keeps the two apart, and nothing mechanical enforces it.

### `./build.sh cite-check`

[`../tools/docs_cite.sh`](../tools/docs_cite.sh) reads every backticked 7–12 digit
hex token in a tracked `.md` and asks whether a reader can still reach the commit
it names. `docs-lint` holds the pages to paths and symbols that exist; it never
read the SHAs, and by the time this gate landed the tracked pages quoted
thirty-seven of them.

**The wrong test is the obvious one.** `git cat-file -e "$sha^{commit}"` asks
whether the object is in **this** clone — and a rebased branch leaves its
pre-rebase commits in the object store, where a backup ref pins them indefinitely.
A citation to a pre-rebase identity therefore resolves on the author's machine,
forever, and resolves nowhere else. The test here is ancestry:
`git merge-base --is-ancestor`.

**Off-branch is not a defect here, which is why there are four tiers rather than
two:**

- an ancestor of HEAD;
- off-branch but reachable from some ref — what the commit this port started
  from is, cited in `tools/upstream/PORT_SOURCES.md`: an object in this clone,
  reachable from a ref, an ancestor of nothing on this branch. It is the tree's
  only one, so the tier has exactly one live instance and `cite-check` prints the
  count;
- resolved in a sibling checkout — what AGENTS.md's rfish, zfish and refish
  citations are, which are not objects in this repository at all;
- reachable from nothing, which is the finding.

A missing sibling **narrows** the run and names it rather than failing, and a
shallow clone narrows to nothing, because ancestry is unanswerable there.

### They cannot tell you a sentence is false

This is the whole point of the section. A page can link cleanly, name only real
paths, quote no signature — and still describe code that was replaced three
commits ago, or frame an unported subsystem as a design decision. Both failures
have happened in this set, and neither is mechanically detectable.

The gates buy the mechanical half so review can spend its attention on the half
that needs a reader. That is the failure mode to write against: docs here are
accurate when written and rot where the code moves under them, and in a repository
mid-port the code moves a lot. Prefer the claim that stays true — name the owner
and the invariant, name the upstream golden for what is missing, and point at the
gate for the number.
