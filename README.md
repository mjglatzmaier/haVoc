# haVoc

A UCI chess engine written in C++20.

haVoc is a hobby project that has been worked on on and off over several years.
It uses bitboard move generation with magic bitboards, an alpha-beta search, and
a hand-written evaluation function. It has not been rated against other engines,
so no strength claims are made here.

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
haVoc v2.0.0
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

Run the test suite (49 tests, including perft against the five standard
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

- The evaluation terms are largely hand-set. Texel tuning infrastructure exists
  but has not produced a well-tuned parameter set yet.
- The static exchange evaluation has known correctness problems and is being
  reworked.
- The search is capped at 64 plies (`MAX_PLY`).
- Syzygy tablebase probing and Polyglot opening books are stubs.
- No Elo measurement has been done, so there is no baseline to regress against.

Possible future work, in rough order of interest: finishing evaluation tuning,
replacing the hand-written evaluation with a learned one, Syzygy probing, and
Chess960 support.

## License

[MIT](LICENSE)
