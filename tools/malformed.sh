#!/usr/bin/env bash
# Assert that a KNOWN-BAD tablebase file is still refused.
#
# Every other gate here watches the engine compute the right answer from a
# well-formed input. `signature` is the anchor and it is green with every parser
# defect this file covers LIVE, because the bench reads no file the engine did not
# ship with. `fuzz-tb` looks for input that is bad in a way nobody has described
# yet -- a different job, on a nightly budget, and probabilistic by construction.
# Neither asks whether a file that was refused yesterday is refused today, so
# nothing did, and the bounds in decode.c and registry.c are the only thing
# standing between a crafted `.rtbw` and the search.
#
# A fixture here is a GENERATOR, not a committed blob. The bytes are six lines of
# Python and the interesting thing about them is WHICH FIELD is wrong; a blob hides
# that and rots the moment the format is read differently.
#
# THESE NEED NO SYNTHETIC MUTATION: the defect is the mutation. Each one is a
# reproducer for a defect that is LIVE IN UPSTREAM STOCKFISH TODAY -- an
# out-of-bounds heap write, a negative size reaching a resize, two shift counts
# taken raw from the file, and a decoded symbol leaving the alphabet it indexes.
# This tree bounds all four; nothing until now re-checked that it still does.
#
# TWO FAMILIES, AND THE SECOND IS THE STRONGER ONE.
#
# REFUSED -- crafted 80-byte headers. The parser must reject them before the table
# is usable. WHAT THIS FAMILY CANNOT ASSERT, written down because it is not
# obvious: the engine emits ONE diagnostic for every refusal, so the gate cannot
# see WHICH check fired. It proves "a crafted header is refused safely and says
# so", never "refused by the check its name implies". The names were re-derived
# against an instrumented build rather than inherited from the sibling -- each one
# trips the site named beside it, on the FIRST of the two per-side parses:
#
#   negative-resize   decode.c, the min > max refusal
#   base64-shift      the same refusal, its min == 0 clause
#   block-shift       decode.c, the block/span log >= 64 refusal
#   btree-past-end    decode.c, the symlen array reaching past the file
#   bad-magic         registry.c, the magic compare
#
# Two fixtures the sibling carries are DELIBERATELY ABSENT, and re-adding them
# would gate nothing here: `symbol-oob` (a btree child outside the alphabet) and
# `symlen-past-domain` (more symbols than a 12-bit Sym can name) both PASS this
# tree's parser by design -- `set_sym_len` clamps an out-of-domain child instead of
# writing through it, and the containers are sized to the DECLARED count rather
# than to a 12-bit cap. Both were refused only because a 2-sided table's second
# header lands in this file's zero padding, which is not what their names claim.
#
# ABSORBED -- real 3-man tables with a handful of bytes changed. These LOAD, so the
# search reaches the decode loop, which is where this tree's per-symbol bounds live
# and where no crafted header can reach: an 80-byte file is refused long before,
# its sparse index alone outrunning the file. The format records nothing that says
# which value was meant, so a reader cannot detect these and must not pretend to;
# what it must do is stay inside its arrays and keep answering. A different bar,
# not a weaker one -- and it discriminates: with the `sym >= symlen_size` bound
# removed, `symbol-past-end` is an ASan heap-buffer-overflow.
#
# What a refusal means here, and all four parts are checked:
#
#   * the process exits 0 -- not a signal, not an abort;
#   * no sanitizer reports anything;
#   * it prints a diagnostic naming the file, so an operator can act. A silent
#     refusal is half a refusal: the table is not being used and nothing says so;
#   * it still answers. A parser that takes the engine down with it has not
#     refused the file, it has been defeated by it.
#
# And one negative control, which is the half that keeps the other four honest: a
# CLEAN corpus must emit no diagnostic at all. Without it "always print Corrupt"
# passes every row above.
#
# Run against the asan+ubsan build, so an out-of-bounds read the shipped binary
# would absorb into mmap's page padding is REPORTED rather than survived. A gate
# for refusals must be stricter than the binary it protects, or it certifies the
# reads it cannot see.
#
# NO SLEEPS, unlike the sibling this is ported from. Upstream quits mid-search on
# EOF, so its harness has to hold stdin open and every fixture costs seconds; this
# tree ends a running search before leaving the loop and waits out a bounded one
# (uci.c), so the whole command list goes in at once and `quit` is a barrier.
#
# Exit codes:  0 every fixture refused   1 a fixture was not   2 skipped

