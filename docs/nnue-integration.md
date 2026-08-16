# The incremental-evaluation seam

Tier 1 item 5 of [`neural-direction.md`](neural-direction.md). This document
records what the codebase already provides, what is actually missing, and the
order the work should be done in. It is written so that the seam can be added
while the HCE is still the only evaluator — which is the point: every step
below is testable against a cheap evaluation that we can compute both ways and
compare exactly.

## 1. Why this is needed at all

NNUE is affordable only because it is *incrementally updated*. The first layer
is a large matrix multiplied by a sparse, almost-unchanged input vector, so
instead of recomputing it each node you keep an **accumulator** and add or
subtract the few columns that changed. A move touches at most a handful of
(piece, square) features, so the per-node cost is a few hundred adds instead of
a full matrix product.

That only works if the engine can answer, cheaply and exactly: *which features
changed when this move was played?* Today it cannot — `position::do_move(const
Move&)` returns `void` and exposes no delta.

## 2. What already exists (verified, not assumed)

The good news is that the decomposition is already there and is already
centralised. Every board mutation in `include/havoc/position.hpp` goes through
exactly **three** primitives on `piece_data`:

| primitive | meaning |
|---|---|
| `do_quiet(c, p, from, to, ifo)` | one piece moves |
| `add_piece(c, p, sq, ifo)` | a piece appears |
| `remove_piece(c, p, sq, ifo)` | a piece disappears |

and every move type is composed from them:

| move type | decomposition |
|---|---|
| quiet | `do_quiet` |
| capture | `remove_piece(captured)` + `do_quiet` |
| en passant | `remove_piece(captured pawn, cs)` + `do_quiet` |
| promotion | `remove_piece(pawn, from)` + `add_piece(promo, to)` |
| promotion-capture | `remove_piece(captured)` + `remove_piece(pawn)` + `add_piece(promo)` |
| castle (either side) | `do_quiet(king)` + `do_quiet(rook)` |

So the *information* an accumulator needs is already flowing through three
functions. Nothing about `do_move`'s branching needs to be rewritten. The
missing piece is only that these primitives discard the delta instead of
recording it.

The bound is small and fixed: the worst case is promotion-capture at three
events, or castling at two piece-moves. A fixed-size buffer of four entries
covers everything, so this needs no allocation and no `std::vector`.

Note also that `do_quiet` already maintains `ifo.pawnkey` separately from
`ifo.key`. That is the same shape of problem — a second incrementally
maintained summary of the position — and it is maintained in exactly these
three primitives. The accumulator is that pattern again, just wider.

## 3. The seam

Add a small delta record, populated by the three primitives and consumed by
whatever evaluator wants it:

```
struct FeatureDelta {          // what changed, not what it means
    struct Event { Color c; Piece p; Square sq; bool added; };
    Event events[4];
    int   n = 0;
};
```

`add_piece`/`remove_piece` push one event; `do_quiet` pushes a remove and an
add. The HCE ignores it entirely, so the change is inert until an NNUE
evaluator reads it.

Two design constraints that matter for keeping this extensible:

- **`position` must not know what a feature is.** It reports
  (colour, piece, square, added/removed). The mapping to HalfKA/HalfKP indices,
  king buckets, and output buckets belongs to the evaluator. Otherwise the
  board is coupled to one network topology and changing the architecture means
  touching move-making.
- **The accumulator does not belong in `position`.** It is per-evaluator and
  per-search-thread state, and `position` objects are copied and constructed in
  tools and tests that never evaluate. It belongs alongside the other
  per-thread evaluation state on `Searchthread`, which is where
  `pawn_table`, `material_table`, `parameters` and the `IEvaluator` already
  live — and which, as of #110, is also where `Movehistory` lives.

`IEvaluator` gained four hooks with empty defaults, so the HCE is unaffected
and the interface stays honest about what it needs:

```
virtual bool wants_deltas() const { return false; }
virtual void push(const position& after, const FeatureDelta&) {}
virtual void pop() {}
virtual void refresh(const position&) {}
```

