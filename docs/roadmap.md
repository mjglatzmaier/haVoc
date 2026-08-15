# Roadmap

Written for whoever — human or agent — picks this up next. It is a summary of
the state of play, what has been tried, what to avoid, and where the remaining
value is. The detailed evidence behind every claim is in
[`revisit-after-tuning.md`](revisit-after-tuning.md); the practical setup steps
are in [`handoff.md`](handoff.md).

---

## 1. Where the engine stands

haVoc is a hand-crafted-evaluation (HCE) alpha-beta engine, currently around
**2500 Elo** on the CCRL scale. `bench` is **697583 nodes** and there are **133
passing tests**. Both are exact checksums — see `handoff.md`.

The single most important finding of recent work:

> Large, well-motivated changes to the hand-crafted evaluation keep measuring
> inside the noise. The correctness work paid; the knowledge work has not.

A coherent reset of 59 evaluation parameters onto consistent scales, removing
double-counting and un-scaling knowledge that had been scaled away, measured
**+14.5 ± 26.9** at fixed time and **−4.5 ± 25.8** at fixed depth. That is a
genuinely neutral result for a change that should have been worth something if
the evaluation were the binding constraint.

Take this as evidence about where effort belongs, not as a counsel of despair.
It argues for search work and for the NNUE/attention track, and against further
rounds of HCE parameter polish.

---

## 2. Non-negotiables when measuring

Most wasted effort in this project has come from trusting a measurement that
could not support the conclusion drawn from it. In rough order of how much time
each one cost:

**Do not read the Elo column of cutechess's gauntlet ranking table.** It is
normalised across the pool, and in a gauntlet the anchors never play each other,
so its gaps are not pairwise-consistent. Subtracting them once turned a genuine
203-point head-to-head gap into a reported 420 and made a ~2500 engine look like
~2300. Use `scripts/testing/anchor_rating.py`, which works from raw results.

**Do not use bench node count as an optimisation target.** It is deterministic,
which makes it look like a clean objective, but it responds chaotically to search
parameters: sweeping `rfp_margin` gives 539240 at 70, 953544 at 80 and 826577 at
90. That is real search instability, not noise. Bench is a checksum, not a fitness
function.

**Do not declare an evaluation term dead on a small sample.** The fraction of
parameters that change no move decision falls from **69% at 300 positions to 42%
at 2000 and 17% at 10000**, and was still falling. Thin sampling manufactures
dead parameters, and it does so preferentially for rare-but-decisive knowledge.
Use at least 10000 positions.

**Attribute by family, not by individual parameter.** A table that moves +7 on
one entry and −4 on another nets out and loses to an unrelated scalar. Adding
family aggregation to `eval_bench` moved corpus attribution from 7/14 to 9/14 and
reversed a conclusion about king safety.

**Self-play overstates field Elo.** A change measured against the engine's own
previous revision routinely fails to appear against outside opponents. One batch
measured +52 in self-play and did not show up in the gauntlet at all.

**A single SPRT cannot catch accumulated drift.** Changes measuring −5 ± 25 merge
as neutral under policy; several in a row can be real regressions. Periodically
SPRT `main` against a snapshot 5–10 PRs back.

The first such check was run against PR #67, seven PRs back, at 10+0.1. It was
stopped early at **−13.0 ± 27.2 over 494 games** to move to faster hardware, and
was recorded as unresolved and mildly negative.

It has now been repeated on the faster machine, `main` against #69, again at
10+0.1 and again seven PRs of accumulated change. **There is no regression:
−3.4 ± 16.1 over 1357 games** (499−519−339, LOS 34.1%). The concern is closed.

The two runs together are the useful artefact, because both of them lied early
and in opposite directions. The first read **+39.3 ± 67.5 with LOS 87.5%** at 86
games and was reported as "leaning positive, no accumulated regression"; it
reversed completely by 400. The second read **−52 Elo** at 120 games and was
reported as a real regression; it decayed monotonically — −13.3 at 877, −9.1 at
1159, −3.7 at 1300 — to nothing. Neither early reading was a weak version of the
final answer. Both were noise with a confident sign attached, and in each case
the sign was the thing that got reported. This is the same trap that manufactures
dead evaluation parameters at 300 positions. Do not report a direction from a
drift check below roughly 800 games; report that it is running.

What this does *not* establish is that the last seven PRs were individually
sound. A ±16 interval still admits several −5 merges hiding inside it. It
establishes that the accumulated delta is not large enough to be worth chasing,
which is the question the check exists to answer.

