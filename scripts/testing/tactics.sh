#!/bin/bash
# usage: tactics.sh <engine> <movetime_ms>
ENG="${1:?engine binary}"; MT="${2:-3000}"
declare -a T=(
"2rr3k/pp3pp1/1nnqbN1p/3pN3/2pP4/2P3Q1/PPB4P/R4RK1 w - -|g3g6"
"8/7p/5k2/5p2/p1p2P2/Pr1pPK2/1P1R3P/8 b - -|b3b2"
"5rk1/1ppb3p/p1pb4/6q1/3P1p1r/2P1R2P/PP1BQ1P1/5RKN w - -|e3g3"
"r1bq2rk/pp3pbp/2p1p1pQ/7P/3P4/2PB1N2/PP3PPR/2KR4 w - -|h6h7"
"5k2/6pp/p1qN4/1p1p4/3P4/2PKP2Q/PP3r2/3R4 b - -|c6c4"
"7k/p7/1R5K/6r1/6p1/6P1/8/8 w - -|b6b7"
"rnbqkb1r/pppp1ppp/8/4P3/6n1/7P/PPPNPPP1/R1BQKBNR b KQkq -|g4e3"
"r4q1k/p2bR1rp/2p2Q1N/5p2/5p2/2P5/PP3PPP/R5K1 w - -|e7f7"
"3q1rk1/p4pp1/2pb3p/3p4/6Pr/1PNQ4/P1PB1PP1/4RRK1 b - -|d6h2"
"2br2k1/2q3rn/p2NppQ1/2p1P3/Pp5R/4P3/1P3PPP/3R2K1 w - -|h4h7"
)
ok=0
for t in "${T[@]}"; do
  fen="${t%%|*}"; want="${t##*|}"
  got=$( (echo "position fen $fen 0 1"; echo "go movetime $MT"; sleep $(echo "scale=1; $MT/1000 + 1.5" | bc); echo "quit") | $ENG 2>/dev/null | grep "^bestmove" | head -1 | awk '{print $2}')
  if [ "$got" == "$want" ]; then ok=$((ok+1)); r=OK; else r="-- (want $want)"; fi
  echo "  $got $r"
done
echo "SOLVED: $ok/${#T[@]}"