set -u
set -o pipefail

ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
cd "$ROOT" || exit 2

EXE=${EXE:-$ROOT/build/mcfish-debug}
RESOURCES=${RESOURCES:-$ROOT/resources}
CORPUS=${CORPUS:-$RESOURCES/syzygy}

[ -x "$EXE" ] || { echo "malformed: SKIPPED -- no engine at $EXE (./build.sh debug)" >&2; exit 2; }

WORK=$(mktemp -d "${TMPDIR:-/tmp}/mcfish_malformed.XXXXXX")
trap 'rm -rf "$WORK"' EXIT

# ---------------------------------------------------------------- the fixtures
#
# One WDL table for KQvK, 80 bytes: `st_size % 64 == 16` and a correct magic are
# the only two things the mapper checks before the parser is handed the bytes, and
# mmap zero-pads the rest of the page, so this is the smallest input that reaches
# the parser at all.
#
# Field offsets, all of them read straight out of the file:
#   4      Split | HasPawns          11  log2 sizeofBlock
#   12     log2 span                 13  padding
#   14     blocksNum (u32)           18  maxSymLen        19  minSymLen
#   20     lowestSym[]               22  symlen count (u16)   24  btree[0]
cat > "$WORK/gen.py" <<'PY'
import os
import sys


def base():
    b = bytearray(80)
    b[0:4] = bytes([0x71, 0xE8, 0x23, 0x5D])   # WDL magic
    b[4] = 0x01                                # Split = 1, HasPawns = 0
    b[11] = 6; b[12] = 6                       # sizeofBlock = span = 64
    # block_length_size is blocks_num + padding, and decode_pairs refuses a block
    # index at or past it. One padding entry with no blocks keeps that refusal off
    # the path, so each fixture below is refused for ITS OWN reason, not this one.
    b[13] = 1                                  # padding = 1
    b[18] = 1; b[19] = 1                       # maxSymLen = minSymLen = 1
    b[22] = 1                                  # one Huffman symbol declared
    return b


def negative_resize():
    """minSymLen above maxSymLen: base64 is sized from their difference."""
    b = base()
    b[18] = 0; b[19] = 0xFF
    return b


def block_shift():
    """A block size of 1 << 200."""
    b = base()
    b[11] = 200
    return b


def base64_shift():
    """minSymLen 0, which right-pads base64 by exactly 64 bits."""
    b = base()
    b[18] = 0; b[19] = 0
    return b


def btree_past_end():
    """65535 symbols declared by a file that holds 80 bytes."""
    b = base()
    b[22] = 0xFF; b[23] = 0xFF
    return b


def bad_magic():
    """A file named KQvK.rtbw that is not a WDL table at all."""
    b = base()
    b[0:4] = b"\x00\x00\x00\x00"
    return b


dest = sys.argv[2]
os.makedirs(dest, exist_ok=True)
with open(os.path.join(dest, "KQvK.rtbw"), "wb") as fh:
    fh.write(bytes(globals()[sys.argv[1]]()))
PY

# Every fixture is a KQvK table, so they all take the same probe.
KQVK_FEN="4k3/8/8/8/8/8/8/3QK3 w - - 0 1"

# name              | generator
FIXTURES=(
  "negative-resize   |negative_resize"
  "block-shift       |block_shift"
  "base64-shift      |base64_shift"
  "btree-past-end    |btree_past_end"
  "bad-magic         |bad_magic"
)

PASS=0; FAIL=0
note() { echo "    $*"; }

# Drive one probe. Echoes the engine's whole output; the caller judges it.
probe() {
    local dir=$1
    ( printf 'setoption name SyzygyPath value %s\nposition fen %s\ngo depth 8\nquit\n' \
             "$dir" "$KQVK_FEN" ) \
      | ( cd "$RESOURCES" && timeout -s KILL 120 "$EXE" ) 2>&1
}

