# haVoc

[![CI](https://github.com/mjglatzmaier/haVoc/actions/workflows/ci.yml/badge.svg?branch=main)](https://github.com/mjglatzmaier/haVoc/actions/workflows/ci.yml)
[![C++20](https://img.shields.io/badge/C%2B%2B-20-blue.svg)](https://en.cppreference.com/w/cpp/20)
[![Platforms](https://img.shields.io/badge/platforms-Linux%20%7C%20macOS%20%7C%20Windows-lightgrey.svg)](https://github.com/mjglatzmaier/haVoc/actions/workflows/ci.yml)
[![License](https://img.shields.io/github/license/mjglatzmaier/haVoc.svg)](LICENSE)

A UCI chess engine written in C++20.

haVoc is a hobby chess engine I've developed on and off for several years.
It uses bitboard move generation with magic bitboards, an alpha-beta search, and
a hand-written evaluation function.

## Strength

Estimated **~2503 Elo** (CCRL Blitz scale, 95% CI ±55), measured 14 August 2026.

| Date | Rating | 95% CI | Games | Notes |
|------|--------|--------|-------|-------|
| 2026-08-14 | **2503** | ±55 | 240 | Correction history, mobility counts captures, storm advancement. Self-play said +52; the gauntlet did not see it — see below |
| 2026-08-14 | 2523 | ±53 | 240 | Eval coverage batch: connected pawns, queen mobility, tactical motifs, rook files, outposts, passer rank coverage, backward pawns |
| 2026-08-13 | 2473 | ±60 | 224 | Correctness batch: zobrist/material keys, phase interpolation, aspiration window, king safety |
| (earlier) | 2413 | ±82 | 150 | Prior baseline, same anchors and hardware |

The last two rows are the honest limit of this method. Between them the engine
gained a measured +30.6 ± 12, +16.7 ± 11 and +4.9 ± 14 in self-play SPRTs
against its own previous revision, and the gauntlet reports 2523 then 2503 —
a change of the opposite sign, comfortably inside either interval. Both things
can be true, and at least one certainly is:

- **Self-play overstates field Elo.** A change is measured against an opponent
  that shares every one of its blind spots, so an improvement that only matters
  in positions both sides misplay collects points that no external opponent
  will concede. This is a well-known effect and there is no reason to think
  this engine is exempt.
- **240 games cannot resolve 50 Elo.** The interval is ±55. Two runs that
  differ by 20 are one run.

The two anchors also disagree with each other by more than the stated interval:
this run implies 2536 from Fruit and 2448 from Glaurung, and the previous run
implied 2491 and 2561 — the same two engines, opposite directions. A fit that
assumes a single strength scale is being asked to reconcile a non-transitive
matchup, so the true uncertainty is wider than the statistical one.

Read the table as "somewhere around 2500 since 13 August" and not as a
trajectory. The SPRT numbers are the trustworthy measurements of individual
changes; this table is a periodic sanity check that the engine has not drifted
away from the field.

### Method

The rating is a maximum-likelihood fit of the standard logistic Elo model. The
opponents' ratings are held fixed at their published values and haVoc's rating
is the value of `r` solving

```
sum_i N_i * E(r - R_i) = sum_i S_i        where  E(d) = 1 / (1 + 10^(-d/400))
```

Draws score a half point, which is what the Elo model assumes; no separate draw
parameter is fitted.

- **Anchors.** Fruit 2.1 (2694) and Glaurung 2.2 (2793), CCRL 40/15 single-CPU
  64-bit entries, list dated 7 August 2026. Both have very large CCRL sample
  sizes and decades of scrutiny, so their published ratings carry small error
  bars. Obscure hobby engines were tried as anchors and discarded: two of them
  produced implied ratings 223 points apart for the same candidate, which is
  what a lumpy evaluation surface and a thinly-played published rating do to a
  fit that assumes a single strength scale.

  Both anchors sit ~200 points *above* haVoc, where it scores about 22%. A
  score that lopsided carries little information per game, and with no anchor
  below the engine the fit is extrapolating rather than interpolating — which
  is a large part of why two anchors can disagree by 90 points. Three
  bracketing anchors have since been added for future runs: Zurichess Fribourg
  (2412), Arasan 12.2 (2505) and Phalanx XXIV (2521). Rows above this note were
  measured against the original two only, so they remain mutually comparable
  but are not comparable to later rows.
- **Conditions.** 20+0.2 on one thread with a 64 MB hash, `OwnBook=false` and
  `Ponder=false` forced on every engine, from a fixed opening book with colours
  reversed on each pair. All engines run on the same machine at the same time
  control.
- **Caveats.** This is an *estimate on the CCRL scale*, not a CCRL rating: it
  comes from a two-opponent gauntlet on one machine, not from CCRL's own pool.
  The confidence interval is statistical only and does not cover anchor error
  or the non-transitivity of engine matchups. Treat the trend across rows as
  more meaningful than any single absolute figure. Consecutive rows here have
  overlapping intervals, so a single step is suggestive rather than proven;
  the rows are directly comparable because the anchors, hardware, book and time
  control are held fixed across them.

Every change that could plausibly affect playing strength is gated behind a
self-play SPRT against the previous revision before it is merged.

The scripts that produce every number in this section live in
[`scripts/testing/`](scripts/testing/README.md), together with the opening book
and the rules for choosing anchors.

To reproduce a rating from a gauntlet PGN:

```sh
python3 scripts/rate.py gauntlet.pgn --only fruit,glaurung
```

## Building

Requirements:

- A C++20 compiler (GCC 12+, Clang 15+, AppleClang 16+, or MSVC 2022+)
- CMake 3.20 or newer

```sh
git clone https://github.com/mjglatzmaier/haVoc.git
cd haVoc
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel
```

The engine binary is written to `build/havoc`.

### CMake options

| Option | Default | Description |
|--------|---------|-------------|
| `HAVOC_ENABLE_TESTS` | `ON` | Build the unit tests (GoogleTest is fetched automatically) |
| `HAVOC_ENABLE_BENCH` | `OFF` | Build the microbenchmark targets |
| `HAVOC_ENABLE_SANITIZERS` | `OFF` | Enable ASan/UBSan in Debug builds |
| `HAVOC_WERROR` | `OFF` | Treat compiler warnings as errors (CI enables this) |
| `HAVOC_NATIVE` | `ON` | Compile with `-march=native` |
| `HAVOC_BUILD_TOOLS` | `OFF` | Build the tuning tools (`havoc_datagen`, `havoc_texel`, `havoc_pgn2epd`) |

`HAVOC_NATIVE` produces a binary tuned for the machine it was built on. Turn it
off if you intend to copy the binary to another machine.

## Running

haVoc speaks the [UCI protocol](https://www.chessprogramming.org/UCI) and can be
used with any UCI-compatible GUI (Arena, Cute Chess, Banksia, and others). It can
also be driven directly from a terminal:

```
$ ./build/havoc
haVoc v2.1.0
by M.Glatzmaier
uci
position startpos moves e2e4
go depth 12
```

Commonly used commands:

```
uci                            Print engine name and options
isready                        Synchronisation check
position startpos moves e2e4   Set up a position
position fen <fen> moves ...   Set up a position from FEN
go depth 15                    Search to a fixed depth
go movetime 5000               Search for a fixed number of milliseconds
go wtime 60000 btime 60000     Search under a time control
go infinite                    Search until 'stop'
stop                           Stop the current search
d                              Print the board, hash key, and FEN
bench [depth]                  Search a fixed set of 12 positions (default depth 10)
quit                           Exit
```

`bench` is useful as a quick regression check: it reports total nodes and nodes
per second over the same positions every time, so two builds can be compared
directly.

### Options

| Option | Type | Default | Notes |
|--------|------|---------|-------|
| `Threads` | spin, 1–1024 | 1 | Number of search threads (Lazy SMP) |
| `Hash` | spin, 1–33554432 | 1024 | Transposition table size in MB |
| `ParamFile` | string | empty | Load evaluation parameters from a file |
| `SyzygyPath` | string | empty | Accepted but currently inert (see below) |
| `BookFile` | string | empty | Accepted but currently inert (see below) |

`SyzygyPath` and `BookFile` are advertised and parsed, but `src/tablebase.cpp`
and `src/book.cpp` are placeholders that always report "not available". Setting
them has no effect on play.

## How it works

### Search

Iterative deepening with aspiration windows around a principal-variation search.
The main heuristics in use are:

- Transposition table with 4-entry clusters, XOR key verification, and
  depth/age-based replacement
- Late move reductions, adjusted by history score and node type
- Null move pruning
- Reverse futility pruning
- Singular extensions
- Internal iterative reductions
- Killer moves, butterfly history, and countermoves for move ordering
- Quiescence search with delta pruning and static exchange evaluation

Multi-threaded search is Lazy SMP: each thread runs an independent search over a
shared transposition table, and the main thread reports the result.

### Evaluation

Evaluation is hand-written and goes through an `IEvaluator` interface, so an
alternative evaluator can be substituted without touching the search. The current
implementation covers:

- Material and piece-square tables, interpolated between middlegame and endgame
- Pawn structure (doubled, isolated, backward, and passed pawns)
- King safety from attacker counts, pawn shelter, and piece coordination
- Mobility, threats, and space
- Some specific endgame knowledge
- Per-thread pawn and material hash tables

## Development

Run the test suite (133 tests, including perft against the five standard
positions from the Chess Programming Wiki):

```sh
ctest --test-dir build --output-on-failure
```

Tuning tools live in `tools/` and are built with `-DHAVOC_BUILD_TOOLS=ON`:

- `havoc_datagen` — generate self-play training positions
- `havoc_texel` — Texel-style evaluation tuning over labelled positions
- `havoc_pgn2epd` — convert PGN games to EPD

`scripts/tune.sh` wraps the datagen/tune loop, and `scripts/bake_params.py`
writes a tuned parameter set back into the source.

Continuous integration runs the build and test suite on Linux, macOS, and
Windows: [ci.yml](https://github.com/mjglatzmaier/haVoc/actions/workflows/ci.yml).

## Current state and known gaps

Being honest about where things stand:

- The evaluation terms are largely hand-set. Texel tuning infrastructure exists,
  and has been shown *not* to be the answer: a parameter's gradient is
  independent of its current value, so the fit is unmoved by better starting
  points, and the error is dominated by material while the quiet filter discards
  the positions where tactical terms fire. The fit it converges to measured
  +1.4 ± 19.9 Elo over 781 games. Use it for material and piece-square tables
  only — see [`docs/roadmap.md`](docs/roadmap.md).
- The search is capped at 64 plies (`MAX_PLY`). Search and quiescence now stop
  cleanly at that bound rather than running past the end of the search stack,
  but the cap itself is low: a ten-second search already reaches ply 29, and a
  forty-second one reaches ply 40.
- Several depth-indexed constants (reduction and futility formulas, null-move
  reduction, razoring and reverse-futility margins) were tuned when a bug made
  the depth counter run at roughly twice the real remaining depth. They are all
  now operating at a different point than the one they were chosen for, and
  none of them has been retuned since.
- The transposition key deliberately mixes in the fifty-move and half-move
  counters, which blocks transpositions between identical positions reached at
  different plies. This looks wrong and was measured: removing it costs about
  20% more nodes, so it stays until something better replaces it.
- Syzygy tablebase probing and Polyglot opening books are stubs.
- Search is not saturated: 29% of positions change their preferred move at depth
  8 and 12% are still changing at depth 12, so nodes remain genuinely valuable
  and the search margins are the most promising thing left to tune.

Possible future work, in rough order of interest: SPSA over the search margins,
move ordering, replacing the hand-written evaluation with a learned one, Syzygy
probing, and Chess960 support. [`docs/roadmap.md`](docs/roadmap.md) has the
reasoning, the negative results, and the things not to retry.

## License

[MIT](LICENSE)