`wants_deltas()` is not decoration. It is cached on `Searchthread` and checked
before every hook call, so an evaluator that does not want deltas pays a
predicted branch instead of an indirect call that cannot be inlined away.

## 4. Order of work

Each step is verifiable on its own, against the HCE, before the next begins.

**Steps 1-4 are done and merged (#116, #117). The seam exists and is tested;
what remains is step 5, the network itself.**

1. ~~**Record the delta.**~~ Done in #116. `FeatureDelta` is populated by the
   three primitives; bench is 697583 nodes before and after, identical.
   **Measured free:** on an idle box, `-march=native`, interleaved runs of the
   pre-seam and post-seam binaries, 8 each — 1 871 386 nps against 1 864 740
   nps, a 0.36% difference inside a 3% run-to-run spread. Recording the delta
   and checking the cached `wants_deltas()` flag together cost less than the
   measurement can resolve, which is what the design needed.
2. ~~**Assert the delta is complete.**~~ Done in #116.
   `FeatureDeltaReproducesEveryMoveOnTheBoard` replays the recorded events onto
   the pre-move squares and requires the post-move squares exactly, over every
   legal move of a four-position walk, pinning the event count per move type
   and asserting a move-type histogram so it cannot pass without generating
   castling, en passant and all four promotion and promotion-capture types.
3. ~~**Wire the hooks.**~~ Done in #117. `push`/`pop`/`refresh`, called from
   four helpers in `SearchEngine` rather than from `position` directly, so a
   new call site cannot make a move without telling the evaluator.
4. ~~**A trivial incremental evaluator.**~~ Done in #117,
   `tests/test_incremental_eval.cpp`, driven by the **real search** so that
   pruning, re-searches, null moves and quiescence all exercise it.
5. **Still to do: the real accumulator, the network, and the king-bucket
   refresh policy.** See §7 for what the earlier steps established about this.

### What was learned doing it

- **`undo_move` runs the same primitives as `do_move`**, so it records the
  *inverse* events. The delta must therefore be cleared on the way in as well
  as on the way out. Getting this wrong overflowed the fixed buffer into
  adjacent board state — found by the step-2 test as a segfault. Recording now
  also drops surplus events rather than writing past the buffer, so the failure
  mode stays a visible count rather than corruption.
- **Hooks must be free when unwanted.** An unconditional virtual call per
  `do_move` is an indirect branch the compiler cannot inline away, taken
  millions of times a second for an empty body. Evaluators declare
  `wants_deltas()` once and `Searchthread` caches it.
- **The root arrives by a jump, not a move.** `refresh()` per thread at the
  start of every search is what stops an accumulator being a whole game out of
  date. `refresh` is called unconditionally, not gated on `wants_deltas()`, so
  an evaluator that keeps state without wanting deltas cannot be silently
  skipped.
- **Material alone is a weak oracle.** It is invariant under piece *movement*,
  so a material-only check accepts an evaluator that drops every quiet move or
  gets every square wrong — exactly what breaks a network. The test therefore
  also carries the exact active-feature set, which is the shape an NNUE input
  layer is indexed by.
- **Swapping the evaluator during a live search is a use-after-free.**
  `set_evaluator_factory` settles the engine first.
- **Comparison builds must have identical flags.** A first attempt at the nps
  measurement had the seam build "13% faster" than the baseline, which is not a
  thing adding stores can do. `HAVOC_NATIVE` was `ON` in one and `OFF` in the
  other, because pointing `cmake -B` at an existing directory reuses its cache
  rather than applying the defaults. Check `CMakeCache.txt` on both sides
  before believing any A/B.

## 5. Data pipeline status

The training data half of this is further along than the engine half.

`tools/datagen.cpp` produces labelled self-play positions and, after #109, does
so about 11x faster than before, with four correctness bugs fixed (see that PR
for the EPD `ce` point-of-view issue in particular). A validation run of 40 000
games at depth 8 produced 3 617 981 positions in 29 minutes — roughly 2 100
positions/sec — with 0 malformed rows, 98.6% distinct positions, and 18 games
abandoned out of 40 000.

At that rate the ~10^9 positions §10 of `neural-direction.md` calls for is
about 5.5 machine-days, which is the number to plan against. Depth, not
throughput, is now the open question: depth 8 labels are cheap but noisy, and
nobody has yet measured how label quality trades against volume here.

## 6. What this does not decide

The network architecture, the quantisation scheme, and the CPU inference kernel
are all still open, and §6 of `neural-direction.md` holds the candidate
designs. This document is deliberately narrower: it is about making the engine
*able* to support any of them, using a seam that is testable today.

## 7. Notes for step 5

The seam carries enough information for a HalfKA/HalfKP accumulator, but a few
contracts are worth stating before anything is built on it.

- **The delta reference is ephemeral.** `push` receives a reference to the
  board's single scratch buffer, which the next move overwrites. Consume it or
  copy it inside `push`; do not retain the reference.
- **A stack frame must save everything that is incremental**, not just a sum:
  both perspectives' accumulators, their king-bucket identifiers, and whether
  each is valid. `pop` restores the frame, which is why the search does not
  need to replay events backwards.
- **King moves may cross a bucket boundary.** The information is all present —
  `push` receives the post-move board, and a king move appears as a remove and
  an add naming both squares — but a bucket change means rebuilding that
  perspective from the position rather than updating it. Test bucket-crossing
  king moves against full recomputation specifically; the generic sync test
  cannot distinguish "refreshed correctly" from "updated correctly".
- **`refresh` must clear the whole frame stack**, not just recompute the
  current accumulator.
- **Network weights are immutable and may be shared across threads.
  Accumulators are per-thread**, and belong on `Searchthread` next to the
  evaluator, which is where the per-thread eval state already lives.
- **`wants_deltas()` must be fixed for an evaluator's lifetime.** It is cached
  when the evaluator is installed. If loading a network is what decides whether
  deltas are wanted, that decision has to be made in the constructor, or the
  evaluator has to be reinstalled through the factory afterwards.
- **The hooks are assumed not to throw.** The helpers pair them with
  `do_move`/`undo_move` directly rather than through an RAII guard, so an
  exception escaping a hook would leave the pair unbalanced. Nothing in the
  engine throws during search today; if that changes, make the pairing a guard.

---

## 8. The staged ladder for step 5

Step 5 is where the money is spent, so it is broken into stages that each end
in a **cheap, unambiguous verdict**. The ordering principle is that every
stage whose answer we already know comes before every stage whose answer we
do not, because a pipeline bug found on a known answer costs hours and the
same bug found on real labels costs days of GPU and CPU time.

| stage | what runs | expected answer | cost if it fails |
|---|---|---|---|
| 0 | train a net to reproduce the **HCE static eval** | converges to a small MAE | hours |
| 1 | quantise, write the C++ kernel, install the stage-0 net | C++ matches Python; SPRT vs HCE ≈ **0 Elo** | hours |
| 1b | accumulator vs full recompute, incl. king moves | exact agreement at every node | hours |
| 2 | label-quality study on real datagen scores | tells us the depth/volume tradeoff | a day |
| 3 | small net on real labels | first honest strength reading | a day |
| 4 | full-size net on the full corpus | the actual gain | days |

**Stage 0 is a known-answer test, and that is its entire purpose.** The target
is haVoc's own static evaluation: a deterministic function we already possess
and can label for free from any corpus of positions, with no self-play at all.
Everything except the idea itself is under test — the feature export, the
loader, the model shape, the loss, the optimiser, the checkpoint. A network
that cannot learn a function we can already compute is not evidence about
NNUE; it is evidence about our pipeline.

**Stage 1's expected result is zero Elo, and a large number in either
direction is a bug report.** Installing a network that mimics the HCE should
play like the HCE. If it is much worse, the quantisation or the kernel lost
the network. If it is much *better*, something is wrong with the comparison
rather than good with the network — most likely the two builds differ in
something besides the evaluator. This is the only stage in the plan where a
positive result should be distrusted.

### Where the feature encoding lives, and why only once

`include/havoc/nnue/features.hpp` is the sole definition of what a network
input index means. `tools/nnue_export.cpp` writes the indices it produces
directly into the training file, so the trainer receives features rather than
positions and **never parses a FEN**. This is not tidiness. A second
implementation of the encoding in Python is the classic way this project is
lost for a week: the two encoders agree on almost every position, the network
trains cleanly against whichever one made the data, and the disagreement
surfaces only as an engine that is inexplicably weak. Transporting the indices
makes that failure inexpressible.

The file carries `feature_set_version`, and the loader refuses a mismatch, so
data generated under an older encoding cannot be silently trained on.

`tests/test_nnue_features.cpp` pins the encoding before anything is trained on
it. The load-bearing cases are mirror invariance — a position and its
colour-and-rank reflection must produce swapped perspectives, which catches a
missing orientation, an orientation applied to the piece square but not the
king square, and an inverted own/enemy polarity — and full reconstruction of
the board from the indices, which catches a lossy encoding. A lossy encoding
still trains; it just caps how good the network can ever be, and it does so
without any visible symptom.

### What stage 0 measured

The pipeline works. Relabelling the existing 3.6M-position corpus with the HCE
took **1 second** (3M positions/sec, 0 rows skipped), and training on one
consumer GPU with the corpus held resident in VRAM runs at **1.4 seconds per
epoch** — which is why the capacity sweep below cost minutes rather than a day.

| L1 | held-out MAE vs the HCE |
|---|---|
| 128 | 27.9 cp |
| 256 | 26.7 cp |
| 512 | **22.2 cp** |

against a **303 cp** predict-the-mean baseline, at correlation 0.9965.

The residual is representational, not a defect. Error is concentrated in
endgames — 38 cp with six men or fewer, falling monotonically to 13 cp with
23–30 — which is exactly where the HCE stops being a smooth sum of piece terms
and starts consulting a KPK bitbase and mate-distance drives. Those are
discontinuous functions of piece placement, and HalfKP cannot express them as
a sum of per-piece contributions. Relative to the label spread the endgame fit
is actually the *better* one (38 cp against an 854 cp spread, versus 13 cp
against 170 cp). Nothing here needs fixing: real training labels are search
scores, in which the bitbase verdict is already baked in consistently.

### The 46 cp quantisation bug, and how it was found

Stage 1 immediately earned its place. The first quantised network disagreed
with its own float weights by **46 cp**, and the sequence that found the cause
is worth recording because the obvious hypothesis was wrong.

The error turned out to be almost pure **bias** — mean +45.7 cp against a
46.5 cp MAE, uniform across every evaluation band — which already ruled out
"random rounding noise" and pointed at something systematic.

- **Sweeping the activation scale QA over 255 → 16383 changed nothing**
  (46.11 → 45.82 cp), and rounding instead of truncating the rescale changed
  nothing either. Activation resolution was not the problem, which killed the
  first and most plausible theory outright.
- **Sweeping the dense weight scale found it.** At the shared scale of 64 the
  error was 46 cp; at 101 it was 10 cp; at int16 precision it bottomed out
  around 2 cp.

The cause: the scale had been fixed at 64 to match the clamp applied during
training (127/64 ≈ 1.98), but the trained network's largest weight was only
**1.25**, so two thirds of the int8 range went unused — and a third of `fc1`'s
weights, whose rms is 0.062, were smaller than a single quantisation step.

The fix is to fit **a scale per layer** from that layer's own peak weight
(`fc1`=101, `fc2`=156, `out`=141) and store the scales in the file, since they
are a property of the trained network rather than of the engine. That took the
error from **46 cp to 9.2 cp** at no runtime cost. Against a modelling error of
26.7 cp, 9.2 cp of quantisation noise is a second-order term, and real training
labels carry far more noise than that.

The general lesson is the one this project keeps relearning: **sweep the
parameter before believing the mechanism.** The activation sweep cost two
minutes and disproved the theory everything else would have been built on.

### Quantisation-aware training: tried, did not help

Rounding the weights onto the int8 grid inside the forward pass, with a
straight-through estimator, is the textbook answer to quantisation loss, and
at 1.4 s/epoch it was nearly free to test. It cut int8-versus-float error from
9.2 cp to 7.4 cp — but *raised* end-to-end error against the labels from
12.2 cp to 14.5 cp, because the constrained network fits the target less well.
It is kept behind `--qat`, off by default, and recorded here so it is not
retried on the assumption that it must help.

## 9. Stage 1: the network in the engine

Stage 0 proved a network could be trained, quantised and read back. Stage 1
puts it behind `IEvaluator` and makes the search drive it, which is where the
accumulator becomes a thing that can silently drift out of step with the
board.

### What was built

- `NNUEEvaluator` (`include/havoc/eval/nnue_evaluator.hpp`) — per-thread stack
  of accumulator frames, one per ply, patched by `push` and unwound by `pop`.
- `setoption name EvalFile value <path>` loads a network and installs the
  evaluator on every search thread; `value none` puts the handcrafted
  evaluation back. A network that fails to load leaves the engine playing with
  the evaluation it already had rather than refusing to start.
- An AVX2 kernel for the dense layers, dispatched to only when the layer widths
  suit it, with `forward_reference` kept as the definition it is checked
  against.

The HCE path is untouched: `bench` is 709908 with and without the branch.

### Correctness, and what each check can actually see

| check | what it would catch |
|---|---|
| accumulator vs full rebuild at every node of a real search, five position families | any desynchronisation, at the node it happens on |
| every legal king move walked by hand, plus one reply | bucket crossing, which a search is free to prune away entirely |
| mutation: king events stripped from the delta | a king move that failed to rebuild its own perspective |
| mutation: removals stripped from the delta | a lost update on the perspective that was *not* rebuilt |
| 20,000 recorded cases replayed against `quantise.py` | any divergence between the C++ and Python integer paths |
| vector kernel vs `forward_reference`, 20,000 random accumulators | a SIMD kernel that is only nearly right |

The two mutation tests are the load-bearing ones. Rebuilding a perspective is
always correct, so a sync check alone cannot distinguish "updated correctly"
from "rebuilt anyway" — it can only prove something if deliberately breaking
each half makes it fail. Both do.

The cross-language replay agreed on **20,000 of 20,000** positions exactly.

### Speed, measured rather than assumed

The scalar reference was never going to be fast enough, and was not:

| kernel | ns per forward | bench nps |
|---|---|---|
| scalar reference | 1174 | 657k |
| AVX2, one output row at a time | 481 | 959k |
| AVX2, four output rows blocked, stack scratch | 347 | — |
| AVX2, dense weights pre-widened to int16 at load | **284** | **1047k** |
| (handcrafted evaluation, for comparison) | — | 1841k |

Three things that were tried and measured as worthless are worth recording so
they are not retried: **eight-row blocking** (229 ns against 226 ns for four),
**an explicit packs/permute/clamp for the accumulator narrowing** (identical to
the plain loop — the compiler was already vectorising it), and `thread_local`
scratch vectors, which were *worse* than plain stack arrays because a
`thread_local` with a non-trivial constructor pays a guard check on every
access.

`_mm256_maddubs_epi16` would double the products per instruction and is what
most engines use, but it accumulates into int16 and **saturates**: with
activations in [0, 255] and weights in [-127, 127] a pair of products reaches
64,770 against a 32,767 ceiling. Engines that use it clip activations to
[0, 127] instead, where `127·127·2 = 32,258` fits exactly. Since the Stage 0
sweep showed activation resolution to be worth nothing (QA 255 → 16383 changed
the error by 0.3 cp), dropping QA to 127 is very likely free and would unlock
it. That is the next optimisation, together with exploiting the sparsity of the
clipped accumulator; neither is on the critical path for the staged plan.

### What the Stage 1 SPRT can and cannot say

The original expectation written here was "~0 Elo, and a large number in either
direction is a bug report". That was wrong in one respect, and the measurement
above is why: **the network costs 43% of the engine's speed** (1841k → 1047k
nps). At 10+0.1 that is worth roughly −50 Elo on its own, before the network's
26.7 cp modelling error and 9.2 cp of quantisation noise are counted at all.

