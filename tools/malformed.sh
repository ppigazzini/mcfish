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


def symbol_oob():
    """A child symbol the declared alphabet does not contain."""
    b = base()
    b[24] = 0x00; b[25] = 0x00; b[26] = 0x80   # btree[0].Right = 2048
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


def symlen_past_domain():
    """More symbols than a 12-bit Sym can name, in a file with ROOM for them.

    btree_past_end declares 65535 in 80 bytes and the space check refuses it
    before the copy: 65535 * 3 does not fit. That is why a declared count was
    never compared against the domain it indexes, and why no table in the corpus
    can show it -- they are all too small. This one is 19,216 bytes (st_size % 64
    is still 16) so 5000 * 3 fits, and a Sym stored in btree[] is twelve bits.
    """
    b = base()
    b.extend(bytearray(64 * 300 - 64))
    b[22] = 0x88; b[23] = 0x13                 # 5000 symbols
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
  "symbol-oob        |symbol_oob"
  "negative-resize   |negative_resize"
  "block-shift       |block_shift"
  "base64-shift      |base64_shift"
  "btree-past-end    |btree_past_end"
  "symlen-past-domain|symlen_past_domain"
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

echo "malformed: 7 crafted headers against $(basename "$EXE")"
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

# ------------------------------------------------------------ the control
#
# A gate that only ever asks for the word "Corrupt" is passed by an engine that
# prints it unconditionally. This is the row that stops that, and it is the reason
# the fixtures above can be trusted at all.
n=0
for f in "$CORPUS"/*.rtbw "$CORPUS"/*.rtbz; do [ -s "$f" ] && n=$((n + 1)); done
if [ "$n" -ne 10 ]; then
    echo "  clean-corpus        SKIPPED -- $CORPUS has $n/10 files (./build.sh tb-fetch)"
    echo
    echo "malformed: $PASS refused, $FAIL not refused, control SKIPPED"
    echo "A skipped control proves nothing: the refusals above are unguarded." >&2
    [ "$FAIL" = "0" ] || exit 1
    exit 2
fi

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

echo
echo "malformed: $PASS passed, $FAIL failed"
[ "$FAIL" = "0" ] || exit 1
exit 0
