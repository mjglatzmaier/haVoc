#!/bin/bash
# SPRT self-play test: candidate binary vs baseline binary.
#
#   sprt.sh <candidate-binary> <baseline-binary> <tag> [tc] [elo1] [maxgames] [elo0]
#
# A sequential probability ratio test, so an early run of luck cannot trigger a
# conclusion. Stops as soon as H0 (elo<=elo0) or H1 (elo>=elo1) is accepted, or
# when maxgames is reached.
#
#   gain test          : elo1=5           (default elo0=0)  "is it better?"
#   non-regression test: elo1=0  elo0=-5                    "is it not worse?"
#
# Use this, not a gauntlet, for version-vs-version questions. Head-to-head is
# strictly more informative per game than measuring both sides against anchors.

set -u
source "$(dirname "${BASH_SOURCE[0]}")/common.sh"

CAND="${1:?candidate binary}"
BASE="${2:?baseline binary}"
TAG="${3:?tag}"
TC="${4:-10+0.1}"
ELO1="${5:-10}"
MAXGAMES="${6:-4000}"
ELO0="${7:-0}"

require_distinct_bounds "$ELO0" "$ELO1"

OUT="$HAVOC_WORK/sprt-$TAG.log"

{
  echo "=== SPRT $TAG ==="
  echo "candidate: $CAND"
  echo "baseline : $BASE"
  echo "tc=$TC elo0=$ELO0 elo1=$ELO1 alpha=0.05 beta=0.05 maxgames=$MAXGAMES conc=$CONC"
  echo "started  : $(date)"
} | tee "$OUT"

"$CUTECHESS" \
  -engine name=cand cmd="$CAND" \
  -engine name=base cmd="$BASE" \
  -each proto=uci tc="$TC" option.Hash=64 option.Threads=1 timemargin=300 \
  -openings file="$HAVOC_BOOK" format=epd order=random \
  -repeat -games "$MAXGAMES" -concurrency "$CONC" \
  -sprt elo0="$ELO0" elo1="$ELO1" alpha=0.05 beta=0.05 \
  -ratinginterval 20 \
  -pgnout "$HAVOC_WORK/sprt-$TAG.pgn" >> "$OUT" 2>&1

echo "finished : $(date)" | tee -a "$OUT"
grep -E "^Score of|^Elo difference|^SPRT" "$OUT" | tail -4
