#!/usr/bin/env bash
set -euo pipefail

# ── Configuration ─────────────────────────────────────────────────────────────
GAMES=${1:-500}
DEPTH=${2:-6}
# Sweeps, not gradient steps. The optimiser is coordinate descent: one sweep
# tries both directions on every parameter and keeps what lowers the error.
ITERATIONS=${3:-20}
THREADS=${4:-$(sysctl -n hw.ncpu 2>/dev/null || nproc 2>/dev/null || echo 4)}
BUILD_DIR="./build"
DATAGEN="$BUILD_DIR/havoc_datagen"
TUNER="$BUILD_DIR/havoc_texel"
ENGINE="$BUILD_DIR/havoc"
DATA_FILE="training_data.epd"
PARAMS_FILE="tuned_params.txt"

echo "=== haVoc Parameter Tuning Pipeline ==="
echo "Games: $GAMES, Depth: $DEPTH, Iterations: $ITERATIONS, Threads: $THREADS"
echo ""

# ── Step 1: Build ─────────────────────────────────────────────────────────────
echo "--- Building ---"
cmake -B "$BUILD_DIR" -DCMAKE_BUILD_TYPE=Release -DHAVOC_BUILD_TOOLS=ON
cmake --build "$BUILD_DIR" --parallel
echo ""

# ── Step 2: Generate training data ───────────────────────────────────────────
echo "--- Generating training data ($GAMES games at depth $DEPTH) ---"
"$DATAGEN" --games "$GAMES" --depth "$DEPTH" --threads "$THREADS" --output "$DATA_FILE"
echo ""

# ── Step 3: Run Texel tuning ─────────────────────────────────────────────────
# One pass over every evaluation parameter, not a ladder of stages.
#
# The stages existed to serve a gradient optimiser, which could not cope with
# fitting a category scale alongside the weights it multiplies -- that pair is
# an exact flat direction in the loss. Coordinate descent has no such problem,
# because it moves one parameter at a time and a single-coordinate move always
# leaves the flat manifold. Staging then became actively harmful: judging a
# category scale against its group's *default* shapes let stage 1 crush a scale
# to 11, and stage 2 had to inflate the weights ninefold to compensate, leaving
# the fit working below its own resolution.
#
# Measured on a disjoint train/validation split of 200k CCRL positions each,
# three sweeps took held-out error from 0.114473 to 0.108037, beating the full
# three-stage ladder's 0.109317 training error on five times the data.
echo "--- Fitting every evaluation parameter ---"
"$TUNER" --data "$DATA_FILE" --output "$PARAMS_FILE" --stage 0 \
         --iterations "$ITERATIONS" --threads "$THREADS"
echo ""

# ── Step 4: Stop ─────────────────────────────────────────────────────────────
# This script deliberately does NOT bake the result into the source tree.
#
# A lower fitting error is not evidence of a stronger engine. The fit is made
# against static evaluations of quiet positions drawn from one distribution,
# while strength is decided by games; the two come apart whenever the training
# set does not look like the positions the engine actually reaches. Baking
# first and measuring later also destroys the baseline needed to measure
# against.
echo "=== Fit complete ==="
echo "Tuned parameters written to: $PARAMS_FILE"
echo ""
echo "This has NOT been baked into the source tree, and should not be until it"
echo "has won a match. The fit can be measured without touching the source at"
echo "all, because the engine accepts it at runtime:"
echo ""
echo "  cutechess-cli -engine cmd=./build/havoc option.ParamFile=$PARAMS_FILE \\"
echo "                -engine cmd=./build/havoc ..."
echo ""
echo "Both sides are then the same binary and the fit is the only difference."
echo "Once it has won, make it the default:"
echo ""
echo "  1. python3 scripts/bake_params.py $PARAMS_FILE"
echo "  2. cmake --build $BUILD_DIR --parallel"
echo "  3. confirm the rebuilt binary matches the one that won the match"
echo ""
echo "Revert the bake if the match does not support it."
