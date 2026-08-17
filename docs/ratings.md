# Rating measurement

How the numbers in the README's strength table are produced, and how much to
trust them. The short version: the per-change SPRTs are the trustworthy
measurements, and the gauntlet is a periodic check that the engine has not
drifted away from the field.

## Method

A maximum-likelihood fit of the logistic Elo model with the opponent ratings
held fixed at their published CCRL 40/40 values, played at 20+0.2 on one thread
with `OwnBook` and `Ponder` forced off. It is an estimate *on* the CCRL scale,
not a CCRL rating.

Anchors are built by [`scripts/testing/fetch-anchors.sh`](../scripts/testing/fetch-anchors.sh),
which pins each version. The harness and the rules for choosing anchors are in
[`scripts/testing/README.md`](../scripts/testing/README.md). To reproduce a
rating from a gauntlet log:

```sh
scripts/testing/anchor_rating.py testruns/gauntlet-<tag>.log
```

## What limits the number

**The fit's ± is statistical only.** It describes how well the model fits these
games. It says nothing about measuring at blitz against ratings established at
40/40, and older engines tend to look relatively stronger at long time control,
so a systematic offset sits on top of it. Treat the uncertainty as roughly ±50,
not the ±12 the fit reports.

**A single strength scale is an approximation.** A one-parameter model asked to
absorb a non-transitive matchup will produce anchors that disagree, which is
exactly what happens below.

**Anchors must bracket the engine.** A fit against anchors that all sit above
the engine is extrapolating. This is the single largest source of error in the
older measurements.

## 2026-08-16, handcrafted evaluation, 2582 ±12, 1000 games

Per-anchor results, which are more informative than the pooled number:

| anchor | published | haVoc score | implies |
|---|---|---|---|
| Zurichess Fribourg | 2412 | 69.5% | 2555 |
| Arasan 12.2 | 2505 | 69.2% | 2646 |
| Phalanx XXIV | 2521 | 55.2% | 2558 |
| Fruit 2.1 | 2694 | 34.5% | 2583 |
| Glaurung 2.2 | 2793 | 21.5% | 2568 |

Four of the five agree closely (2555–2583). Arasan is an outlier: haVoc scores
69.2% against it but only 55.2% against Phalanx, whose published rating is 16
points *higher* — a ~100 Elo discrepancy between two engines rated the same.
One of those published ratings does not describe the engine as built here, and
dropping the anchor that disagrees would be choosing the answer, so the pooled
figure keeps it and the spread is reported instead. Excluding Arasan the fit
gives 2566 ± 13.

## Superseded measurements

| Date | Rating | fit ± | Games | Anchors | Notes |
|------|--------|-------|-------|---------|-------|
| 2026-08-14 | 2503 | ±55 | 240 | 2 | Correction history, mobility counts captures |
| 2026-08-14 | 2523 | ±53 | 240 | 2 | Evaluation coverage batch |
| 2026-08-13 | 2473 | ±60 | 224 | 2 | Correctness batch |
| (earlier) | 2413 | ±82 | 150 | 2 | Prior baseline |

Do not read this column as a trajectory. The jump from ~2500 to ~2580 was
mostly a change of instrument, not of engine: every row here was fitted against
Fruit and Glaurung alone, both ~200 Elo *above* haVoc, where the model
extrapolates. The August 16 run added three anchors at or below the engine, so
the fit interpolates instead. The anchor binaries were also rebuilt from source
for that run, and are not the same builds that produced these rows.
