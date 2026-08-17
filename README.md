# haVoc

[![CI](https://github.com/mjglatzmaier/haVoc/actions/workflows/ci.yml/badge.svg?branch=main)](https://github.com/mjglatzmaier/haVoc/actions/workflows/ci.yml)
[![C++20](https://img.shields.io/badge/C%2B%2B-20-blue.svg)](https://en.cppreference.com/w/cpp/20)
[![Platforms](https://img.shields.io/badge/platforms-Linux%20%7C%20macOS%20%7C%20Windows-lightgrey.svg)](https://github.com/mjglatzmaier/haVoc/actions/workflows/ci.yml)
[![License](https://img.shields.io/github/license/mjglatzmaier/haVoc.svg)](LICENSE)

A UCI chess engine in C++20: magic bitboards, alpha-beta search, and — since
v2.3 — an NNUE evaluation trained on the engine's own self-play.

The hand-written evaluation it started with is still in the tree and still works
(`EvalFile none` selects it). It is kept deliberately: the network was
bootstrapped from it, and an engine that cannot play without its network has no
way to show where its network came from.

## Strength

| Version | Evaluation | Rating | Games | Measured |
|---|---|---|---|---|
| v2.3 | NNUE, L1=256 | **2834** ±12 | 1200 | 2026-08-17 |
| v2.2 | handcrafted | **2582** ±12 | 1000 | 2026-08-16 |

Estimated on the CCRL 40/40 scale against six anchors spanning 2412–2985.
Treat the uncertainty as roughly ±50 rather than the ±12 the fit reports — it
is statistical only, and the games are played at blitz against ratings
established at 40/40. The per-change SPRTs are the trustworthy measurements;
this table is a periodic check that the engine has not drifted away from the
field. Method, per-anchor results and superseded measurements are in
[`docs/ratings.md`](docs/ratings.md).

## Building

Needs a C++20 compiler (GCC 12+, Clang 15+, AppleClang 16+, MSVC 2022+) and
CMake 3.20+.

```sh
git clone https://github.com/mjglatzmaier/haVoc.git
cd haVoc
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel
scripts/fetch-net.sh          # download the evaluation network, ~20 MB
```

The binary is written to `build/havoc`. Run the tests with:

```sh
ctest --test-dir build --output-on-failure
```

| CMake option | Default | Description |
|--------|---------|-------------|
| `HAVOC_ENABLE_TESTS` | `ON` | Build the unit tests (GoogleTest is fetched automatically) |
| `HAVOC_ENABLE_BENCH` | `OFF` | Build the microbenchmark targets |
| `HAVOC_ENABLE_SANITIZERS` | `OFF` | Enable ASan/UBSan in Debug builds |
| `HAVOC_WERROR` | `OFF` | Treat compiler warnings as errors (CI enables this) |
| `HAVOC_NATIVE` | `ON` | Compile with `-march=native`; turn off to copy the binary to another machine |
| `HAVOC_SYZYGY` | `ON` | Build Syzygy tablebase probing |
| `HAVOC_BUILD_TOOLS` | `OFF` | Build the tuning tools (`havoc_datagen`, `havoc_texel`, `havoc_pgn2epd`) |

## The network

haVoc plays roughly 160 Elo stronger with its network than without, so getting
it loaded matters more than any other setup step.

Networks are not kept in git — one ~20 MB blob per training iteration would make
the repository permanently expensive to clone — so the repository commits the
hash and the provenance, and the release page serves the bytes:

```
$ scripts/fetch-net.sh
fetch-net: verified nets/havoc-69c7d05e4298.nnue
```

The script checks the download against the SHA256 in `nets/default.txt` and
refuses a file that does not match. The engine then finds it automatically on
startup, searching in this order:

1. `EvalFile`, if you set it
2. `$HAVOC_NET_DIR`
3. beside the binary, then `nets/` beside the binary
4. the current directory, then `nets/` under it — where `fetch-net.sh` installs
5. `~/.local/share/havoc/`, or the platform equivalent

So the usual case needs no configuration. To point at a specific file, or to
select the handcrafted evaluation:

```
setoption name EvalFile value /path/to/havoc-69c7d05e4298.nnue
setoption name EvalFile value none
```

The startup banner and the `bench` line both name the evaluation actually in
use, so the two are never silently confused. `nets/*.provenance.json` records,
for each network, its architecture, the corpus it was trained on, the engine
commit that labelled that corpus, and what it measured.

## Running

haVoc speaks the [UCI protocol](https://www.chessprogramming.org/UCI) and works
with any UCI GUI (Arena, Cute Chess, Banksia). It can also be driven directly:

```
$ ./build/havoc
haVoc v2.3.0
by M.Glatzmaier
info string Loaded network from nets/havoc-69c7d05e4298.nnue
uci
position startpos moves e2e4
go depth 12
```

```
uci / isready                  Handshake and synchronisation
position startpos moves e2e4   Set up a position
position fen <fen> moves ...   Set up a position from FEN
go depth 15                    Search to a fixed depth
go movetime 5000               Search for a fixed number of milliseconds
go wtime 60000 btime 60000     Search under a time control
go infinite / stop             Search until stopped
d                              Print the board, hash key, and FEN
bench [depth]                  Search 12 fixed positions (default depth 10)
quit                           Exit
```

`bench` is a quick regression check: it reports total nodes and nodes per second
over the same positions every time, so two builds can be compared directly.
Comparing across evaluations is meaningless, which is why the line names it.

| Option | Type | Default | Notes |
|--------|------|---------|-------|
| `Threads` | spin, 1–1024 | 1 | Search threads (Lazy SMP) |
| `Hash` | spin, 1–33554432 | 1024 | Transposition table size in MB |
| `EvalFile` | string | auto | Network path, or `none` for the handcrafted evaluation |
| `SyzygyPath` | string | empty | Directory of Syzygy tablebases, up to 6 pieces |
| `ParamFile` | string | empty | Load search/evaluation parameters from a file |
| `BookFile` | string | empty | Accepted but inert; `src/book.cpp` is a placeholder |

Syzygy probing is WDL-only and happens inside the search; resolved positions are
counted in the `tbhits` field of every `info` line. It is skipped when the
position has castling rights or a non-zero fifty-move counter, because the
tables model neither. Without DTZ the engine knows a position is won but not how
far off the win is, so it substitutes a preference for winning sooner.

## How it works

**Search.** Iterative deepening with aspiration windows around a
principal-variation search, a 4-entry-cluster transposition table, late move
reductions, null move pruning, reverse futility pruning, ProbCut, singular and
double extensions, internal iterative reductions, and a quiescence search with
delta pruning and SEE. Move ordering uses killers, countermoves, butterfly,
capture and continuation history; correction history adjusts the static
evaluation. Multi-threading is Lazy SMP over a shared transposition table.

**Evaluation.** A HalfKP network, 40960→256×2→32→32→1, quantised to int8/int16
and run with AVX2. It was trained on ~29M positions from the engine's own
self-play, labelled by the previous network, with WDL blended into the search
score. Both evaluators sit behind one `IEvaluator` interface, so the search is
unaware of which is in use. Training and data-generation details are in
[`docs/nnue-data.md`](docs/nnue-data.md) and
[`docs/nnue-integration.md`](docs/nnue-integration.md).

## Known gaps

- **The search is capped at 64 plies** (`MAX_PLY`). Ten seconds already reaches
  ply 29.
- **The search constants are tuned for the network, not for the handcrafted
  evaluation.** Several are margins compared against a static evaluation in
  centipawns, and the two evaluators do not produce centipawns with the same
  distribution, so `EvalFile none` now runs on margins calibrated for something
  else — it searches ~45% more nodes for the same bench and measures -16.5 Elo
  against its own older settings, while the network gains +10.4. The handcrafted
  evaluation is kept as provenance rather than as a playing configuration; for
  a handcrafted-only engine, take the parameters from commit `158e544`.
- **Search parameters are tuned below their own resolution.** SPSA at 32 games
  an iteration cannot resolve anything below ~8–10 Elo per parameter, which is
  why recent tunes move one parameter and leave the rest untouched. Fixed-node
  rather than fixed-time games would remove the timing variance; not done yet.
- **Several depth-indexed constants are calibrated to a bug that no longer
  exists** — they were tuned while the depth counter ran at roughly twice the
  real remaining depth. Retuning them hits the resolution floor above.
- **The transposition key mixes in the fifty-move and half-move counters**,
  blocking transpositions between identical positions reached at different
  plies. That looks like a mistake, but removing it measured ~20% more nodes, so
  it stays until something better replaces it.
- **The network's width is not obviously right.** L1=256 was chosen by
  measurement: at 512 and 1024 the network predicts its training labels *better*
  and still loses badly, because the feature transformer outgrows the 36 MB L3
  cache (21 MB at 256, 42 MB at 512, 84 MB at 1024). That is an implementation
  limit, not a fact about chess — sparse propagation, AVX-VNNI and lazy
  accumulator updates are all unimplemented, and each would move the trade.
- **Texel tuning was a dead end**, left in the tree as history: its fit measured
  +1.4 ± 19.9 Elo over 781 games.
- **Polyglot book support is a stub.**

Where the remaining strength is thought to be, and what has already been ruled
out, is in [`docs/roadmap.md`](docs/roadmap.md) and
[`docs/neural-direction.md`](docs/neural-direction.md).

## License

[MIT](LICENSE)
