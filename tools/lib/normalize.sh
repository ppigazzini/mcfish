#!/usr/bin/env bash
# The ONE definition of the golden-diff normalization, sourced by everything that
# needs it: `./build.sh golden` and `golden-update` on one side,
# tools/upstream_golden_audit.sh on the other.
#
# It lives here because the audit used to lift it out of build.sh with a `sed` keyed
# on one anchored line -- a TEXT dependency between two files, which is a hack the
# build having no importable unit forced. Restating it in the audit instead would be
# worse: the two copies would drift exactly when it matters, which is when a gap
# closes and its line must STOP being dropped. One file, two sourcing callers, no
# extraction.
#
# Sourced, never executed: it defines a function and does nothing else.

normalize() {
  # Elide what is volatile, and DROP what is a declared gap -- never both silently.
  #
  # The identity banner carries a version and a git sha, so it differs between
  # mcfish and the oracle by construction and cannot be compared; it is replaced,
  # not removed, so its ABSENCE is still a diff.
  #
  # The dropped lines below are upstream output mcfish does not yet produce because
  # the corresponding subsystem is unwired. Each is a GAP, and this filter is the
  # only thing keeping it out of the goldens -- so when the subsystem lands, delete
  # its line here FIRST and let the gate go red. A filter that outlives its gap
  # silently stops comparing real output.
  # "Network replica N" now keeps its LINE and loses only its BACKING word, where it
  # used to be dropped whole. mcfish emits the line -- the count and the shape are
  # upstream's -- but reports `Local memory.` against upstream's `Shared memory.`,
  # because it holds one network in ordinary process memory and has no system-wide
  # mapping to name. Eliding the word compares what is comparable: that the line is
  # emitted, in the right place, once per replica. Dropping the line compared
  # nothing, and hid its absence for as long as it was absent.
  #
  # "Available processors" and the NUMA binding suffix keep their LINE and lose their
  # VALUE. The line is behaviour -- mcfish must emit it, on `go`, in upstream's place --
  # and the goldens assert that. The value is the machine: this box reports 0-15 and a
  # 4-CPU runner reports 0-3, so pinning it makes every golden fail off the developer's
  # hardware. That is what `nps` and `time` are elided for, and this belongs with them.
  #
  # "Using N thread" is NOT machine-decided -- it echoes the Threads option -- so it
  # stays pinned in full.
  #
  # `compiler`'s two IDENTITY lines keep their LINE and lose their VALUE, for the same
  # reason the banner is replaced rather than removed: clang built this and g++ built
  # the oracle, so those two can never be equal and comparing them would only ever
  # report the toolchain. The two lines that carry BEHAVIOUR -- the architecture and the
  # settings list -- are compared in full, which is the whole point of that block.
  #
  # `upstream-transcript` does NOT elide any of these: it compares two engines on one
  # machine at one moment, where the real values must agree.
  sed -E 's/ nps [0-9]+//; s/ time [0-9]+//; s/^Total time \(ms\) *: [0-9]+$/Total time (ms) : <elided>/; s/^Nodes\/second *: [0-9]+$/Nodes\/second    : <elided>/' \
    | sed -E 's/^(mcfish|Stockfish) [^ ]+ by .*/<engine banner>/' \
    | sed -E 's/^(Compiled by                : ).*/\1<toolchain>/' \
    | sed -E 's/^(Compiler __VERSION__ macro : ).*/\1<toolchain version>/' \
    | sed -E 's/^info string Available processors: .*/info string Available processors: <cpus>/' \
    | sed -E 's/ with NUMA node thread binding: .*/ with NUMA node thread binding: <binding>/' \
    | sed -E 's/^(info string Network replica [0-9]+: ).*/\1<backing>/' \
    | tr -d '\r'
}
