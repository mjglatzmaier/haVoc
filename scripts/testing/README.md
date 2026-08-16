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

## How many games to spend

The hard constraint is precision per unit time, and it is worse than intuition
suggests. Measured on the 24-core box at `10+0.1` with `CONC=30`, roughly 55
games/minute, at the draw rate this book produces:

| games | 95% CI | wall clock |
|---|---|---|
| 1000 | ±16.0 Elo | 18 min |
| 2000 | ±11.3 Elo | 36 min |
| 3000 | ±9.2 Elo | 55 min |
| 4000 | ±8.0 Elo | 73 min |
| 8000 | ±5.7 Elo | 2 h 25 |
| 16000 | ±4.0 Elo | 4 h 51 |

Read the last two rows carefully. **An 8000-game run resolves ±5.7 Elo.** A
change genuinely worth +3 Elo needs 30–60k games — twelve to twenty-four hours
— before a single test can distinguish it from nothing. So running every
candidate to 8000 games is not merely expensive, it does not answer the
question either. It buys a number that still contains zero.

The way out is not more games per change. It is to stop asking each change to
prove its own gain, and instead ask each change not to do damage, then measure
gain in batches where the effect is large enough to see.

### Tier 0 — no match at all

Docs, tests, CI, refactors, and any change whose bench node count is
byte-identical. An identical bench is a *stronger* proof of inertness than any
match could provide, and it is free. Do not spend games on it.

### Tier A — sanity check, cap 1200 games, ~20 min

Every functional PR. `elo0=-10 elo1=0`, a non-regression test. It is not
trying to prove the change is good; it is trying to catch the change being
bad, which is a much cheaper question.

- LLR crosses the upper bound → merge.
- LLR crosses the lower bound → do not merge; investigate.
- Cap reached undecided → merge only if the point estimate is ≥ −8 Elo **and**
  there is independent evidence: fewer bench nodes, a mechanism that explains
  the gain, and a clean review.

That last clause is what makes the tier safe, and it is a real gate. A change
with no evidence beyond "it measured neutral" should be parked as an issue, not
merged. Tier A cannot tell those two cases apart, so the judgement has to come
from somewhere else.

### Tier B — cumulative measurement, 3000 games, ~55 min, every 7–10 PRs

Current `main` against the previous Tier B tag. This is the number to report as
progress; no Tier A result should ever be quoted as an Elo gain.

Individually a change can measure neutral — say −5 ± 25 — and still be merged
under the rules above. Ten such merges can hide a real regression that no
single run was ever powered to see. Batching fixes that: eight PRs worth +3
each is +24 Elo, which 3000 games resolves comfortably, and it resolves in one
direction rather than as a scatter of overlapping intervals.

### Tier C — 8000+ games

Release candidates, or one specific change that has to be banked as a known
quantity. Rare, and scheduled deliberately rather than reached by default.

### Standing rule

One machine-occupying job at a time. A second match, a build, or a datagen run
sharing the box does not just slow an SPRT down — the test is a *timing*
measurement at a fixed time control, so competing load corrupts the result
rather than merely delaying it.

## Parameter optimisation

```sh
scripts/testing/spsa.py --engine ./build/havoc \
    --param king_shelter_0=-15:-60:20 --param king_shelter_1=7:-30:40 \
    --param king_shelter_2=1:-30:40   --param king_shelter_3=-4:-40:30 \
    --iterations 60 --games-per-iter 32 --tc 10+0.1 --tag shelter
```

Games are the objective. Bench node count is not usable as a proxy — it moves
non-monotonically in the search margins — and the Texel error is independent of
the values it is supposed to be fitting, so neither can stand in. See
`docs/roadmap.md` section 5.

SPSA needs two objective measurements per iteration no matter how many
parameters are being tuned, which is the only reason a games-based objective is
affordable. Both measurements come from one match: every parameter is perturbed
at once by `+c*delta` and by `-c*delta`, and those two parameter sets play each
other. The match score *is* the difference between the two measurements. Both
sides are the same binary with different `ParamFile`s, so no code delta can leak
in.

Two things to get right:

