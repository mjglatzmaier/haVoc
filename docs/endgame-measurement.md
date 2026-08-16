# Measuring endgame strength with tablebases

`scripts/eval/endgame_oracle.py` grades haVoc's endgame play against Syzygy
tablebases. It exists because the usual alternative — a curated book of famous
studies — cannot answer the question we actually have.

## Why not a study book

GM-RAM, the Lucena and Philidor positions and similar collections are small and
hand-picked. They tell you whether the engine solved N specific puzzles. They
cannot tell you the *rate* at which it mishandles rook-and-pawn endings, and
they are selected for being interesting to humans, which is not the same thing
as being where our engine is weak.

Syzygy is a perfect oracle over an unlimited, uncurated sample. Every number
the harness prints is a rate with a Wilson confidence interval.

## The oracle must be invisible to the engine

haVoc accepts `setoption name syzygypath`. If the engine can see the same
tables the harness is grading it with, it plays these positions perfectly by
construction and every metric reads 100%. The harness never sets `syzygypath`;
do not point the engine at tables when running it.

## Getting the tables

3-4-5 man is about 1 GB and covers every constellation in the default list:

```
mkdir -p ~/havoc-data/syzygy
cd ~/havoc-data/syzygy
curl -sO 'https://tablebase.lichess.ovh/tables/standard/3-4-5/{list}'
```

6-man is roughly 150 GB. It is worth it for rook-and-pawn work (KRPvKRP) but
is not needed for the first pass.

## The two questions

### `--mode probe`

Given a position whose theoretical result is known, does the engine's chosen
move preserve it? Cheap, so it runs on thousands of positions. It also records
what the engine *scores* the position, which separates evaluation from search:
a theoretically drawn position scored at +400 is an evaluation defect no matter
which move gets played.

Reported per constellation:

- `win-held` — of theoretically won positions, how often the move kept the win.
- `draw-held` — of theoretically drawn positions, how often the move kept the draw.
- `|eval| on drawn` — mean absolute score on drawn positions. Should be near zero.
- `frac>150cp` — fraction of drawn positions the engine thinks are clearly better
  for someone. This is the number that indicts a missing drawishness rule.

Lost positions are excluded rather than counted as successes. Nothing can be
thrown away in a lost position, so including them inflates every rate towards
100% in constellations that are mostly losses.

### `--mode convert`

Can the engine actually *win* a won position, against perfect defence, inside
the fifty-move rule? Many engines know KBNvK is winning and still fail to mate,
because the win needs a 20+ move plan no shallow search sees.

Probe mode cannot detect this. Every individual move preserves the win right up
until the draw is claimed, so a shuffling engine scores 100% on preservation
and 0% on conversion. The defender maximises DTZ among result-holding moves,
which is what creates the fifty-move pressure that exposes the gap.

## `--max-preserving`, and why the default is misleading

Sampled uniformly, most positions in a constellation like KBBvK are ones where
almost every legal move keeps the win. An engine that plays plausible-looking
moves scores ~99% and the metric cannot distinguish a good endgame evaluator
from a bad one.

`--max-preserving 0.35` keeps only positions where at most 35% of legal moves
hold the result. That concentrates the sample on the moments that decide the
ending — the same thing a study book is reaching for, without the curation
bias. It costs one tablebase probe per legal move.

Always report the `difficulty` column alongside the rates. A win-held rate is
only comparable against another run at the same `--max-preserving`.

## Example

```
python3 scripts/eval/endgame_oracle.py \
  --engine /tmp/b-rel/havoc \
  --syzygy ~/havoc-data/syzygy \
  --samples 400 --depth 12 --max-preserving 0.35 \
  --json ~/havoc-data/endgame-baseline.json
```

## What this is for

The evaluation's endgame layer is not deleted when NNUE lands. Drawishness
scale factors and specialised material knowledge sit in front of the network in
every strong engine, because sparse long-horizon facts are what a net trained
on our data volume learns worst. So the profile this harness produces stays
useful across the NNUE transition, and each fix to `src/eval/hce.cpp` can be
measured against it rather than argued about.
