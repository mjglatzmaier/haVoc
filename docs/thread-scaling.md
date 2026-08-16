# Thread scaling: preliminary profiling

Roadmap Tier 1 item 2 (`docs/neural-direction.md` §10) asks for nps *and*
strength to be measured against thread count "before assuming it scales". This
is a first pass at that. It is **profiling, not a verdict**: the sample is small
relative to the effects, and the confounds listed at the bottom are not yet
controlled. Nothing here establishes that Lazy SMP is implemented incorrectly.

Machine: i9-13900KS, 8 P-cores (16 hardware threads) + 16 E-cores, 24 cores /
32 threads total. All runs `-DHAVOC_NATIVE=ON`, Hash 1024 MB.

## Depth reached at fixed time

14 positions spanning openings, middlegames and endgames, `go movetime 1500`,
2 repetitions, mean of the deepest completed iteration. Stagger span 4.

| threads | mean depth | gain over 1 thread |
| --- | --- | --- |
| 1 | 20.714 | -- |
| 2 | 20.821 | +0.11 |
| 4 | 21.286 | +0.57 |
| 8 | 21.357 | +0.64 |
| 16 | 21.429 | +0.71 |
| 24 | 21.286 | +0.57 |
| 32 | 21.107 | +0.39 |

Read this as a shape, not as seven reliable numbers: independent repetitions of
the same configuration differed by up to 0.25 ply, which is comparable to most
of the differences in the table. The shape that does survive that spread is
that the curve flattens early and does not keep climbing.

## Raw throughput

| threads | nps | vs 1 thread |
| --- | --- | --- |
| 1 | 1.74 M | 1.0x |
| 8 | 10.8 M | 6.2x |
| 16 | 17.0 M | 9.8x |
| 24 | 19.2 M | 11.0x |
| 32 | 16.5 M | 9.5x |

**Aggregate nps is not parallel speedup and must not be read as one.** Sixteen
threads duplicating each other's work produce excellent aggregate nps and no
depth at all. The number is here only to establish that the threads are running
and doing work, not to quantify what that work is worth.

## What is and is not established

Established:

- The curve flattens by 4 to 8 threads and does not improve past 16.
- 32 threads is not better than 16 on this machine.

Not established, and specifically **not** claimed:

- That helper threads "contribute nothing". They do not need to finish an
  iteration to be useful; the transposition-table entries they write during an
  unfinished iteration are exactly how Lazy SMP transmits work. Their results
  are also consumed directly at `src/search.cpp` in the collection loop after
  `wait_finished()`.
- That the observed gain is abnormal. There is no universal expected ply gain
  for a correct Lazy SMP; it depends on the effective branching factor, the
  same-depth speedup, and how much duplicated work the threads do. Deriving the
  engine's own single-thread node growth per ply, and comparing that against
  measured same-depth speedup, is the prerequisite for calling any number
  disappointing. That has not been done.

Time-to-depth at fixed depth was also measured and is too noisy to use: 16
threads gave 4.49x in one run and 3.35x in another, because time-to-depth under
Lazy SMP is a max over randomised threads.

## Confounds not yet controlled

- **Hybrid cores.** The 13900KS mixes P-cores, E-cores and SMT siblings. The
  24-to-32 regression may be SMT or scheduler placement rather than anything in
  the search. Experiments should be pinned to core classes before concluding.
- **Sample size.** 14 positions repeated twice is not 28 independent samples.
  Detecting 0.2 ply reliably needs on the order of 50-200 independent paired
  positions; detecting 0.1 ply, several hundred.
- **No interleaving.** Configurations were run in blocks, so thermal and
  frequency drift is aliased onto the thread count.
- **TT pressure.** A 1 GB table under 32 threads of random access was not
  varied, and `hashfull` and miss rates were not recorded.

## Practical guidance for now

- **Do not assume search threads scale datagen.** N independent single-threaded
  processes scale linearly and are the safe way to fill a datagen corpus; that
  is unaffected by anything in this document.
- The useful range measured here is 4 to 16 threads, and most of it is already
  won by 4.
- Judge future SMP work against depth-at-fixed-time with a properly powered
  sample, not against aggregate nps.

## The stagger bug fixed alongside this profiling

Helper threads began iterative deepening at `1 + thread_id`, unbounded. A
time-based search passes `MAX_PLY` (64) as its target, so on a 32-thread search
the upper half of the threads began with a first iteration of depth 17 to 32
with no iterative-deepening warmup behind it. Whatever those searches are
worth, they are not what the stagger was meant to produce.

The offset now wraps modulo `smp_stagger_span` (default 4), so every helper
starts somewhere it can reach while still not walking an identical path to its
neighbours.

| threads | unbounded (old) | span 2 | span 4 | span 8 |
| --- | --- | --- | --- | --- |
| 16 | 21.286 | 21.393 | 21.250 | 21.286 |
| 32 | 20.536 | 21.071 | 20.857 | 20.893 |

At 16 threads the spans are indistinguishable, including from the old
behaviour. At 32 threads every bounded span beats unbounded, by +0.32 ply for
the shipped default and +0.54 for span 2. The span itself is therefore a
heuristic: what the data supports is that *some* bound beats none at 32
threads, not that 4 is the best value. Span 2 won this table but by too little
to justify shipping it on this evidence.

This is a robustness fix that removes a pathology, not a source of Elo. It is
bench-identical because thread 0 is unaffected either way.
