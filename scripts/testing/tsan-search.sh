#!/bin/bash
# usage: tsan-search.sh <engine> [quick|full]
#
# Runs multi-threaded searches under ThreadSanitizer and fails if any of them
# reports a data race.
#
# The unit tests cannot find these. They drive the engine through its C++
# interface, largely on one thread, so the interesting concurrency -- N search
# threads sharing a transposition table, summing each other's node counters,
# and racing to set the stop flag -- never runs. Every race this script has
# caught so far was invisible to the whole rest of the suite.
#
# Build it with:
#   cmake -B build-tsan -DCMAKE_BUILD_TYPE=RelWithDebInfo -DHAVOC_NATIVE=OFF \
#         -DCMAKE_CXX_FLAGS=-fsanitize=thread \
#         -DCMAKE_EXE_LINKER_FLAGS=-fsanitize=thread
#   scripts/testing/tsan-search.sh build-tsan/havoc
#
# RelWithDebInfo rather than Debug on purpose: TSan is slow enough already, and
# a Debug build makes these searches take long enough that nobody runs them.
#
# Exits non-zero if TSan reports anything.

set -u
ENG="${1:?engine binary}"
MODE="${2:-quick}"
FAILURES=0
CASES=0

if [[ ! -x "$ENG" ]]; then
    echo "not an executable: $ENG" >&2
    exit 1
fi

# TSan aborts at startup with "FATAL: unexpected memory mapping" on kernels
# that hand out more address-space randomisation than its shadow mapping can
# cope with, which includes current Ubuntu. Running with randomisation off is
# the documented workaround and does not weaken the checking.
SETARCH=(setarch "$(uname -m)" -R)
if ! "${SETARCH[@]}" true 2>/dev/null; then
    SETARCH=()
    echo "note: setarch -R unavailable, running with ASLR on" >&2
fi

# halt_on_error=0 so one race does not hide the rest; exitcode=0 so that the
# engine's own exit status stays readable and the race count below is what
# decides the result.
export TSAN_OPTIONS="halt_on_error=0 exitcode=0 history_size=2 ${TSAN_OPTIONS:-}"

# A search only runs while stdin is open: "go" followed immediately by "quit"
# aborts it, which would make this script pass by not searching at all. Hold
# stdin open with a sleep, and check afterwards that a bestmove came back.
run_case() {
    local label="$1" threads="$2" go="$3" wait_s="$4"
    CASES=$((CASES + 1))
    local log out races
    log=$(mktemp)
    out=$( (printf 'uci\nsetoption name Threads value %s\nisready\nposition startpos\n%s\n' \
                "$threads" "$go"; sleep "$wait_s") \
           | timeout 900 "${SETARCH[@]}" "$ENG" 2>"$log" )

    races=$(grep -c 'WARNING: ThreadSanitizer' "$log")
    if ! grep -q '^bestmove' <<<"$out"; then
        echo "FAIL  $label -- no bestmove; the search did not run, so this proves nothing"
        FAILURES=$((FAILURES + 1))
    elif [[ "$races" -gt 0 ]]; then
        echo "FAIL  $label -- $races data race(s)"
        grep -A 6 'WARNING: ThreadSanitizer' "$log" | head -40
        FAILURES=$((FAILURES + 1))
    else
        echo "ok    $label"
    fi
    rm -f "$log"
}

echo "ThreadSanitizer search check: $ENG ($MODE)"

# Fixed depth and time-managed searches take different paths -- the second one
# is what exercises the stop flag and the time checks -- so both are covered.
run_case "depth 8, 2 threads"      2  "go depth 8"        3
run_case "depth 8, 4 threads"      4  "go depth 8"        3
run_case "movetime 2000, 4 threads" 4 "go movetime 2000"  5

if [[ "$MODE" == "full" ]]; then
    run_case "depth 12, 4 threads"      4  "go depth 12"      10
    run_case "movetime 2000, 8 threads" 8  "go movetime 2000" 6
    run_case "movetime 1500, 16 threads" 16 "go movetime 1500" 6
fi

echo "$CASES case(s), $FAILURES failure(s)"
[[ "$FAILURES" -eq 0 ]]