check_refused() {
    local name=$1 dir=$2 out rc
    out=$(probe "$dir"); rc=$?

    local bad=0
    [ "$rc" = "0" ] || { note "exit $rc -- a refusal exits 0"; bad=1; }
    if grep -qE 'AddressSanitizer|runtime error:|LeakSanitizer|UndefinedBehaviorSanitizer' <<< "$out"; then
        note "a sanitizer reported:"
        grep -m2 -E 'AddressSanitizer|runtime error:' <<< "$out" | sed 's/^/      /'
        bad=1
    fi
    if grep -qE 'terminate called|Assertion|Aborted' <<< "$out"; then
        note "the process aborted:"
        grep -m2 -E 'terminate called|Assertion' <<< "$out" | sed 's/^/      /'
        bad=1
    fi
    grep -qi 'Corrupt tablebase file' <<< "$out" \
      || { note "no diagnostic -- refused without saying so, or accepted"; bad=1; }
    grep -q '^bestmove' <<< "$out" || { note "no bestmove -- the engine stopped answering"; bad=1; }

    if [ "$bad" = "0" ]; then
        printf '  %-19s refused\n' "$name"; PASS=$((PASS + 1))
    else
        printf '  %-19s NOT REFUSED\n' "$name"; FAIL=$((FAIL + 1))
    fi
}

