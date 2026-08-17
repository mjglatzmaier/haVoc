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

## 2026-08-17, NNUE L1=256, 2834 ±12, 1200 games

Six anchors, 20+0.2, one thread, 64 MB hash, `OwnBook` and `Ponder` forced
off. Two games out of 1200 ended on time (0.17%) and none crashed or played
an illegal move, so the spread below is a property of the anchors, not of
corrupted results.

| anchor | published | haVoc score | implies |
|---|---|---|---|
| Zurichess Fribourg | 2412 | 88.0% | 2758 |
| Arasan 12.2 | 2505 | 88.0% | 2851 |
| Phalanx XXIV | 2521 | 80.2% | 2764 |
| Fruit 2.1 | 2694 | 76.2% | 2896 |
| Glaurung 2.2 | 2793 | 59.2% | 2858 |
| Senpai 1.0 | 2985 | 29.8% | 2836 |

Senpai 1.0 was added for this run. The previous set topped out at Glaurung
2793; at this strength a fit against it would have been extrapolating past its
highest anchor, which is the defect that invalidated every superseded row
below.

The analyser warns that the 139-point spread exceeds its threshold, and the
warning is correct, but it does not threaten the central value. Every
defensible subset agrees:

| subset | estimate |
|---|---|
| all six | 2834 ±12 |
| excluding Arasan (the outlier in the 2582 run too) | 2832 ±13 |
| only anchors scored between 25% and 75%, where the logistic model is least saturated | 2848 ±18 |

The disagreement is between the anchors' published ratings, not about where
haVoc sits. Zurichess and Arasan yield identical 88.0% scores while their
published ratings differ by 93 points, so at least one of those two figures
does not describe the binary built here. Arasan implied high in the 2582 run
as well, which points at the Arasan build rather than at anything that moved
this time.

### The prediction was wrong, and the naive method beat the careful one

Before the run two estimates were on record:

| method | prediction | error |
|---|---|---|
| naive sum of self-play SPRT gains, 2582 + 261 | 2843 | **+9** |
| discounting those gains by a 0.6–0.75 "transfer factor" | 2690 | **−144** |

Restricting the new fit to the same five anchors used for the 2582 run, to
compare like with like, gives 2834 ±14, so the measured gain is **+252**
against the +261 the self-play SPRTs claimed.

The reasoning behind the discount was that self-play overstates gains, that
Elo is not additive across a moving baseline, and that fixed-depth results do
not transfer to time control. Each of those effects is real. The conclusion
drawn from them was still wrong, because the correction was applied with no
calibration data — it substituted a plausible story for a measurement and
happened to be worth −144 Elo of pessimism.

For now, treat the sum of self-play SPRT gains as an *unbiased* predictor of
gauntlet movement in this rating range, and do not discount it again without
evidence. One paired data point is thin, so the next gauntlet is a direct test:
if the naive sum is accurate a second time, this stops being a coincidence.

A caveat that cuts the other way: these games are blitz while the anchor
ratings come from 40/40, and older engines generally gain relative strength at
long time control. That systematic offset is not in the ±12 and is the reason
the README quotes ±50.

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
