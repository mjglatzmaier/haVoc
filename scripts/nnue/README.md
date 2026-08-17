# NNUE training

The engine half of this lives in `include/havoc/nnue/` and `tools/`; this
directory is only the trainer. The plan and the reasoning behind the staging
are in [`docs/nnue-integration.md`](../../docs/nnue-integration.md) §8.

## The one rule

**Nothing here knows what a chess position is.** Feature indices are computed
in `include/havoc/nnue/features.hpp`, written into the training file by
`tools/nnue_export.cpp`, and read here as bare integers. Do not add a FEN
parser, a board representation, or an index calculation to this directory. A
second implementation of the feature encoding agrees with the first on almost
every position, trains cleanly against whichever one produced the data, and
surfaces only as an engine that is inexplicably weak — after the GPU time has
been spent.

The dataset carries `feature_set_version`; `dataset.py` refuses a file that
disagrees with the engine rather than training on it.

## Stage 0: the known-answer run

Stage 0 trains a network to reproduce haVoc's **own static evaluation**. The
target is a deterministic function we already possess, so the run tests the
whole pipeline and nothing about the idea. If it does not converge, the fault
is here, and finding that out costs hours rather than the days a real run
costs.

```sh
# Build the exporter (tools are off by default).
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DHAVOC_BUILD_TOOLS=ON
cmake --build build -j --target havoc_nnue_export

# Relabel an existing corpus with the HCE. No self-play needed: any
# collection of positions will do, since the label is computed here.
./build/havoc_nnue_export --input ~/havoc-data/selfplay-d8-s20240815.epd \
                          --output ~/havoc-data/stage0-hce.hbin --label hce

# Train. --expect-label makes it an error to point this at real labels by
# accident, which would turn a known-answer test into an unknown-answer one.
python3 scripts/nnue/train.py --data ~/havoc-data/stage0-hce.hbin \
                              --out /tmp/stage0.pt --expect-label hce_static
```

Read the result as a verdict on the pipeline:

| val MAE | means |
|---|---|
| under ~25 cp | the pipeline works; go to stage 1 |
| 50-150 cp | it is learning something but not the target — suspect scaling, loss, or learning rate |
| near the label standard deviation | it is predicting the mean; suspect the features |

## Files

| file | role |
|---|---|
| `dataset.py` | reads `.hbin`, checks versions, refuses mismatches |
| `model.py` | the network; clipped ReLU because that is what quantises exactly |
| `train.py` | training loop, holds the corpus resident in VRAM |
| `fetch-net.sh` | downloads and verifies a published network |

## Distributing networks

Networks are ~21 MB and there will be many of them, so they are not in git
history. git-lfs was rejected too: it puts a bandwidth meter on a public
repository, and the failure mode when the quota runs out is that clones start
handing people pointer files that the engine cannot load.

Instead each network is an asset on its own permanent release, tagged
`net-<hash>`. These are deliberately **not** tied to an engine version: any
binary can fetch any network, and a release is only ever added, never
rewritten, so a hash that resolved once resolves forever.

A network is named after the first 12 hex digits of its own sha256:

```
havoc-69c7d05e4298.nnue
   └── sha256sum of the file begins 69c7d05e4298
```

So the name identifies the contents, and two files with the same name are
necessarily the same file. `fetch-net.sh` checks the download against the full
sha256 in `nets/default.txt` rather than the 48-bit prefix in the name, and
verifies before moving the file into place, so a truncated or corrupted fetch
can neither masquerade as a good network nor be left sitting where the engine
looks.

```bash
scripts/fetch-net.sh            # the network this checkout expects
scripts/fetch-net.sh --list     # everything published
scripts/fetch-net.sh --dir ./   # install somewhere specific
```

With no arguments it reads `nets/default.txt`, which is also where CMake reads
the name it compiles in, so it always fetches the network matching the code you
are about to build. Networks land in `nets/` by default, which is on the
engine's search path.

### Where the engine looks

The engine loads its network at startup without being told to, because the
alternative — silently falling back to the handcrafted evaluation, ~160 Elo
weaker, with no error — is a bad default. Search order is in
`include/havoc/net_path.hpp` and pinned by `tests/test_net_path.cpp`:

1. `$HAVOC_EVAL_FILE` (used verbatim)
2. `$HAVOC_NET_DIR`
3. next to the binary, then `<binary dir>/nets/`
4. the working directory, then `./nets/` — where `fetch-net.sh` installs
5. `~/.local/share/havoc/` (`%LOCALAPPDATA%\havoc\` on Windows)

Explicit configuration comes first: someone who set `HAVOC_NET_DIR` said which
network they want, and a file that merely happens to sit beside the binary
should not quietly outrank that.

The UCI `EvalFile` option overrides all of it, and `EvalFile none` returns the
engine to the handcrafted evaluation. If nothing is found the engine says so
and keeps playing rather than refusing to start.

### Published networks

| network | L1 | corpus | notes |
|---|---|---|---|
| `havoc-69c7d05e4298.nnue` | 256 | 28.9M positions, iteration 4 | ships with v2.3; +60.8 ± 22.0 Elo over the one below |
| `havoc-a311596752e1.nnue` | 256 | iteration 3 | first network to beat HCE; superseded |

Because `bench` node counts differ between evaluations, the bench line names
the evaluation it used. Do not compare across them:

```
Bench: 728941 nodes, ..., eval NNUE     # with havoc-69c7d05e4298.nnue
Bench: 709908 nodes, ..., eval HCE      # no network, e.g. in CI
```

## Notes

- The corpus is kept on the GPU rather than streamed. At 128 bytes a record,
  several million positions fit with room to spare, and the host-to-device
  copy would otherwise dominate a network this small. When the corpus outgrows
  VRAM, this is the first thing to change.
- Perspectives are concatenated **side-to-move first**. The network cannot
  express "it is my move" unless the ordering carries it, and getting this
  backwards trains to a plausible loss while evaluating the wrong side.
- The dense layers are clamped into the int8-representable range on every
  step, so the network trains *inside* the set quantisation can hold rather
  than being projected into it afterwards.
