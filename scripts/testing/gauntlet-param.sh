#!/bin/bash
# Sweep ONE tuning parameter over several values in a single machine occupancy.
#
#   gauntlet-param.sh <binary> <base-paramfile> <param> <tag> <val>...
#
# Every engine in the run is the same executable differing only in the value of
# <param>, so nothing but that parameter can explain the result. The base (the
# value already in <base-paramfile>) is entered first, which makes it the
# gauntlet engine: it plays each variant and every game spends itself on a
# comparison against the incumbent.
#
# Why a gauntlet rather than N serial SPRTs. Only one match can own this box at
# a time, so N candidates run back to back and cost N occupancies. A gauntlet
# tests all of them in one, and a sweep is the right shape when the question is
# "where is the optimum" rather than "is this one specific value better".
# The cost is that it answers with confidence intervals instead of a stopping
# rule, so read it as a gradient and confirm the winner with sprt.sh.
#
# Both sides load a parameter file, including the base. Comparing "loaded a
# file" against "used compiled-in defaults" would put the file-loading path
# itself into the diff; this way that path is common to every engine.
#
# TC, ROUNDS, CONC come from the environment (see common.sh).

set -u
source "$(dirname "${BASH_SOURCE[0]}")/common.sh"

BIN="${1:?engine binary}"
BASEPF="${2:?base param file}"
PARAM="${3:?parameter name}"
TAG="${4:?tag}"
shift 4
[ "$#" -ge 1 ] || { echo "error: give at least one value to sweep" >&2; exit 2; }

TC="${TC:-10+0.1}"
ROUNDS="${ROUNDS:-600}"

[ -x "$BIN" ]     || { echo "error: engine not executable: $BIN" >&2; exit 2; }
[ -f "$BASEPF" ]  || { echo "error: no such param file: $BASEPF" >&2; exit 2; }

# The parameter must exist in the file, exactly once. A typo would otherwise
# produce a sweep in which every "variant" file is byte-identical to the base
# and the run would report a few hundred Elo of pure noise as a result.
n=$(grep -cE "^${PARAM} = " "$BASEPF" || true)
if [ "$n" -ne 1 ]; then
    echo "error: '$PARAM' matches $n lines in $BASEPF (expected exactly 1)." >&2
    exit 2
fi
BASEVAL=$(grep -E "^${PARAM} = " "$BASEPF" | sed -E 's/.* = //')
echo "sweeping $PARAM: base=$BASEVAL against $*"

ENGINES=(-engine cmd="$BIN" name="base$BASEVAL" option.ParamFile="$BASEPF")

for v in "$@"; do
    if [ "$v" = "$BASEVAL" ]; then
        echo "error: $v is the base value; it would play itself." >&2
        exit 2
    fi
    PF="$HAVOC_WORK/params-$TAG-$PARAM$v.txt"
    sed -E "s/^${PARAM} = .*/${PARAM} = ${v}/" "$BASEPF" > "$PF"

    # Exactly one line may differ. sed silently doing nothing, or matching more
    # than intended, both show up here rather than as a mysterious result.
    d=$(diff "$BASEPF" "$PF" | grep -c '^<' || true)
    if [ "$d" -ne 1 ]; then
        echo "error: $PF differs from base on $d lines (expected 1)." >&2
        exit 2
    fi
    ENGINES+=(-engine cmd="$BIN" name="$PARAM$v" option.ParamFile="$PF")
done

OUT="$HAVOC_WORK/gauntlet-$TAG.log"

nohup "$CUTECHESS" \
  "${ENGINES[@]}" \
  -each proto=uci tc="$TC" timemargin=1000 option.Threads=1 option.Hash=64 \
  -tournament gauntlet -games 2 -rounds "$ROUNDS" -concurrency "$CONC" -repeat \
  -openings file="$HAVOC_BOOK" format=epd order=random \
  -pgnout "$HAVOC_WORK/gauntlet-$TAG.pgn" > "$OUT" 2>&1 &

echo "started (tc=$TC rounds=$ROUNDS conc=$CONC)"
echo "log: $OUT"
echo "read with: grep -E 'Elo difference|Rank' $OUT | tail"
