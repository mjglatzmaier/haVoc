#!/bin/bash
# usage: fuzz-uci.sh <engine> [seed]
#
# Feeds the engine input a GUI or a script could plausibly send but that no
# well-formed session ever produces, and reports anything that does not come
# back alive. Every case in the FEN and setoption corpora below crashed or hung
# a released build at some point; they are kept as a standing check rather than
# as a story about one afternoon.
#
# Run it against a sanitizer build to get the most out of it:
#   cmake -B build-san -DCMAKE_BUILD_TYPE=RelWithDebInfo -DHAVOC_SANITIZE=address,undefined
#   scripts/testing/fuzz-uci.sh build-san/havoc
#
# Exits non-zero if any case crashes, hangs, or trips a sanitizer.

set -u
ENG="${1:?engine binary}"
SEED="${2:-7}"
FAILURES=0
CASES=0

# A search only runs while stdin is open. Piping "go" followed immediately by
# "quit" makes the engine abort the search and return whatever it has, which
# looks exactly like a bug and is not one -- hold stdin open with a sleep.
run_case() {
    local label="$1" script="$2" wait_s="${3:-2}"
    CASES=$((CASES + 1))
    local out rc
    out=$( (printf '%b' "$script"; sleep "$wait_s") | timeout 40 "$ENG" 2>&1 )
    rc=$?
    local why=""
    case $rc in
        0) ;;
        124) why="hang (timed out)" ;;
        134) why="SIGABRT" ;;
        139) why="SIGSEGV" ;;
        *) why="exit $rc" ;;
    esac
    if grep -qiE "runtime error|ERROR: AddressSanitizer|LeakSanitizer" <<<"$out"; then
        why="${why:+$why, }sanitizer diagnostic"
    fi
    if [ -n "$why" ]; then
        FAILURES=$((FAILURES + 1))
        printf '  FAIL  %-52s %s\n' "$label" "$why"
    fi
}

echo "fuzzing $ENG"

# ── Malformed FENs ──────────────────────────────────────────────────────────
# Missing kings indexed the attack tables with no_square, which is 65 against
# 64 entries. A pawn on the first or last rank drove kpk::pawn_index() negative.
# A placement field that runs off the board wrote outside the piece bitboards.
echo "position fen"
while IFS= read -r fen; do
    [ -z "$fen" ] && continue
    run_case "$fen" "position fen $fen\ngo depth 4\n"
done <<'FENS'
8/5ppp/8/8/8/8/5PPP/R5K1 w - - 0 1
6k1/5ppp/8/8/8/8/5PPP/R7 w - - 0 1
8/8/8/8/8/8/8/8 w - - 0 1
4K3/8/8/8/8/8/8/4K3 w - - 0 1
4k3/8/8/8/8/8/8/4K2P w - - 0 1
p7/8/8/8/8/8/8/4K2k w - - 0 1
9999999/8/8/8/8/8/8/4K3 w - - 0 1
/////// w - - 0 1
8/8/8/8/8/8/8/RRRRKRRR w - - 0 1
rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - abc 1
rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq zz 0 1
rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBN w KQkq - 0 1
4k3/8/8/8/8/8/8/4K3
4k3/4k3/8/8/8/8/8/4K3 w - - 0 1

FENS

# ── Option values ───────────────────────────────────────────────────────────
# The handshake advertises bounds for both spin options; these check they are
# enforced and not merely printed.
echo "setoption"
while IFS= read -r opt; do
    [ -z "$opt" ] && continue
    run_case "$opt" "$opt\nposition startpos\ngo depth 6\n"
done <<'OPTS'
setoption name hash value -1
setoption name hash value 0
setoption name hash value 999999999
setoption name hash value abc
setoption name threads value 0
setoption name threads value -4
setoption name threads value abc
setoption name paramfile value /nonexistent/file
setoption name bookfile value /nonexistent/file
setoption name syzygypath value /nonexistent/dir
OPTS

# ── go limits ───────────────────────────────────────────────────────────────
echo "go"
while IFS= read -r g; do
    [ -z "$g" ] && continue
    run_case "$g" "position startpos\n$g\n" 3
done <<'GOS'
go depth 0
go depth -5
go depth 999
go movetime -1
go nodes abc
go wtime abc btime 100
go movestogo -1 wtime -1
GOS

# ── Move lists ──────────────────────────────────────────────────────────────
# This is the path cutechess actually drives, so it matters most. One engine
# process takes the whole corpus: a crash anywhere kills it and is caught by
# the line count at the end.
echo "position ... moves (randomised)"
CMDS=$(mktemp) || exit 1
OUT=$(mktemp) || exit 1
trap 'rm -f "$CMDS" "$OUT"' EXIT
python3 - "$SEED" >"$CMDS" <<'PY'
import random, sys
random.seed(int(sys.argv[1]))
files, ranks, promo = "abcdefgh", "12345678", "qrbnQRBN"
junk = ["", "z9z9", "e2e", "e2e4e5", "0000", "a1a1", "9999", "--", "e2e4q",
        "h8h8n", "xxxx", "e9e1", "i1i2"]
N = 3000
for _ in range(N):
    mvs = []
    for _ in range(random.randint(0, 6)):
        if random.random() < 0.35:
            mvs.append(random.choice(junk))
        else:
            m = (random.choice(files) + random.choice(ranks)
                 + random.choice(files) + random.choice(ranks))
            if random.random() < 0.2:
                m += random.choice(promo)
            mvs.append(m)
    base = ("startpos" if random.random() < 0.7 else
            "fen rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1")
    print("position " + base + " moves " + " ".join(mvs))
    print("d")
print(N)
PY
EXPECT=$(tail -1 "$CMDS")
head -n -1 "$CMDS" >"$OUT.cmds" && mv "$OUT.cmds" "$CMDS"
CASES=$((CASES + EXPECT))
if ! timeout 300 "$ENG" <"$CMDS" >"$OUT" 2>&1; then
    FAILURES=$((FAILURES + 1))
    echo "  FAIL  randomised move lists                              engine exited non-zero"
fi
GOT=$(grep -c '^fen: ' "$OUT")
if [ "$GOT" != "$EXPECT" ]; then
    FAILURES=$((FAILURES + 1))
    echo "  FAIL  randomised move lists                              answered $GOT of $EXPECT"
fi
if grep -qiE "runtime error|ERROR: AddressSanitizer" "$OUT"; then
    FAILURES=$((FAILURES + 1))
    echo "  FAIL  randomised move lists                              sanitizer diagnostic"
fi

echo
if [ "$FAILURES" -eq 0 ]; then
    echo "$CASES cases, all survived"
    exit 0
fi
echo "$CASES cases, $FAILURES failed"
exit 1
