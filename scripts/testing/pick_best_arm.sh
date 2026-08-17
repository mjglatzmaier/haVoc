#!/bin/bash
# Pick the winning arm of a multi-arm match sweep, by match result.
#
# Written after the iteration-4 chain installed the *worst* of four nets --
# 154 Elo below the best -- and then spent twenty-four minutes tuning search
# against it. Nothing in the pipeline noticed. The failure is worth recording
# in full, because neither half of it looked like a bug:
#
#   1. Completion was tested with `grep -q "^Elo difference" LOG`. cutechess
#      prints that line at every `-ratinginterval`, not only at the end, so the
#      test passed for an arm that was still playing.
#
#   2. The value was read with `grep -oE`, which returns *every* match. A
#      finished log holds three, so the shell substituted a multi-line string
#      into `awk "BEGIN{exit !($e > $best)}"`. awk died of a syntax error, and
#      because the comparison was the condition of an `if`, the arm was
#      silently skipped rather than reported.
#
# Together those produced a confident, plausible, wrong answer: three finished
# arms were skipped as unparseable, and the one arm still mid-match had exactly
# one interim number, parsed cleanly, and won by default.
#
# The rules that follow from that:
#   - completion means the *terminal* marker, never a value that also appears
#     in progress output;
#   - take the last value, and require it to be a single number;
#   - a parse failure is fatal, never a skip;
#   - every arm must be present and complete before anything is chosen.
#
# Usage: pick_best_arm.sh LOG_PREFIX ARM [ARM...]
#   e.g. pick_best_arm.sh testruns/iter4- a b c d
# Prints "<arm> <elo>" on success. Exits non-zero, with a reason, otherwise.
set -eu

if [ $# -lt 2 ]; then
    echo "usage: $0 LOG_PREFIX ARM [ARM...]" >&2
    exit 2
fi

prefix=$1
shift

best=""
best_elo=""

for arm in "$@"; do
    log="${prefix}${arm}.log"

    [ -s "$log" ] || { echo "$0: $log is missing or empty" >&2; exit 1; }

    # The terminal marker. "Elo difference" is not usable here: cutechess emits
    # it at every rating interval, which is precisely how a match still in
    # flight was once mistaken for a finished one.
    grep -q "^Finished match" "$log" || {
        echo "$0: $log has no 'Finished match' -- the match did not complete" >&2
        exit 1
    }

    # Last value, not every value.
    elo=$(grep -E "^Elo difference:" "$log" | tail -1 \
          | sed -nE 's/^Elo difference: (-?[0-9]+(\.[0-9]+)?).*/\1/p')

    # A parse failure is fatal. Skipping is what let a single arm win by
    # default against no comparison at all.
    case "$elo" in
        ""|*[!0-9.-]*|*-*-*)
            echo "$0: could not read a single Elo value from $log (got '$elo')" >&2
            exit 1
            ;;
    esac

    echo "  $arm: $elo Elo" >&2

    if [ -z "$best_elo" ] || awk -v a="$elo" -v b="$best_elo" 'BEGIN{exit !(a>b)}'; then
        best_elo=$elo
        best=$arm
    fi
done

[ -n "$best" ] || { echo "$0: no arm selected" >&2; exit 1; }

echo "$best $best_elo"
