#!/bin/bash
# Gauntlet against anchors with published, well-sampled CCRL ratings, used to
# place haVoc on an absolute scale.
#
#   gauntlet-anchors.sh <havoc-binary> <tag> [rounds] [tc] [conc]
#
# This answers "how strong is haVoc". It is the WRONG tool for "is version B
# better than version A" -- for that use sprt.sh, which spends every game on the
# comparison you actually care about instead of on anchors.
#
# ---------------------------------------------------------------------------
# Anchor selection
# ---------------------------------------------------------------------------
# haVoc measures ~2500 (CCRL 40/40 scale), from head-to-head results in three
# runs of >=100 games each: Fruit implied 2492/2491/2536, Glaurung 2451/2561/2448.
#
# Anchors must BRACKET the engine. Information per game is maximised near a 50%
# score; at the 22% haVoc scores against Fruit and Glaurung each game carries
# little, and with both anchors above it the fit is extrapolating rather than
# interpolating. Arasan 12.2, Phalanx XXIV and Zurichess sit at or just below
# haVoc and turn that extrapolation into interpolation.
#
# Anchors must also be well sampled. Two rejected outright:
#   GopherCheck 0.2.3 and Rusty-Rival implied 2284 and 2507 for the SAME
#   candidate -- a 223-point contradiction.
#   Sungorus 1.4, despite 1583 CCRL games, implied 2192 in a run where Fruit and
#   Glaurung implied 2429 and 2397. A thinly-scrutinised published rating and a
#   lumpy eval surface both break the assumption of a single strength scale.
# Also rejected for thin sampling: Arasan 12.1 (354 games), Phalanx XXIII (361).
#
#   engine                    CCRL 40/40   games   +/-
#   Zurichess Fribourg 64-bit      2412     449     23
#   Arasan 12.2 64-bit             2505     825     18
#   Phalanx XXIV                   2521     887     17
#   Fruit 2.1                      2694     505     21
#   Glaurung 2.2 64-bit            2793    1396     13
#
# CCRL 40/40 is far slower than the blitz TC used here, and older engines tend
# to look relatively stronger at long TC, so a systematic offset sits on top of
# the statistical error. Treat the absolute number as +/-50 at best; trust the
# TREND across runs far more than the value.
#
# ---------------------------------------------------------------------------
# Anchor defaults are hostile and MUST be forced
# ---------------------------------------------------------------------------
# Glaurung defaults to Threads=7 and Ponder=true; Zurichess defaults to
# Ponder=true; Fruit, Glaurung and Arasan all default to OwnBook=true. A
# 300-game run was invalidated once by not forcing these. Phalanx speaks xboard
# only, not UCI.

set -u
source "$(dirname "${BASH_SOURCE[0]}")/common.sh"

CAND="${1:?havoc binary}"
TAG="${2:?tag}"
ROUNDS="${3:-60}"
TC="${4:-20+0.2}"
[ -n "${5:-}" ] && CONC="$5"

R="$REFENGINES"
ENGINES=()
FOUND=()

add_anchor() {
    local path="$1"; shift
    if [ -x "$path" ]; then
        ENGINES+=(-engine cmd="$path" "$@")
        FOUND+=("$1")
    fi
}

add_anchor "$R/zurichess-fribourg/zurichess-bin" name=zurichess proto=uci \
           option.Ponder=false
add_anchor "$R/arasan-12.2.0/export/arasanx"     name=arasan122 proto=uci \
           option.OwnBook=false option.Ponder=false option.Threads=1
add_anchor "$R/phalanx-XXIV/phalanx"             name=phalanx24 proto=xboard
add_anchor "$R/fruit-2.1/src/fruit"              name=fruit     proto=uci \
           option.OwnBook=false
add_anchor "$R/glaurung-2.2/src/glaurung"        name=glaurung  proto=uci \
           option.Threads=1 option.Ponder=false option.OwnBook=false

if [ "${#FOUND[@]}" -lt 2 ]; then
    echo "error: fewer than two anchors found under REFENGINES=$R" >&2
    echo "       see scripts/testing/README.md for build instructions." >&2
    exit 2
fi

echo "anchors: ${#FOUND[@]} found under $R"

nohup "$CUTECHESS" \
  -engine cmd="$CAND" name=havoc proto=uci option.Threads=1 \
  "${ENGINES[@]}" \
  -each tc="$TC" timemargin=1000 option.Hash=64 \
  -tournament gauntlet -games 2 -rounds "$ROUNDS" -concurrency "$CONC" -repeat \
  -openings file="$HAVOC_BOOK" format=epd order=random \
  -pgnout "$HAVOC_WORK/gauntlet-$TAG.pgn" > "$HAVOC_WORK/gauntlet-$TAG.log" 2>&1 &

echo "gauntlet started (tc=$TC rounds=$ROUNDS conc=$CONC)"
echo "log: $HAVOC_WORK/gauntlet-$TAG.log"
echo "analyse with: scripts/testing/anchor_rating.py $HAVOC_WORK/gauntlet-$TAG.log"
