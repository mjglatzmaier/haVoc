# Plan to 3000

Written 2026-08-17, revised the same day once the gauntlet came in at **2834
±12** on the CCRL 40/40 scale. This is the argument for how the engine gets to
3000+, in what order, and why the order is that way rather than the more
tempting one.

The short version: **3000 does not require an overhaul.** It requires collecting
Elo that is already identified and quantified, in an order that keeps the
measuring instrument ahead of the thing being measured. The original research
ideas are worth doing and should be done *last*, for reasons given below.

## 1. Where the engine stands

| | |
|---|---|
| Measured rating | **2834 ±12** fit, NNUE L1=256, 1200 games, 6 anchors, 2026-08-17 |
| Previous | 2582 ±12 fit, handcrafted evaluation, 1000 games, 2026-08-16 |
| Evaluation | HalfKP 40960→256×2→32→32→1, int8/int16, AVX2 |
| Corpus | 28.9M self-play positions, training iteration 4 |
| Speed | ~1.83M nps single-threaded with NNUE |

Two things about that speed figure matter more than the number itself. It is in
the same range as engines rated several hundred points higher, and it is *not*
the bottleneck. That is the usual reason people conclude an engine needs
rewriting, and it does not apply here.

## 2. The estimate, and why the naive sum won

This section originally argued that the naive sum of self-play SPRT gains was
badly inflated. The gauntlet has since been run, so the argument can be scored
instead of asserted, and **it lost**.

Since the 2582 gauntlet, the SPRT-measured gains sum to roughly +261 Elo:
+93.6 ±19.2 for the first net over the handcrafted evaluation at fixed depth 8,
+106.8 ±18.3 for the net-labelled d8 arm, +60.8 ±22.0 for iteration 4. Adding
those to 2582 gives 2843.

Three reasons were given for discounting it:

- **Self-play overstates.** Every one of those figures is haVoc against haVoc:
  same search, same book, same blind spots. A change exploiting a weakness both
  sides share reads larger than a foreign field will score it.
- **Elo does not add across moving baselines.** Compounding assumes
  transitivity, and this engine is measurably non-transitive: Arasan and Phalanx
  are rated 16 points apart and disagree by ~100 Elo about us.
- **Fixed-depth measurements flatter NNUE**, because they hand it its accuracy
  without charging the speed it costs. The honest time-control number for that
  first step is +75.9 ±18.9 at 10+0.1, not +93.6.

That produced a "defensible chain" of ≈+137 self-play, discounted by a 0.6–0.75
transfer factor to +90–115, giving **~2690, 80% interval 2640–2740**.

The measurement, on the same five anchors as the 2582 run: **2834 ±14, a gain
of +252.** The naive sum was off by 9 Elo. The careful correction was off by
144, and outside its own 80% interval.

The three effects above are all real. The error was applying a numeric discount
for them with nothing calibrating its size — a plausible story standing in for
a measurement, worth −144 Elo of pessimism. Whatever offsets those effects
create, they evidently cancel against something in this range (most likely the
blitz time control used here, which favours the modern engine against anchors
rated at 40/40).

**Working rule until contradicted:** treat the sum of self-play SPRT gains as an
unbiased predictor of gauntlet movement, and do not discount it again without
calibration data. This rests on one paired observation, so the next gauntlet is
a genuine test of it rather than a formality.

## 3. The Elo budget to 3000

At a measured 2834, **+166 is needed**, not the +310 this section was written
against. The identified levers:

| lever | estimate | confidence |
|---|---|---|
| More self-play iterations and far more data | +100 to +150 | high |
| NNUE architecture: output buckets, king buckets, wider L1 | +80 to +120 | medium-high |
| Search tuning with adequate statistical power | +30 to +50 | high |
| Search refinement: LMR terms, pruning, time management | +40 to +70 | medium |

Sum: +250 to +390, against +166 required. The budget now has roughly a factor
of two of headroom rather than bracketing the target, which means 3000 no
longer depends on every lever paying out near the top of its range — and that
the first two levers alone would do it.

Two cautions against reading that as "3000 is easy". The gauntlet number
carries a systematic uncertainty of about ±50 that the ±12 does not express, so
the true starting point may be nearer 2790. And Elo gets harder to buy as it
accumulates: these estimates were sized against a 2690 engine, and the same
work is worth less at 2834.

**The strongest single signal is that the network is nowhere near its
plateau.** It is only training iteration 4, on 28.9M positions, where modern
networks use hundreds of millions to billions. And the per-iteration gains are
not decaying: +93.6, then +106.8, then +60.8. An engine approaching its data
ceiling shows shrinking iteration gains. Ours has not started.

The second strongest is that two defects are already diagnosed and unfixed: the
depth-indexed search constants are calibrated to a depth-counter bug that no
longer exists, and the tuner cannot resolve anything below 8–10 Elo per
parameter. That is known, quantified Elo that nobody has collected.

## 4. Why the grind comes before the original work

The tempting order is to do the interesting research now and the mechanical
work later. That order is wrong here, for three reasons.

**A novel idea tested on a weak baseline is measured against a weak control.**
With +300 Elo of *standard* technique missing, an original heuristic that gains
+15 cannot be distinguished from one that is merely filling a hole a standard
technique would fill better and more cheaply.

**Work layered on a shifting foundation gets invalidated.** The search constants
are calibrated to a bug. Any clever pruning or ordering scheme tuned against the
current search may evaporate the moment the search is retuned — and then it is
unclear whether the idea was wrong or the measurement was.

