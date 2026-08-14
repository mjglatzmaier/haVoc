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

# ── Step 3: Run staged Texel tuning ──────────────────────────────────────────
# The category scales and the individual weights they multiply are fitted in
# separate stages on purpose: fitting them together leaves an exact flat
# direction in the loss. That means stage 2 changes the shapes underneath the
# scales chosen in stage 1, so stage 1 is refitted afterwards.
echo "--- Stage 1: category scales (coarse) ---"
"$TUNER" --data "$DATA_FILE" --output "$PARAMS_FILE" --stage 1 \
         --iterations "$ITERATIONS" --threads "$THREADS" --max-step 32
echo ""

echo "--- Stage 2: individual weights and curve shapes ---"
"$TUNER" --data "$DATA_FILE" --params "$PARAMS_FILE" --output "$PARAMS_FILE" --stage 2 \
         --iterations "$ITERATIONS" --threads "$THREADS" --max-step 24
echo ""

echo "--- Stage 1 refit: category scales against the new shapes ---"
"$TUNER" --data "$DATA_FILE" --params "$PARAMS_FILE" --output "$PARAMS_FILE" --stage 1 \
         --iterations "$ITERATIONS" --threads "$THREADS" --max-step 24
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
echo "has won a match. To evaluate it:"
echo ""
echo "  1. python3 scripts/bake_params.py $PARAMS_FILE"
echo "  2. cmake --build $BUILD_DIR --parallel"
echo "  3. SPRT the resulting binary against the current one"
echo ""
echo "Revert the bake if the match does not support it."