---

## 3. What has been tried

### Worked

- **Correctness fixes.** Zobrist and material key desync, phase interpolation,
  aspiration window handling, TT bugs. This is where the measurable Elo came
  from, and it is the reason the roadmap starts with correctness.
- **King shelter re-indexing.** It was indexed by a *count* of pawns in the eight
  squares around the king, giving four expressible states, one 6cp step between
  perfect and broken, and no way to tell h3 from h4. Re-indexed by distance per
  file over the king file and its neighbours. Worth **−27% bench nodes**, an
  unpredicted and large win. The magnitude is still unresolved (see below).
- **Building instruments instead of arguing.** `eval_sensitivity` (what is a term
  worth), `eval_bench` (does the evaluation discriminate, and why), and
  `--decisions` (what actually changes a move) each settled questions that had
  been circular. When stuck, the fastest route has consistently been to build the
  measurement rather than reason harder.

### Did not work

- **Texel tuning.** Covered in section 5 — do not retry it.
- **Piecemeal margin raises.** Individually plausible, measured −85, −37, −22 and
  −12.6 Elo. The margins are coupled; they must be moved together by a search.
- **TT prefetch.** Node-identical and measurably faster, yet **−14 Elo**. Unexplained
  and worth understanding before any similar micro-optimisation is trusted.
- **Hand-halving the shelter values.** Restores the old dynamic range but loses
  both the discrimination (9cp → 4cp) and the node win (697583 → 790209). The
  effects cannot be separated by hand.
- **Link-time optimisation.** The obvious free win: `havoc_core` is a static
  library, so nothing in `search.cpp` can currently inline into `eval/hce.cpp` or
  `movegen.cpp`, and LTO costs only build time. Measured on an idle machine,
  pinned to one P-core to keep the scheduler off the E-cores, 20 interleaved
  bench runs per configuration: **+0.28% nps, SE 0.81%, t = 0.35**. Node count
  identical at 697583, as expected. There is no speed win here to have. The
  binary does shrink 19% (425KB → 346KB), which is not worth a build-system
  option on its own. The likely reason — untested — is that the cross-TU calls
  are once-per-node into large callees, where inlining buys nothing; the
  per-node inner loops are already within a single translation unit. Note also
  that faster does not imply stronger in this engine: see TT prefetch above,
  which was node-identical, genuinely faster, and −14 Elo.
- **Obscure engines as rating anchors.** GopherCheck and Rusty-Rival implied 2284
  and 2507 for the same candidate. Sungorus 1.4 implied 2192 where Fruit and
  Glaurung implied 2429 and 2397 in the same run, despite 1583 CCRL games behind
  its rating.

---

## 4. Paths forward

Ordered by expected return per unit of compute.

### 4.1 SPSA the search margins, with games as the objective

The clearest remaining headroom. Search is demonstrably **not saturated**: 29% of
positions change their preferred move at depth 8, 22% at depths 10 and 11, 12%
still at depth 12, and the median settle depth is 9. Nodes are therefore
genuinely valuable, and the margins decide how they are spent.

Bench is unusable as the objective (section 2), so this has to be games, which is
why it wants a bigger machine. A screening gauntlet over `rfp_margin` 60/100,
`fp_margin` 70/110 and `qs_delta_margin` 750/1050 found nothing grossly wrong,
with one weak but directionally consistent hint: `qs_delta_margin` 1050 measured
+46 and 750 measured −26, both pointing at larger being better. That is well
inside the error bars of a 40-game screen and needs a real SPRT.

### 4.2 SPSA the king shelter magnitude

Flagged when merged. The shape fix was right, but summing over three files
tripled the dynamic range as a side effect (+21/−45 against the old +6/−22), and
the merged variant measured −11.9 ± 31.8. Hand-tuning fails. This is a small,
well-defined search space and a good first SPSA target.

### 4.3 Move ordering

Untouched by the recent work and historically the highest-leverage area in an
alpha-beta engine. Measure the effective branching factor and the cutoff move
index distribution before changing anything.

### 4.4 Trim genuinely dead evaluation

Only after a `--decisions` run on **≥10000 positions**. The prize is not Elo
directly — it is nps and simplicity, which buy depth. Note that evaluation *code*
cost is small here (nps was flat at 291k vs 297k across the reset), so the case
for trimming rests on complexity, not speed.

Related: the three overlapping king-safety models should be collapsed properly
rather than by zeroing attacker tables, and `attackers_of2` is about **11% of
runtime**.

