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

Estimated **~2580 Elo** on the CCRL 40/40 scale, measured 16 August 2026 over
1000 games against five anchors. Treat the uncertainty as roughly ±50, not the
±12 the fit reports; see below.

| Date | Rating | fit ± | Games | Anchors | Notes |
|------|--------|-------|-------|---------|-------|
| 2026-08-16 | **2582** | ±12 | 1000 | 5 | First run with anchors below the engine |
| 2026-08-14 | 2503 | ±55 | 240 | 2 | Correction history, mobility counts captures |
| 2026-08-14 | 2523 | ±53 | 240 | 2 | Evaluation coverage batch |
| 2026-08-13 | 2473 | ±60 | 224 | 2 | Correctness batch |
| (earlier) | 2413 | ±82 | 150 | 2 | Prior baseline |

The jump from ~2500 to ~2580 is mostly a change of instrument, not of engine.
Every earlier row was fitted against Fruit and Glaurung alone, both ~200 Elo
*above* haVoc, where the model extrapolates; this run adds three anchors at or
below it, so the fit interpolates. The anchor binaries were also rebuilt from
source for this run, and are not the same builds that produced the older rows.
Do not read the column as a trajectory.

Per-anchor results, which are more informative than the pooled number:

| anchor | published | haVoc score | implies |
|---|---|---|---|
| Zurichess Fribourg | 2412 | 69.5% | 2555 |
| Arasan 12.2 | 2505 | 69.2% | 2646 |
| Phalanx XXIV | 2521 | 55.2% | 2558 |
| Fruit 2.1 | 2694 | 34.5% | 2583 |
| Glaurung 2.2 | 2793 | 21.5% | 2568 |

Four of the five agree closely (2555–2583). Arasan is an outlier: haVoc scores
69.2% against it but only 55.2% against Phalanx, whose published rating is 16
points *higher* — a ~100 Elo discrepancy between two engines rated the same.
One of those published ratings does not describe the engine as built here, and
dropping the anchor that disagrees would be choosing the answer, so the pooled
figure keeps it and the spread is reported instead. Excluding Arasan the fit
gives 2566 ± 13.

Two further things limit the number, and they are worth stating plainly:

- **The fit's ± is statistical only.** It describes how well the model fits
  these games. It says nothing about measuring at blitz against ratings
  established at 40/40, and older engines tend to look relatively stronger at
  long time control, so a systematic offset sits on top of it.
- **A single strength scale is an approximation.** The Arasan and Phalanx
  results above cannot both be right under one scale, which is what a
  non-transitive matchup looks like when a one-parameter model is asked to
  absorb it.

The per-change SPRTs remain the trustworthy measurements; this table is a
periodic check that the engine has not drifted away from the field.

The method is a maximum-likelihood fit of the logistic Elo model with opponent
ratings held fixed at their published CCRL 40/40 values, at 20+0.2 on one thread
with `OwnBook` and `Ponder` forced off. It is an estimate *on* the CCRL scale,
not a CCRL rating. The anchors are built by
[`scripts/testing/fetch-anchors.sh`](scripts/testing/fetch-anchors.sh), which
pins each version; the harness and the rules for choosing anchors are in
[`scripts/testing/`](scripts/testing/README.md).

To reproduce a rating from a gauntlet log:

```sh
scripts/testing/anchor_rating.py testruns/gauntlet-<tag>.log
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
- Null move pruning, reverse futility pruning, and ProbCut
- Singular extensions
- Internal iterative reductions
- Killer moves, countermoves, butterfly history, capture history, and
  continuation history for move ordering
- Correction history on the static evaluation
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

Run the test suite (175 tests, including perft against the five standard
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

- **The evaluation is hand-set, and Texel tuning is not the way out of that.** A
  parameter's gradient is independent of its current value, so the fit is
  unmoved by better starting points, and the error is dominated by material
  while the quiet filter discards exactly the positions where tactical terms
  fire. The fit it converges to measured +1.4 ± 19.9 Elo over 781 games. Useful
  for material and piece-square tables, and little else.
- **The search is capped at 64 plies** (`MAX_PLY`). Search and quiescence stop
  cleanly at the bound, but the cap is low: ten seconds already reaches ply 29.
- **Several depth-indexed constants are calibrated to a bug that no longer
  exists.** The reduction and futility formulas, null-move reduction and
  reverse-futility margins were tuned while the depth counter ran at roughly
  twice the real remaining depth, and none has been retuned since.
- **The transposition key mixes in the fifty-move and half-move counters**,
  which blocks transpositions between identical positions reached at different
  plies. That looks like a mistake, but removing it measured ~20% more nodes, so
  it stays until something better replaces it.
- **Syzygy probing and Polyglot books are stubs**, although both options are
  advertised and parsed.
- **The search is not saturated:** 29% of positions change their preferred move
  at depth 8, and 12% are still changing at depth 12, so nodes remain valuable.

The direction from here is a learned, CPU-side evaluation rather than further
hand-tuning. The reasoning, the alternatives that were ruled out and the
measured dead ends are in
[`docs/neural-direction.md`](docs/neural-direction.md) and
[`docs/roadmap.md`](docs/roadmap.md).

## License

[MIT](LICENSE)
