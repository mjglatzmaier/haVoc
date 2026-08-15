#!/bin/bash
# Fixed-depth SPRT: identical to sprt.sh but replaces the clock with a depth
# limit, so the result is independent of machine load and of any nps difference
# between the two binaries.
#
#   sprt-depth.sh <candidate> <baseline> <tag> [depth] [elo1] [maxgames] [elo0]
#
# Use it to separate "this change searches better" from "this change is faster".
# A change that wins on time but is flat at fixed depth bought its Elo with nps,
# not with knowledge -- and vice versa.

set -u
source "$(dirname "${BASH_SOURCE[0]}")/common.sh"

CAND="${1:?candidate binary}"
BASE="${2:?baseline binary}"
TAG="${3:?tag}"
DEPTH="${4:-8}"
ELO1="${5:-10}"
MAXGAMES="${6:-4000}"
ELO0="${7:-0}"

require_distinct_bounds "$ELO0" "$ELO1"

OUT="$HAVOC_WORK/sprt-$TAG.log"

{
  echo "=== SPRT (fixed depth $DEPTH) $TAG ==="
  echo "candidate: $CAND"
  echo "baseline : $BASE"
  echo "depth=$DEPTH elo0=$ELO0 elo1=$ELO1 alpha=0.05 beta=0.05 maxgames=$MAXGAMES conc=$CONC"
  echo "started  : $(date)"
} | tee "$OUT"

"$CUTECHESS" \
  -engine name=cand cmd="$CAND" \
  -engine name=base cmd="$BASE" \
  -each proto=uci tc=inf depth="$DEPTH" option.Hash=64 option.Threads=1 timemargin=300 \
  -openings file="$HAVOC_BOOK" format=epd order=random \
  -repeat -games "$MAXGAMES" -concurrency "$CONC" \
  -sprt elo0="$ELO0" elo1="$ELO1" alpha=0.05 beta=0.05 \
  -ratinginterval 20 \
  -pgnout "$HAVOC_WORK/sprt-$TAG.pgn" >> "$OUT" 2>&1

echo "finished : $(date)" | tee -a "$OUT"
grep -E "^Score of|^Elo difference|^SPRT" "$OUT" | tail -4