### 4.5 Fix the mis-attributed discrimination pairs

Five of fourteen are decided by `sq_score_category_scale` or `pst_mg_king` rather
than the term they were written to test. Each one is a small, concrete puzzle and
they tend to surface real evaluation defects — that is how the shelter bug was
found.

### 4.6 NNUE / attention

`feature/trainer` (27 commits, last touched March) is this line of work, developed
on separate hardware. Given that the HCE reset measured neutral, this is the most
plausible route to a substantial jump, and the target machine has the GPU and RAM
for it. Treat the HCE as the thing that must be *correct and fast* to serve as a
fallback and a datagen engine, not as the thing to squeeze for the next 100 Elo.

Prerequisite: the `datagen` fixes in the backlog.

---

## 5. Do not retry Texel tuning

It cannot help this evaluation, and the reason is structural rather than a matter
of setup, so a better fit or more data will not change it.

A parameter's gradient is **independent of its current value**. Zeroing
`bishop_king_distance_penalty` moved its measured worth from 1.94 to 0.00 while
its gradient moved only from 1.935 to 1.938. The optimiser therefore sees the same
landscape after any reset of the defaults, and its minimum has not moved. A refit
walks straight back to the fit that measured **+1.4 ± 19.9 Elo over 781 games**.

Compounding this, the error is dominated by material, and the quiet-position
filter discards the ~22% of positions where tactical terms fire — precisely the
knowledge worth tuning.

**Use it for material and piece-square tables. Nothing else.**

For the same reason, "reset the parameters to natural values and refit" is not a
plan. The reset was worth doing on its own merits — hygiene, no double-counting,
knowledge not scaled away — and it was done. It did not make the tuner useful.

### What to do instead

Games are the only objective that has proven trustworthy. That points at SPSA and,
if the compute is available, at population-based methods over self-play — which is
what the user independently identified as the honest version of this problem.

---

## 6. Open questions

- **Why is TT prefetch worth −14 Elo** while being node-identical and faster? Until
  this is explained, no micro-optimisation in that area should be trusted.
- **Why do two well-established anchors disagree by 70–90 points** about haVoc in
  the same run, in opposite directions across runs? Bracketing anchors have been
  added to find out; if the disagreement survives, it is real non-transitivity and
  it limits how precisely this engine can be rated at all.
- **What is the right shelter magnitude?** See 4.2.
- **Is the HCE ceiling near?** Three separate lines of evidence — the neutral
  reset, the material dominance of the argmax, and the flat response to
  magnitude work — point the same way. Worth confirming deliberately rather than
  concluding by accumulation.
- Current `main` runs about **10% lower nps** than the state seven PRs earlier
  while searching about 8% fewer nodes, so bench time is a wash. Whether that
  trade is favourable in games is exactly what the drift check measures, and the
  partial answer so far (−13.0 ± 27.2) is not encouraging. Note the trade is
  TC-dependent: the node saving should matter more at longer time controls and
  the nps loss more at shorter ones, so this deserves measuring at two TCs rather
  than one.
- **Does the accumulated delta over PRs #68–#74 come out negative?** Unresolved at
  494 games. If it does, the two structural merges accepted under policy —
  the coherent reset and the shelter magnitude — are the first place to look, and
  the policy itself needs revisiting rather than just those two changes.

---

## 7. Branch inventory

Merges are squashed, so most remote branches are delivered leftovers and can be
deleted. These never went through a PR and should be checked before being assumed
to contain undelivered work — most are probably superseded:

`exp/threat-escape-ordering`, `feat/king-zone-shelter`, `fix/aspiration-window`,
`fix/qsearch-fail-soft-delta`, `fix/see-material-sync`, `test/search-symmetry`.

`feature/trainer` is the NNUE/attention line and is live work on other hardware —
do not delete or rebase it.

---

## 8. Rules of engagement

These were adopted during the recent work and are worth keeping.

- **One commit per fix**, with a message that says what was wrong and how it was
  proven, not what was typed.
- **Structural** changes measuring neutral-to-slightly-negative may merge, and are
  recorded in `revisit-after-tuning.md`. **Pure parameter** changes must earn their
  keep with an SPRT.
- **Prefer building a measurement over extending an argument.** Every genuine
  advance recently came from a new instrument.
- **Record negative results.** Half of this document is things that did not work,
  and that half has saved more time than the other.
- **Run one CPU-heavy job at a time**, and never build or run tests during a match
  unless niced.