So the honest Stage 1 expectation is a clear *loss*, and the informative
quantity is not the Elo but how it decomposes. A result far worse than the
speed cost plus the eval noise would mean something is wrong that the tests
above did not see; a result better than the speed cost alone would mean the
comparison is not measuring what it claims.

## 10. What Stage 1 actually found: the held-out number was a lie

The Stage 1 match was supposed to be a formality. It was not, and this section
is the most useful thing in this document.

### The measurements

| comparison | result |
|---|---|
| NNUE-mimic vs HCE, 10+0.1, 200 games | **−252 ± 52 Elo** |
| NNUE-mimic vs HCE, **fixed depth 8**, 200 games | **−111 ± 45 Elo** |

Fixed depth removes the 43% speed cost from the comparison, so roughly 140 Elo
was speed and 111 Elo was the evaluation itself. A network reproducing the HCE
to 26.7 cp should not lose 111 Elo at equal depth, so either the integration
was wrong or the 26.7 cp was.

### The 26.7 cp was

Relabelling positions taken from **real games** with the HCE and running the
same network over them gave **82 cp**, not 26.7. Nothing in the C++ was
involved in either number, so this was not an integration fault — and the
integration tests, including a 20,000-position exact replay of the Python
quantiser, all passed.

The corpus is written by self-play datagen in game order. Measured on it:

