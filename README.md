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

Estimated **~2503 Elo** on the CCRL 40/40 scale (95% CI ±55), measured
14 August 2026.

| Date | Rating | 95% CI | Games | Notes |
|------|--------|--------|-------|-------|
| 2026-08-14 | **2503** | ±55 | 240 | Correction history, mobility counts captures, storm advancement |
| 2026-08-14 | 2523 | ±53 | 240 | Evaluation coverage batch |
| 2026-08-13 | 2473 | ±60 | 224 | Correctness batch |
| (earlier) | 2413 | ±82 | 150 | Prior baseline |

Read this as "somewhere around 2500 since 13 August" rather than as a trajectory.
Three things limit it, and they are worth stating plainly:

- **240 games cannot resolve 50 Elo.** The interval is ±55, so two runs differing
  by 20 are one run.
- **Self-play overstates field Elo.** A change is measured against an opponent
  that shares all of its blind spots, so gains confined to positions both sides
  misplay do not transfer to the field.
- **The anchors disagree with each other** by more than the stated interval: this
  run implies 2536 from Fruit and 2448 from Glaurung, and the previous run
  implied 2491 and 2561 — the same two engines, opposite directions. A fit
  assuming a single strength scale is being asked to reconcile a non-transitive
  matchup, so the real uncertainty is wider than the statistical one.

The per-change SPRTs are the trustworthy measurements; this table is a periodic
check that the engine has not drifted away from the field.

These rows were measured against Fruit 2.1 and Glaurung 2.2 alone, both ~200
points above the engine, where a fit is extrapolating rather than interpolating.
Three bracketing anchors have since been added — Zurichess Fribourg (2412),
Arasan 12.2 (2505) and Phalanx XXIV (2521) — so later rows will be more reliable
but not directly comparable to these.

The method is a maximum-likelihood fit of the logistic Elo model with opponent
ratings held fixed at their published CCRL 40/40 values, at 20+0.2 on one
thread with `OwnBook` and `Ponder` forced off. It is an estimate *on* the CCRL
scale, not a CCRL rating. The scripts, the
opening book and the rules for choosing anchors are in
[`scripts/testing/`](scripts/testing/README.md).

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
