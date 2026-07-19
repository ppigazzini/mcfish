# NNUE throughput: where the instructions actually go

Measured, not guessed. Every earlier version of this note named a single culprit
and was wrong the moment it was fixed, so this one records the method and the
whole distribution instead.

## How to take the measurement

Instruction counts, never wall clock. This host has read the SAME binary at
511k and 581k nps in two batches, and 46k next to 151k under load; callgrind
resolves 0.01% and does not care what else is running.

**Hold the compiler and the ISA constant, or the number is about neither engine.**
The oracle that `upstream_oracle.sh` builds uses gcc, which is correct for node
counts and wrong for any cost ratio. Build a clang oracle for perf work:

```sh
cd ../.mcfish-upstream-oracle/src
make clean && make -j4 build ARCH=x86-64-sse41-popcnt COMP=clang EXE=sf_clang
./sf_clang bench          # must print the anchor before you trust anything
```

and build mcfish at the matching tier with `MCFISH_ARCH=sse41`. Then subtract
startup from both: the ~90 MB net parse and the magic init are around 40% of a
shallow profile, and leaving them in flattens every ratio toward 1.

```sh
valgrind --tool=callgrind --callgrind-out-file=cc.out ./mcfish bench 16 1 8
valgrind --tool=callgrind --callgrind-out-file=up.out ./sf_clang bench 16 1 8
python3 ../zfish/tools/perf_fingerprint.py costs cc.out
```

Read costs by SUMMING a function across origin files — callgrind emits one entry
per (origin-file, function) pair, and reading a single line per side once turned a
real 0.99x parity into a reported "1.87x, the worst component".

## Where it stands

Both engines built at their own native tier, which on this host is AVX-512 VNNI
for each, node counts asserted equal by `nps_ab.sh`, machine idle, ten rounds:

    mcfish / zfish = 1.014 and 1.006 across two independent runs

That is parity. Take it as parity and not as "mcfish is faster" -- 0.6-1.4% is
inside what this method resolves, and the tool says so itself.

The number is load-sensitive in a way worth remembering: the same two binaries
read 0.749 at load ~2.2 and 1.014 at load ~0.8. `nps_ab.sh` pins both to core 0,
so anything else scheduled there lands in the measurement. A run taken while
another agent or build is active is not evidence.

Against a clang-built upstream oracle at a matched SSE4.1 tier, by callgrind with
startup subtracted: **1.154x** instructions. That is the honest remaining gap and
the one to work on, because it is deterministic and does not care about load.

## The distribution

Search instructions, startup subtracted, mcfish against the clang oracle at
SSE4.1: **1.154x** with LTO (1.242x before it). The table below was taken at
1.242x; LTO folded the cross-TU helpers into their callers and moved every row,
but the shape -- accumulator worst, move picking at parity, `do_move` ahead --
did not change.

| path | mcfish | oracle | ratio |
| --- | ---: | ---: | ---: |
| accumulator update (`apply_combined` + `acc_rows_i16` + `apply_psqt_delta_in_place` + `nnue_full_append_changed` + `nnue_bb_pieces_of_exact`) | 872M | 601M | **1.45** |
| affine (`nnue_affine_32` vs `propagate`) | 398M | 316M | 1.26 |
| transform + side eval (`nnue_transform_bucket` + `evaluate_side`) | 247M | 182M | 1.35 |
| threat update | 66M | 48M | 1.38 |
| move picking (`score_list` + `movepick_next` vs `next_move`) | 128M | 129M | 1.00 |
| `do_move` | 36M | 43M | 0.83 |

**The accumulator update is the largest gap in absolute instructions, not the
affine.** Move picking is already at parity and `do_move` is ahead; neither is
worth touching.

## What has already been done here

- The affine folds each output's four sublanes inside the multiply instead of
  widening to int32 first. Cut search instructions per node from 26442 to 14074.
- The dot kernel is tiered: `vpdpbusd` on AVX-512 VNNI, `vpmaddubsw`/`vpmaddwd` on
  AVX2, `pmaddubsw`/`pmaddwd` on SSSE3, a lane loop otherwise.
- Accumulator chains scale with the tier. Three chains on a narrow tier cost
  8 chunks x 3 = 24 registers against a file of 16 and measured +4.2%; the
  latency they hide is worth less than the spill they cause.
- `min`/`max` use the elementwise builtin where the compiler has one.
- The NNZ mask is branchless; both ClippedReLUs store their eight lanes as a unit.

## The constraint on any change here

`./build.sh signature` must not move, and `./build.sh simd-scalar` and
`./build.sh arch-determinism` must both stay green — they are what prove the
vector and scalar bodies, and every ISA tier, still agree. Integer accumulation is
exact int32, so reassociating adds is safe; changing a width, a shift, a clamp
bound or the fold order is not.
