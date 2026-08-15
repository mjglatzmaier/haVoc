# haVoc measurement harness

Everything here answers one of two questions, and confusing them wastes days:

| question | tool | why |
|---|---|---|
| Is version B better than version A? | `sprt.sh` | Head-to-head. Every game is spent on the comparison you care about. |
| How strong is haVoc, absolutely? | `gauntlet-anchors.sh` + `anchor_rating.py` | Needs external anchors with published ratings. |

Adding anchors to an A/B test does **not** improve its precision — it steals
games from the only comparison that matters. Keep the two separate.

## Setup

All machine-specific paths are environment variables with defaults
(see `common.sh`):

```sh
export CUTECHESS=/path/to/cutechess-cli   # default: found on PATH
export REFENGINES=$HOME/code/refengines   # anchor engines
export HAVOC_WORK=$PWD/testruns           # logs and PGNs land here
export CONC=14                            # default: physical cores - 2
```

`book.epd` (2000 positions) is committed here so results are comparable across
machines. Using a different book changes the draw rate and therefore the number
of games needed to resolve a given Elo difference.

## A/B testing

```sh
scripts/testing/sprt.sh ./build/havoc /path/to/baseline mytag 10+0.1 5 4000 0
```

Bound conventions:

- gain test — `elo1=5 elo0=0`, "is it better?"
- non-regression — `elo1=0 elo0=-5`, "is it not worse?"

`elo0` must differ from `elo1`. When they are equal the log likelihood ratio is
identically zero, the test can never reach a bound, and the run burns every one
of `maxgames` while concluding nothing. The scripts refuse this rather than
document it, because it cost an overnight run once.

`sprt-depth.sh` replaces the clock with a fixed depth. Use it to tell "searches
better" apart from "is faster": a change that wins on time but is flat at fixed
depth bought its Elo with nps, not knowledge.

`sprt-paramfile.sh` compares a parameter file against the same binary's
compiled-in defaults, so no rebuild and no unrelated code delta can leak in.
Generate a file with `havoc_texel --iterations 0 --output F`, and **always
verify the round-trip reproduces the default bench node count exactly before
trusting it**. `havoc_texel` embeds compiled-in defaults, so it must be rebuilt
after any `parameters.hpp` change or it silently dumps stale values.

### Accumulated-drift check

Individually a change can measure neutral — say −5 ± 25 — and still be merged
under the structural-change policy. Ten such merges can hide a real regression
that no single SPRT was ever powered to see. Periodically SPRT current `main`
against a snapshot from 5–10 PRs back: the accumulated delta is larger, so it
resolves faster and in one direction.

## Absolute rating

```sh
scripts/testing/gauntlet-anchors.sh ./build/havoc r2026xxxx 60 20+0.2
scripts/testing/anchor_rating.py testruns/gauntlet-r2026xxxx.log
```

**Do not read the Elo column of cutechess's ranking table.** It is normalised
across the whole pool, and in a gauntlet the anchors never play each other, so
its gaps are not pairwise-consistent and must not be subtracted. Subtracting
them once turned a genuine 203-point head-to-head gap into a reported 420,
making a ~2500 engine look like ~2300. `anchor_rating.py` ignores that table and
works only from raw game results.

### Choosing anchors

Two rules, both learned the hard way.

**Bracket the engine.** Information per game is maximised near a 50% score. At
the 22% haVoc scores against Fruit and Glaurung each game carries little, and
with every anchor above it the fit is extrapolating. `anchor_rating.py` warns
when this happens.

**Demand a large, long-scrutinised sample.** Rejected anchors, recorded in the
script so nobody re-adds them:

- GopherCheck 0.2.3 and Rusty-Rival implied 2284 and 2507 for the *same*
  candidate — a 223-point contradiction.
- Sungorus 1.4 implied 2192 in a run where Fruit and Glaurung implied 2429 and
  2397, despite 1583 CCRL games behind its rating.
- Arasan 12.1 (354 games) and Phalanx XXIII (361) — too thin.

Current set, CCRL 40/40 "all engines":

| engine | Elo | games | ± | protocol |
|---|---|---|---|---|
| Zurichess Fribourg 64-bit | 2412 | 449 | 23 | uci |
| Arasan 12.2 64-bit | 2505 | 825 | 18 | uci |
| Phalanx XXIV | 2521 | 887 | 17 | **xboard** |
| Fruit 2.1 | 2694 | 505 | 21 | uci |
| Glaurung 2.2 64-bit | 2793 | 1396 | 13 | uci |

Anchor defaults are hostile and are forced in the script: Glaurung defaults to
`Threads=7` and `Ponder=true`; Zurichess defaults to `Ponder=true`; Fruit,
Glaurung and Arasan all default to `OwnBook=true`. A 300-game run was
invalidated once by not forcing these.

Zurichess is Go source with pre-modules import paths. To build it:

```sh
cd $REFENGINES/zurichess-fribourg
go mod init bitbucket.org/zurichess/zurichess   # once; it has no external deps
go build -o zurichess-bin ./zurichess
```

CCRL 40/40 is far slower than the blitz TC used here, and older engines tend to
look relatively stronger at long TC, so a systematic offset sits on top of the
statistical error. Treat the absolute value as ±50 at best and trust the trend
across runs far more than the number.

### Current standing

From `gauntlet-r20260813` (120 games per anchor): Fruit implied 2491, Glaurung
2561, pooled **2525 ± 27** (statistical only). Two other runs of ≥100 games
agree: 2492/2451 and 2536/2448.

## Tactics

```sh
scripts/testing/tactics.sh ./build/havoc 3000
```

Ten positions with forced best moves. A coarse smoke test for search sanity, not
a strength measure — it is far too small to resolve Elo.