- adjacent records share **85%** of their features (median Jaccard) — they are
  consecutive plies of one game;
- records 1,000 apart share **0.8%**;
- **11.5%** of feature vectors occur more than once outright.

A random train/validation split therefore places nearly every validation
position's near-twin, and often its exact twin, into the training set. The
26.7 cp was measuring memorisation.

### Three numbers for the same network

| validation protocol | MAE |
|---|---|
| random split of the corpus | 26.7 cp |
| contiguous tail block, 200k-record gap, exact duplicates dropped | 48.7 cp |
| independent positions from real games | **67–71 cp** |

Both effects are real and separable. The gap between the first two rows is
**leakage**; the gap between the last two is **distribution shift** — a
held-out block of a datagen corpus is still a datagen position, and the engine
does not play datagen positions. `train.py` now defaults to the contiguous
protocol and takes `--val-file` for the third, which is the only one that is
honest by construction and is what every real run should use.

### What is actually limiting the network

With the honest measurement in place, two sweeps say the same thing.

**Data (L1=256, 25 epochs, validated on game positions):**

| positions | MAE |
|---|---|
| 0.36M | 139.8 cp |
| 0.9M | 100.6 cp |
| 1.8M | 83.8 cp |
| 3.6M | 71.3 cp |

Every doubling buys about 15%, with no sign of flattening.