- **`--a-frac` is calibrated against `c`, not against the range.** The update is
  `a_k * diff / (2*c_k)`, so a step expressed directly as a fraction of the
  range is divided by the perturbation and comes out far smaller than intended.
  Set this way round, `--a-frac 0.08` means "a hypothetical 100%–0% match moves
  the parameter by 8% of its range on iteration 1", and the `1/c_k` scaling that
  makes the estimate a gradient is kept. The first version of this script used
  the obvious reading and moved an 80-wide parameter by 0.09 per iteration,
  which is indistinguishable from not running at all.
- **The output is a starting point, not a result.** SPSA optimises a noisy
  objective and will happily report movement that is noise. Always finish with
  `sprt-paramfile.sh` against the compiled-in defaults.

State is written after every iteration and `--resume` continues from it, which
matters on a machine that can lock up under a long run.

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

### Building the anchors

```sh
scripts/testing/fetch-anchors.sh            # all five, skips what exists
scripts/testing/fetch-anchors.sh --force arasan
```

It installs into `$REFENGINES` (default `~/code/refengines`), pins every
version, applies the patches these 2005-2015 codebases need on current
compilers, and refuses to report success for a binary that does not answer
`uci` — a mis-built anchor would otherwise be scored as losing every game and
would drag the fit down silently. It provisions a local Go toolchain if none is
installed. Do not run it during a match; builds are CPU-heavy and SPRTs are
timing measurements.

This script exists because the anchors were lost once, to a reboot, having been
built by hand into a path that did not persist. Four of the five had no recorded
recipe anywhere, so every absolute rating published here rested on binaries that
could not be reproduced.

Provenance, since most of these are no longer where they were published:

| engine | source | note |
|---|---|---|
| Fruit 2.1 | `github.com/rwbc/Fruit-2.1` | original host wbec-ridderkerk.nl is dead |
| Glaurung 2.2 | `github.com/phenri/glaurung` | personal mirror; no upstream remains |
| Arasan 12.2 | `arasanchess.org` | author's own site, still maintained |
| Phalanx XXIV | SourceForge | Debian ships XXII and XXV, never XXIV |
| Zurichess Fribourg | `github.com/easychessanimations/zurichess` @ `c2bcb164` | see below |

Zurichess needed recovering. Its Bitbucket Mercurial repository was deleted in
the June 2020 purge, Software Heritage never crawled it, the release tarball and
prebuilt binary were never captured by the Wayback Machine, and the author's
domain is now parked — so the release genuinely is gone from every location it
was ever published at. What survives is a git conversion pushed two months
before the deletion, carrying the full history. No tags came across, but the
release is identifiable by commit: `c2bcb164` is the last commit of the release
day, the two before it touch only the readme and the release script, the last
functional change is two weeks earlier, and `main.go` there reads
`buildVersion = "fribourg"`, which the binary prints. That commit is pinned in
the script.

Worth keeping in mind that this is the only anchor *below* haVoc, so it is the
one doing the bracketing. Mirror it rather than assume it will still be there.

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

## Correctness scripts

Two scripts in this directory check things a match cannot. Neither measures
strength; both are pass/fail, and both exist because the unit tests structurally
cannot reach what they cover.

| script | what it covers | why the test suite misses it |
| --- | --- | --- |
| `fuzz-uci.sh` | malformed UCI input | the tests drive the C++ interface, never the UCI parser |
| `tsan-search.sh` | data races between search threads | the tests are single-threaded, so nothing is ever contended |

Both run in CI on every PR. Run them locally against the matching sanitizer
build before proposing anything that touches parsing or threading:

```sh
cmake -B build-tsan -DCMAKE_BUILD_TYPE=RelWithDebInfo -DHAVOC_NATIVE=OFF \
      -DCMAKE_CXX_FLAGS=-fsanitize=thread -DCMAKE_EXE_LINKER_FLAGS=-fsanitize=thread
cmake --build build-tsan --parallel
scripts/testing/tsan-search.sh build-tsan/havoc full
```

`full` adds deeper and more heavily threaded searches than CI runs, which is
worth doing on a machine with the cores to contend with. If TSan aborts with
`unexpected memory mapping`, that is the known ASLR conflict on current kernels;
the script already reruns itself under `setarch -R` to avoid it.
