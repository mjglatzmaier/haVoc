#!/bin/bash
# SPRT a parameter file against the same binary running its compiled-in
# defaults. Both sides are the identical executable, so the only difference is
# the parameter set -- no rebuild, no chance of an unrelated code delta leaking
# into the comparison.
#
#   sprt-paramfile.sh <binary> <paramfile> <tag> [tc] [maxgames] [elo1] [elo0]
#
# ALWAYS verify the round-trip first. Generate the file with
#   havoc_texel --iterations 0 --output F
# then confirm `setoption name ParamFile value F` reproduces the default bench
# node count exactly. havoc_texel embeds compiled-in defaults, so it must be
# rebuilt after any parameters.hpp change or it silently dumps stale values.

set -u
source "$(dirname "${BASH_SOURCE[0]}")/common.sh"

BIN="${1:?engine binary}"
PF="${2:?param file}"
TAG="${3:?tag}"
TC="${4:-10+0.1}"
MAXGAMES="${5:-2000}"
ELO1="${6:-5}"
ELO0="${7:-0}"

require_distinct_bounds "$ELO0" "$ELO1"

OUT="$HAVOC_WORK/sprt-$TAG.log"

{
  echo "=== SPRT $TAG ==="
  echo "engine   : $BIN"
  echo "paramfile: $PF"
  echo "tc=$TC elo0=$ELO0 elo1=$ELO1 maxgames=$MAXGAMES conc=$CONC"
  echo "started  : $(date)"
} | tee "$OUT"

"$CUTECHESS" \
  -engine name=tuned cmd="$BIN" option.ParamFile="$PF" \
  -engine name=base  cmd="$BIN" \
  -each proto=uci tc="$TC" option.Hash=64 option.Threads=1 timemargin=300 \
  -openings file="$HAVOC_BOOK" format=epd order=random \
  -repeat -games "$MAXGAMES" -concurrency "$CONC" \
  -sprt elo0="$ELO0" elo1="$ELO1" alpha=0.05 beta=0.05 \
  -ratinginterval 20 -pgnout "$HAVOC_WORK/sprt-$TAG.pgn" >> "$OUT" 2>&1

echo "finished : $(date)" | tee -a "$OUT"
grep -E "^Score of|^Elo difference|^SPRT" "$OUT" | tail -4
