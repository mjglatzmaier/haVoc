#!/bin/bash
# Shared configuration for the haVoc measurement harness.
#
# Everything machine-specific is resolved here, through environment variables
# with sane defaults, so the same scripts run unchanged on another box.
#
#   CUTECHESS   path to cutechess-cli            (default: found on PATH)
#   REFENGINES  directory holding anchor engines (default: ~/code/refengines)
#   HAVOC_WORK  where logs and PGNs are written  (default: ./testruns)
#   HAVOC_BOOK  opening book                     (default: this directory's book.epd)
#   CONC        concurrent games                 (default: physical cores - 2)
#
# Concurrency default is deliberately conservative. Oversubscribing the box
# during a match does not bias the result when both engines are equally starved,
# but it does inflate the variance of every game, which costs games-to-decision.

set -u

HARNESS_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

CUTECHESS="${CUTECHESS:-$(command -v cutechess-cli || echo cutechess-cli)}"
REFENGINES="${REFENGINES:-$HOME/code/refengines}"
HAVOC_WORK="${HAVOC_WORK:-$PWD/testruns}"
HAVOC_BOOK="${HAVOC_BOOK:-$HARNESS_DIR/book.epd}"

if [ -z "${CONC:-}" ]; then
    if command -v nproc >/dev/null 2>&1; then
        _cores=$(nproc)
    elif [ "$(uname)" = "Darwin" ]; then
        _cores=$(sysctl -n hw.physicalcpu 2>/dev/null || sysctl -n hw.ncpu)
    else
        _cores=4
    fi
    CONC=$(( _cores > 3 ? _cores - 2 : 1 ))
fi

mkdir -p "$HAVOC_WORK"

if [ ! -x "$CUTECHESS" ] && ! command -v "$CUTECHESS" >/dev/null 2>&1; then
    echo "error: cutechess-cli not found at '$CUTECHESS'." >&2
    echo "       Set CUTECHESS=/path/to/cutechess-cli." >&2
    exit 2
fi

if [ ! -f "$HAVOC_BOOK" ]; then
    echo "error: opening book not found at '$HAVOC_BOOK'." >&2
    exit 2
fi

# Guard against the degenerate SPRT. When elo0 == elo1 the log likelihood ratio
# is identically zero, so the test can never reach either bound: the run burns
# every one of maxgames and concludes nothing. This cost a full overnight run
# once, so it is checked rather than documented.
require_distinct_bounds() {
    if [ "$1" = "$2" ]; then
        echo "error: elo0 and elo1 are both $1. The SPRT would be degenerate --" >&2
        echo "       llr stays 0 forever and the run can never reach a bound." >&2
        echo "       Use elo1=5 for a gain test, or elo1=0 elo0=-5 for non-regression." >&2
        exit 2
    fi
}