**The instrument has to lead.** Our SPSA has a measured resolution floor of
8–10 Elo per parameter: score standard error is 0.063 at 32 games an iteration
while one Elo is worth 0.00144 of score. Research ideas that are worth 5–15 Elo
are *invisible* to that instrument. Fixing tuning power is therefore not
housekeeping, it is a prerequisite for being able to evaluate research at all.

The synthesis: **grind first, but choose the grind so that much of it is
enabling engineering rather than parameter-nudging.** The NNUE inference work
(AVX-VNNI, sparse propagation, lazy accumulator updates) is genuine engineering
with real intellectual content, and it *unlocks* the architecture work rather
than competing with it. That is the sweet spot, and it is where the interesting
near-term work lives.

## 5. Stages

Each stage has an explicit gate. A stage that fails its gate stops rather than
being carried forward on optimism.

### Stage A — fix the instrument
`tune-fixed-nodes`, `tune-power-floor`, `tune-fewer-params`

Switch tuning matches to fixed nodes rather than fixed time, which removes
timing variance and makes a tune reproducible; nps is irrelevant for
search-shape parameters. Tune fewer parameters per run so the per-parameter
budget rises. Document the floor so nobody re-derives it.

*Gate:* a deliberately injected 15 Elo parameter change is recovered by the
tuner. If the instrument cannot find a known effect, it cannot find an unknown
one.

### Stage B — scale the data
`nnue-data`, further self-play iterations

The largest single lever. Target 200M+ positions and iterations 5–7. Training
is GPU work and does not contend for the CPU box; datagen does.

*Gate:* per-iteration gain stays above +20 Elo. When it falls below that, the
data lever is exhausted and Stage C becomes the priority.

### Stage C — inference speed
`wl1-profile` → `wl1-vnni`, `wl1-sparse`, `wl1-mmap`, `wl1-lazy-acc`

Profile first: apportion the 47% L1=1024 slowdown between dense arithmetic and
feature-transformer memory traffic. That single measurement decides whether the
rest is worth doing. The CPU has `avx_vnni` and no AVX-512, while
`network.hpp` branches only on `__AVX2__`, so the dense loop currently spends
three instructions where `_mm256_dpbusd_avx_epi32` spends one.

*Gate:* L1=512 runs at ≥90% of L1=256 nps. Without that, wider networks stay
off the table and Stage D is limited to bucketing.

### Stage D — architecture
`res-topology`, output buckets, king buckets / HalfKAv2

Output buckets by piece count are cheap and standard. King buckets are the
larger change. Wider L1 only if Stage C passed its gate.

*Gate:* each change SPRTs positive independently before being stacked.

### Stage E — search refinement
`search-retune-nnue-2`, `max-ply-raise`, LMR/pruning terms, time management

Cheap in machine time relative to the stages above, and now measurable because
Stage A fixed the instrument.

### Stage F — original work
`res-policy-head`, `res-learned-pruning`, `res-sparse-accum`

Last, deliberately. By this point the baseline is strong, the instrument can
resolve 5 Elo, and a positive result means the idea is genuinely good rather
than filling a gap standard technique had left open. This is also where the
work is most likely to be publishable or novel, which is precisely why it
deserves a trustworthy control.

## 6. Execution model: batching

The contended resource is the **CPU box**, needed by datagen, SPRTs and
gauntlets. Training is GPU and does not compete. Code changes cost no machine
time at all.

So the working pattern should be:

1. **Between windows (no box):** write code, implement architecture changes,
   prepare the SPRT queue, do analysis and documentation.
2. **In a window (box for 1–2 days):** run datagen, then drain the batched SPRT
   queue back to back, then a gauntlet if a release is due.
3. **Continuously (GPU only):** train.

This is strictly better than holding the box continuously, which is what
happened during v2.3. One window per training iteration is enough, and each
window should begin with a written queue of what it will run, so the box is
never idle waiting for a decision.

## 7. What would actually require an overhaul

Above roughly 3200–3300 the following stop being optional: staged and
incremental move legality, a genuinely sophisticated NNUE inference path, and
training infrastructure at a scale that becomes its own engineering project.
None of that is needed for 3000. The inference work in Stage C is a *module*
rewrite, not an engine rewrite, and the board representation and move
generation survive untouched.

## 8. The assumptions most likely to be wrong

Listed so that they can be checked rather than discovered.

- **Self-play may stagnate.** The +100–150 for data assumes the loop keeps
  behaving. Bootstrapped self-play can plateau when the network begins learning
  its own artifacts, and that would only become visible after two or three more
  iterations. If it happens, that lever could be worth half the estimate. This
  is the single assumption most likely to break.
- **The transfer factor was a rule of thumb, and it was wrong.** *Settled by the
  v2.3 gauntlet.* The 0.6–0.75 discount predicted 2690; the measurement was
  2834, outside the stated interval, while the undiscounted sum was accurate to
  9 Elo. See §2. The replacement working rule — self-play SPRT sums are
  unbiased — rests on a single paired observation and is itself now the
  assumption to check at the next gauntlet.
- **The anchor set no longer bracketed the engine.** *Addressed.* Senpai 1.0
  (2985) was added before the v2.3 run, so the fit interpolated rather than
  extrapolated. At 2834 this is already tightening again: only one anchor is
  above the engine, and the next gauntlet needs something near 3000–3100 to
  keep the bracket. Under the previous five-anchor set the v2.3 number would
  have carried the same known flaw that made every pre-2582 rating
  untrustworthy.
- **Architecture gains assume the speed problem is solvable.** If Stage C fails
  its gate, +80 to +120 becomes perhaps +30, and 3000 moves out by an iteration
  or two rather than becoming unreachable.
