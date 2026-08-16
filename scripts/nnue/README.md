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