echo "malformed: 5 crafted headers, 4 mutated tables, against $(basename "$EXE")"
for row in "${FIXTURES[@]}"; do
    IFS='|' read -r name gen <<< "$row"
    name=${name%% *}
    gen=${gen// /}
    if ! python3 "$WORK/gen.py" "$gen" "$WORK/fx/$gen"; then
        echo "malformed: SKIPPED -- the $gen generator failed" >&2
        exit 2
    fi
    check_refused "$name" "$WORK/fx/$gen"
done

# ------------------------------------------------- the absorbed family
#
# Real tables with a handful of bytes changed. Each edit list is REPLAYED FROM THE
# FUZZ RUN THAT FOUND IT rather than quoted as a seed: a seed reproduces the
# harness, a byte list reproduces the defect, and it survives the harness changing.
#
# The judge asks for SURVIVAL, not for a diagnostic. These land on fields whose
# only constraint is internal consistency, and the format records nothing that says
# which value was meant -- so a reader cannot detect them and must not pretend to.
# It must stay inside its arrays and keep answering. `Found 5 WDL` is checked for
# the same reason the clean control is: a fixture that loaded nothing probed
# nothing, and would pass while gating the decode loop it exists to reach.
mutate() {
    local dest=$1 stem=$2; shift 2
    mkdir -p "$dest"
    cp "$CORPUS"/*.rtbw "$CORPUS"/*.rtbz "$dest/" 2> /dev/null || return 1
    python3 - "$dest/$stem" "$@" <<'PYMUT'
import sys
path, edits = sys.argv[1], [int(x) for x in sys.argv[2:]]
b = bytearray(open(path, "rb").read())
for i in range(0, len(edits), 2):
    b[edits[i]] = edits[i + 1]
open(path, "wb").write(bytes(b))
PYMUT
}

check_absorbed() {
    local name=$1 dir=$2 fen=$3 out rc
    out=$( ( printf 'setoption name SyzygyPath value %s\nposition fen %s\ngo depth 12\nquit\n' \
                    "$dir" "$fen" ) \
           | ( cd "$RESOURCES" && timeout -s KILL 120 "$EXE" ) 2>&1 ); rc=$?

    local bad=0
    [ "$rc" = "0" ] || { note "exit $rc -- survival means exiting 0"; bad=1; }
    if grep -qE 'AddressSanitizer|runtime error:|UndefinedBehaviorSanitizer' <<< "$out"; then
        note "a sanitizer reported:"
        grep -m2 -E 'AddressSanitizer|runtime error:' <<< "$out" | sed 's/^/      /'
        bad=1
    fi
    if grep -qE 'terminate called|Assertion|Aborted' <<< "$out"; then
        note "the process aborted:"; bad=1
    fi
    grep -q '^bestmove' <<< "$out" || { note "no bestmove -- it stopped answering"; bad=1; }
    grep -q 'Found 5 WDL' <<< "$out" \
      || { note "the table did not load, so the decode loop was never reached"; bad=1; }

    if [ "$bad" = "0" ]; then
        printf '  %-19s absorbed\n' "$name"; PASS=$((PASS + 1))
    else
        printf '  %-19s NOT ABSORBED\n' "$name"; FAIL=$((FAIL + 1))
    fi
}

corpus_files=0
for f in "$CORPUS"/*.rtbw "$CORPUS"/*.rtbz; do [ -s "$f" ] && corpus_files=$((corpus_files + 1)); done

if [ "$corpus_files" -ne 10 ]; then
    echo "  absorbed family     SKIPPED -- $CORPUS has $corpus_files/10 files (./build.sh tb-fetch)"
else
    mutate "$WORK/mut/huffman-noncanon" KRvK.rtbw \
      32 220 108 111 29 250 109 40 189 0 65 158 113 213 163 212
    check_absorbed "huffman-noncanon" "$WORK/mut/huffman-noncanon" \
      "4k3/8/8/8/8/8/8/3RK3 w - - 0 1"

    mutate "$WORK/mut/symbol-past-end" KQvK.rtbw \
      144 1 222 248 35 189 268 220 66 15 228 85 229 65 108 212
    check_absorbed "symbol-past-end" "$WORK/mut/symbol-past-end" \
      "4k3/8/8/8/8/8/8/3QK3 w - - 0 1"

    mutate "$WORK/mut/cyclic-btree" KRvK.rtbw 100 132 53 183 119 76 185 57 183 198
    check_absorbed "cyclic-btree" "$WORK/mut/cyclic-btree" \
      "4k3/8/8/8/8/8/8/3RK3 w - - 0 1"

    mutate "$WORK/mut/flags-vs-material" KNvK.rtbw 4 192 41 124 33 37 5 7 61 88 54 110
    check_absorbed "flags-vs-material" "$WORK/mut/flags-vs-material" \
      "4k3/8/8/8/8/8/8/3NK3 w - - 0 1"
fi

# ------------------------------------------------------------ the controls
#
# A gate that only ever asks for the word "Corrupt" is passed by an engine that
# prints it unconditionally. These are the rows that stop that, and they are the
# reason the refusals above can be trusted at all.
#
# The FIRST needs no corpus, which is what makes it the one that always runs: a
# SyzygyPath with no tables in it must produce no diagnostic. That is the whole of
# the unconditional-print failure mode, and it is available on a machine that has
# fetched nothing.
mkdir -p "$WORK/empty"
out=$(probe "$WORK/empty")
if grep -qi 'Corrupt tablebase file' <<< "$out"; then
    note "an EMPTY path produced a corruption diagnostic:"
    grep -m2 -i 'Corrupt' <<< "$out" | sed 's/^/      /'
    printf '  %-19s FAILED\n' "empty-path"; FAIL=$((FAIL + 1))
elif ! grep -q '^bestmove' <<< "$out"; then
    note "the engine stopped answering with an empty tablebase path"
    printf '  %-19s FAILED\n' "empty-path"; FAIL=$((FAIL + 1))
else
    printf '  %-19s silent\n' "empty-path"; PASS=$((PASS + 1))
fi

# The SECOND needs the 3-man set, and is the stronger claim: a table that is REAL
# must load and stay quiet. Without the corpus it is unexercised, which is reported
# the way `do_tb` reports its own missing half -- narrow the gate, never fail it,
# and never let the narrowing read as a pass.
if [ "$corpus_files" -ne 10 ]; then
    printf '  %-19s SKIPPED -- %s has %s/10 files\n' "clean-corpus" "$CORPUS" "$corpus_files"
else
    out=$(probe "$CORPUS")
    if grep -qi 'Corrupt tablebase file' <<< "$out"; then
        note "a VALID table was reported corrupt:"
        grep -m2 -i 'Corrupt' <<< "$out" | sed 's/^/      /'
        printf '  %-19s FAILED\n' "clean-corpus"; FAIL=$((FAIL + 1))
    elif ! grep -q 'Found 5 WDL' <<< "$out"; then
        note "the control loaded no table, so it tested nothing"
        printf '  %-19s FAILED\n' "clean-corpus"; FAIL=$((FAIL + 1))
    else
        printf '  %-19s silent\n' "clean-corpus"; PASS=$((PASS + 1))
    fi
fi

echo
if [ "$corpus_files" -ne 10 ]; then
    echo "malformed: $PASS passed, $FAIL failed -- THE CORPUS HALF IS UNEXERCISED" >&2
    echo "  the absorbed family and the clean-corpus control need ./build.sh tb-fetch;" >&2
    echo "  what ran is the crafted-header family and the empty-path control." >&2
else
    echo "malformed: $PASS passed, $FAIL failed"
fi
[ "$FAIL" = "0" ] || exit 1
exit 0