**Capacity (3.6M positions, 25 epochs):**

| L1 | MAE |
|---|---|
| 64 | 68.3 cp |
| 128 | 68.1 cp |
| 512 | 67.1 cp |
| 1024 | 67.0 cp |

Flat. Sixteen times the first-layer width buys 2%.

The network is **data-limited, not capacity-limited**, and it is not close. At
~15% per doubling, 30M positions lands near 45 cp and 100M near 35 cp; datagen
runs at 2,100 positions/second, which makes 30M about four hours and 100M about
thirteen. That is the lever, and it is cheap.

The capacity result has an immediate practical consequence: **L1=64 costs a
quarter of L1=256 in the dense layer** — which is where all the evaluation time
goes — and at present data volumes evaluates just as well. Stage 3 should start
small and grow the network only when the data justifies it, rather than the
other way round.

### Verdict on Stage 1

The integration is correct: every exactness and synchronisation test passes,
the C++ and Python integer paths agree on 20,000 of 20,000 positions, and the
Elo loss is fully accounted for by 43% of the speed plus 70–80 cp of evaluation
error. The stage did exactly what a known-answer test is for — it failed on the
thing that was actually wrong, which was the answer, not the code. Finding this
now cost an afternoon; finding it after a full training run would have cost
days and would have been blamed on the labels.
