# Thread scaling: measured

Roadmap Tier 1 item 2 (`docs/neural-direction.md` §10) asks for nps *and*
strength to be measured against thread count "before assuming it scales". This
is that measurement. The short version is that it does not scale: **16 threads
buy about seven tenths of a ply over one thread**, and past 16 threads the
engine gets weaker.

Machine: i9-13900KS, 8 P-cores (16 hardware threads) + 16 E-cores, 24 cores /
32 threads total, 93 GB RAM. All runs `-DHAVOC_NATIVE=ON`, Hash 1024 MB.

## Depth reached at fixed time

The measurement that matters, because it is what a game actually rewards. 14
positions spanning openings, middlegames and endgames, `go movetime 1500`, 2
repetitions, mean of the deepest completed iteration.

| threads | mean depth | gain over 1 thread |
| --- | --- | --- |
| 1 | 20.714 | -- |
| 2 | 20.821 | +0.11 |
| 4 | 21.286 | +0.57 |
| 8 | 21.357 | +0.64 |
| 16 | **21.429** | **+0.71** |
| 24 | 21.286 | +0.57 |
| 32 | 21.107 | +0.39 |

Scaling peaks at 16 threads and then goes backwards. Thirty-two threads is
worse than eight.

## Raw throughput

Raw nps scales roughly as expected, which is the important part of the
diagnosis: the threads really are running and really are searching.

| threads | nps | vs 1 thread |
| --- | --- | --- |
| 1 | 1.74 M | 1.0x |
| 2 | 3.35 M | 1.9x |
| 4 | 6.15 M | 3.5x |
| 8 | 10.8 M | 6.2x |
| 16 | 17.0 M | 9.8x |
| 24 | 19.2 M | 11.0x |
| 32 | 16.5 M | 9.5x |

## The gap is the finding

Eleven times the nodes per second converts into +0.7 ply. The parallel work is
being done and then wasted: it is not reaching the thread whose result is
returned. The transposition table *is* shared (a single `tt_` on
`SearchEngine`), so the sharing mechanism exists; what has not been established
is whether helper output survives in it under 16-32 threads of write pressure,
or whether the aspiration and staggering interact so that helpers spend their
time on iterations nobody consumes. That investigation is filed separately.

Time-to-depth at fixed depth was also measured and is extremely noisy -- 16
threads gave 4.49x in one run and 3.35x in another -- because time-to-depth
under Lazy SMP is a max over randomised threads. Depth-at-fixed-time over many
positions is the more stable statistic and is what the table above reports.

## What this changes

- **Do not use search threads to scale datagen.** N independent single-threaded
  processes scale linearly; N search threads deliver +0.7 ply at N=16. Datagen
  throughput should come from parallel games, not parallel search.
- **The useful range today is 4 to 16 threads**, and most of even that is won
  by 4.
- Any future SMP work should be judged against the depth-at-fixed-time table
  above, not against nps, which looks healthy and is measuring the wrong thing.

## The stagger bug fixed alongside this measurement

Helper threads began iterative deepening at `1 + thread_id`, unbounded. A
time-based search passes `MAX_PLY` (64) as its target, so on a 32-thread search
the upper half of the threads began with a first iteration of depth 17 to 32
with no iterative-deepening warmup behind it. Those are searches no move's time
budget can finish, so those cores produced nothing but transposition-table
traffic.

The offset now wraps modulo `smp_stagger_span` (default 4), so every helper
starts somewhere it can actually reach while still not walking an identical
path to its neighbours.

Measured, 14 positions, `go movetime 1500`, 2 repetitions, mean depth:

| threads | unbounded (old) | span 2 | span 4 | span 8 |
| --- | --- | --- | --- | --- |
| 16 | 21.286 | 21.393 | 21.250 | 21.286 |
| 32 | 20.536 | 21.071 | 20.857 | 20.893 |

Neutral at 16 threads, worth about half a ply at 32. This is a robustness fix
that removes a pathology rather than a source of Elo, and it is bench-identical
because thread 0 is unaffected either way.
