# Revisit after tuning

Structural work that was accepted on correctness or coverage grounds rather
than on measured Elo, and that should be re-examined once the evaluation has
actually been fit to data.

The reasoning behind the policy: a new evaluation term arrives with weights
that are a guess. Gating it on a self-play match against the untuned engine
tests the guess, not the term. A term that describes something real but is
weighted badly can measure flat or slightly negative and still be the right
thing to carry into tuning, because the tuner is what decides its weights. So
a structural improvement is accepted when it is sound, reaches the evaluation,
and does not regress badly -- and it lands here so the decision is revisited
with real numbers instead of being forgotten.

Each entry should say what to check and what would count as evidence to change
course.

> **New here?** Start with [`roadmap.md`](roadmap.md) for the state of play, what
> has been tried, and where the remaining value is; and
> [`handoff.md`](handoff.md) for setting up on a new machine. This file is the
> underlying evidence log they both cite.

## Evaluation terms whose defaults are guesses

| Term | Landed | What to check after tuning |
| --- | --- | --- |
| Connected pawns (`phalanx_pawn_*`, `supported_pawn_*`) | `eval/connected-pawns` | 22 registered entries across four rank-indexed tables. Defaults were chosen to sit in scale with the existing pawn terms, which are unusually small here (isolated 4, doubled 4, backward 1). Check the fitted values are not an order of magnitude off the hand-picked ones; if they are, the pawn terms as a group are probably on the wrong scale. |
| Queen mobility (`queen_mobility_*`) | `eval/connected-pawns` | 18 of 28 buckets are fit; the rest are coverage gaps. The curve was made deliberately flatter than the rook's on the argument that a cramped queen is not a large loss. That argument is a guess and the fit will either support it or not. |
| King safety constants (`pawnless_flank_penalty`, `king_storm_penalty`, `king_shelter`) | `fix/king-safety-untunable` | These were literals in the evaluation and are now parameters at exactly the values they had, so nothing about them was ever measured. `king_shelter` in particular spent its life halved by integer division, which means the array's tuned history was fit against a truncated version of itself; treat its current values as arbitrary. The storm penalty ignores how far advanced the storming pawns are, which is the signal that actually matters -- worth adding once the flat version has been fit. |
| `eval_threats` weights (`threat_by_pawn`, `threat_weak_pawn`, `queen_pin_*`, `discovered_check_bonus`, `restriction_weight`, `skewer_bonus`) | `fix/threat-weights-untunable` | An entire evaluation component that was frozen at unmeasured literals. `restriction_weight` is the one to watch: it multiplies a difference of attacked-square counts that can reach several dozen, so it is the highest-leverage single number in the file and a fit may want it well below 1, which the integer type cannot express. If the fit pushes it to zero, the term probably needs a divisor or the counts need scaling down. |
| ProbCut (`probcut_min_depth`, `probcut_margin`, `probcut_depth_reduction`) | `feat/probcut` | Search parameters, so SPSA rather than Texel. Measured node-neutral on bench (1884909 -> 1871476) with a 61% cutoff rate on entry, which says the technique works but that the verification search costs about what it saves at these settings. The margin and the reduction are the two knobs that decide whether it pays; if SPSA cannot find a profitable setting, the honest conclusion is that haVoc's tree is too shallow at fixed depth for ProbCut to earn its keep. |
| King zone open files (`king_open_file_penalty`, `king_semi_open_file_penalty`) | `integration/ordering-shelter` | Landed on a batched SPRT that measured +3.8 +/- 25.9 over 460 games, which is a statement about the batch and not about these two numbers. Both defaults are guesses. They also overlap the shelter term, which already charges for a missing pawn in front of the king: an open file beside the king implies no shelter pawn on it, so some of this penalty is being paid twice. Check whether the fit drives one of the two toward zero, which is what collinearity looks like. |
| Threat-escape move ordering | `integration/ordering-shelter` | Not a weight but an ordering rule: moves of the piece the null-move threat lands on are lifted by `kCounterMoveBonus`, a fixed eighth of the history range. That constant was picked for the countermove term and reused here without measurement. It is an SPSA candidate rather than a Texel one. |

## Evaluation defaults introduced with no evidence behind them

- **`open_file_bonus` (1 -> 8) and `semiopen_file_bonus` (new, 4).** A rook's
  half-open file was not scored at all, and the fully open case was worth one
  centipawn. Both numbers are now guesses in the right ballpark rather than one
  guess in the wrong one. They are strongly collinear with the rook mobility
  table -- an open file is mostly why a rook has moves -- so expect a fit to
  move all three together, and do not read either in isolation.
- **`king_storm_rank_penalty` {6, 4, 2, 1, 0, 0}.** A new six-entry table
  replacing an ungraded count. The shape (steeply decaying with distance) is
  the confident part; the magnitudes are not. Three scales were measured, and
  the sequence is the clearest illustration in this repository of why a new
  weight has to be scaled against the *total* a term contributes rather than
  against its own entries: the table is summed over up to three pawns, and the
  count table it augments tops out at 4 centipawns for a full storm.

  | Peak entry | Full storm | Result |
  | --- | --- | --- |
  | 32 | 96 | -22 Elo over 141 games (in a two-change batch) |
  | 12 | 36 | -37 Elo over 228 games |
  | 6 | 18 | +4.9 +/- 14 over 484 games -- merged |

  At 6 the term is still four times the old value, but it reads as an
  adjustment to the count table rather than a replacement of it, and that is
  the difference between -37 and neutral. It is merged on the neutral-plus-
  structural rule: an ungraded count genuinely cannot tell a pawn on the third
  rank from one on the sixth, so the axis is worth having in the parameter set
  even though its present weights bought nothing. It overlaps
  `king_storm_penalty`, which still charges for the number of storming pawns,
  and both overlap the shelter and king-zone open-file terms -- move all three
  together under the tuner.
- **`cont_hist1_pct` and `cont_hist2_pct`, both 100.** The two continuation
  history planes are added to the plain history score at full weight because
  that is what other engines do, not because it was measured here. Zero
  switches a plane off, so SPSA can say whether the second plane earns its
  dimensions.

## Known coverage gaps

Buckets that live code reads but the test corpus does not reach, so the tuner
sees no gradient for them and they keep their defaults. Listed in
`known_unreached` in `tests/test_eval.cpp`.

- `queen_mobility_23` .. `queen_mobility_27`, plus 5, 10, 11, 15, 17. Positions
  added specifically to reach the wide end moved none of them: the kings and
  the remaining pawns keep the real safe-square count below the top buckets.
  If tuning data covers them, they are worth fitting; otherwise consider
  shortening the table so the unreachable tail is not carried around.
- The equivalent knight, bishop and rook mobility gaps, which predate this.

## Structural issues deliberately not fixed yet

- **Endgame heuristics are still hardcoded.** `eval_passed_kpk`,
  `eval_passed_krrk`, `eval_passed_knbk`, `eval_kpk`, `eval_krrk` and
  `eval_knbk` between them hold thirteen `constexpr` constants -- opposition
  bonuses, pawn spread bonuses, blockade penalties, advanced passer bonuses --
  that the tuner cannot see. They were left alone deliberately: they fire only
  in narrow material configurations that a Texel corpus will barely contain, so
  registering them would add thirteen parameters with almost no gradient and a
  matching thirteen entries to the coverage corpus. Revisit if a fit ever has
  enough endgame data to support them, or if these endgames measure badly.
- **Terms left unregistered on purpose.** `sq_score_scaling`, `attack_scaling`
  and `mobility_scaling` are all-ones multipliers in front of terms that are
  themselves tuned, so fitting both sides of the product is rank deficient.
  `pinned_scaling` is a divisor the tuner could drive to zero. The reasoning is
  recorded in `src/parameters.cpp`; revisit only if a fit wants that freedom.
- **The pawnless scaling rule has a cliff in it, and the obvious repairs are
  both wrong.** `no_pawn_scale` is applied when `is_pawnless_endgame(p) &&
  std::abs(score) < 400`, so the *finished* score decides whether the finished
  score gets quartered. A position scoring 399 is damped to 99 and one scoring
  401 is left alone: two centipawns of raw evaluation are worth three hundred,
  and a search that can nudge the raw score over the line collects the jump for
  free. To see it, score a few hundred random pawnless positions at depth 1 and
  histogram them -- across 583 such positions not one could land between 100 and
  400. The band is not sparse, it is unreachable, because everything below the
  threshold is multiplied by 32/128 and everything above it is not. Disabling
  `minor_advantage_no_pawn_scale` does not close the gap, which rules out the
  other scaling rule as the cause.

  Two repairs were written and both were discarded after measurement, which is
  why this is a note and not a patch:

  - *Ramp the damping out linearly as the score approaches 400.* Removes the
    discontinuity and is monotone, but it only moved 8 of the 583 positions,
    because `minor_advantage_no_pawn_scale` is the binding constraint through
    `std::min` almost everywhere the ramp would matter.
  - *Key the rule on material instead of on the score,* which is the
    principled version -- whether an ending is drawish is a property of the
    material, and material only changes on a capture, so there is no boundary
    left to cross by shuffling. It scored rook-versus-knight and
    two-bishops-versus-knight at +370 and +394 instead of +92 and +98. Those
    endings are drawish and the old rule was right about them by accident, so
    the principled version is worse where it differs.

  What that leaves is that `ei.me->score` does not mean what the neighbouring
  rule assumes it means: rook versus knight is a 180cp material edge, yet it
  does not satisfy `std::abs(ei.me->score) <= 315`, so the "single minor piece
  advantage" rule never fires on it. Establish what that field actually holds
  before touching either rule; the threshold and the damping factor both need
  refitting afterwards, so this belongs with tuning rather than ahead of it.

## The history table lives on the wrong scale for its own thresholds

Measured on a depth-15 bench, at every node where history pruning is
considered, over 146769 samples:

| `history_malus_pct` | table min | table max | below `lmr_hist_bad` (2000) | below `history_prune_margin` (4096) |
|---|---|---|---|---|
| **0 (the shipped default)** | **-194** | 9181 | **0.00%** | **0.00%** |
| 50  | -4760 | 2946 | 5.08%  | 0.08%  |
| 100 | -9042 | 2437 | 27.85% | 14.88% |
| 200 | -13608 | 543 | 60.50% | 51.18% |

With the default, the negative half of the table never leaves the
neighbourhood of zero, so **history pruning and the LMR bad-history extra
reduction never fire at all** -- not rarely, never. Both are live code
reading a threshold two orders of magnitude outside the range the table
reaches. `history_malus_pct` exists precisely to develop that half and
ships switched off.

Turning it on is not free, and the table is the reason: the malus is
applied to every quiet at every fail-low node, which vastly outnumbers
the bonuses handed out at cutoffs, so the whole distribution slides
negative rather than widening. At 100 the maximum falls to 2437, which
kills `lmr_hist_good` (4000) exactly as it revives `lmr_hist_bad`. The
three thresholds cannot all be reachable at once while bonus and malus
are this far out of balance.

What this needs is a joint fit over `history_bonus_scale`,
`history_malus_pct`, `lmr_hist_bad`, `lmr_hist_good` and
`history_prune_margin`, not five separate guesses -- they are one
mechanism and SPSA should see them together. Until then, note that three
SPSA dimensions are being spent on thresholds that cannot move the
search, which is worse than leaving them out.

## Test curation that depends on evaluation values

`SearchTest.ExactSearchIsMirrorSymmetric` asserts a curated list of positions.
The search it uses is not exact in the strict sense -- the transposition table,
the aspiration window and the quiescence SEE filter all survive the knobs it
turns off -- so roughly one position in seventy disagrees with its mirror by a
few centipawns, and *which* positions do depends on the evaluation's actual
numbers. Adding queen mobility moved one position into that set and it was
replaced; the tactical motif line-clear fix then moved a different one, which
was removed with its reasoning written into the list. That is twice in one
session, which is the argument for fixing the test properly.

This is worth removing rather than curating around. Making `baseDelta` and
`smallDelta` in `search.cpp` into parameters, so a test can open the aspiration
window fully, and giving the test a way to run without the table, would turn
the assertion from a curated observation into something that follows from a
symmetric evaluation. Until then, expect to re-curate this list whenever the
evaluation changes, and check eval-level symmetry first: if the position's
evaluation still equals minus its mirror's, the search residual is the cause.

## Half the evaluation is written in units it cannot speak in

Counting every non-PST tunable in `parameters.hpp` against a pawn worth 100:

| range | count | share |
| --- | --- | --- |
| exactly 0 | 18 | 6% |
| 1 to 4 | 154 | 52% |
| 5 to 15 | 79 | 27% |
| 16 to 50 | 30 | 10% |
| above 50 | 34 | 11% |

Over half the evaluation's weights are worth four centipawns or less. That is
not a tuning observation, it is a structural one: a term that spans two
centipawns end to end cannot express a judgement at all, whatever the position.
`king_shelter` is `{-1, -1, 1, 1}` -- a king with no pawns in front of it and a
king behind three are two centipawns apart. `threat_weak_pawn` is `{1, 1, 1, 1}`,
so a queen attacking a hanging pawn says the same thing as a knight. Terms like
these are outvoted by a single piece-square entry.

This is the most likely reason a large body of chess knowledge in this
evaluation never showed up in the rating, and the most likely place for Texel
tuning to find a lot at once: the shapes are mostly right and the magnitudes are
mostly placeholders. It also says something about what to do *before* tuning.
Every dead or near-dead term found this session -- outposts firing for 0.68% of
minors, a rook's open file worth 1 and its half-open file worth nothing, two
storming pawns worth nothing, a passer on rank 4 worth 2 with the rest of its
evaluation skipped entirely -- would have been fitted as noise by a tuner rather
than fixed by one. Structure first, magnitudes second.

The corollary is the trap this session fell into twice. Correcting a structural
defect invites setting the new weight to what the term "should" be worth, which
in a compressed evaluation means setting it far above its neighbours. A king
storm table peaking at 32 beside a `king_open_file_penalty` of 9 measured -22
Elo; at 12, which is 36 centipawns summed over three pawns against a previous
maximum of 4, it measured -37 over 228 games. Fix the shape, keep the magnitude
in family with whatever sits beside it, and let the tuner raise the whole block
together.

## Passed pawn ladder and blocked penalty

`eval_passed_pawns` used to stop at `row_dist > 3`, so a passer four or more
ranks from promotion collected 2 centipawns and skipped the rest of the
function entirely -- no rook-behind credit, no connected-passer bonus, no
control of the square in front, and no penalty for losing it. Both tables are
now six entries covering every rank a passer can stand on.

| | old | new |
| --- | --- | --- |
| `passed_pawn_rank_bonus` | `{2, 45, 90, 180}` | `{5, 11, 22, 45, 90, 180}` |
| `passed_pawn_blocked_penalty` | `{30, 55, 120}` | `{3, 7, 15, 30, 55, 120}` |

The three new entries in each continue the existing ladder's roughly doubling
shape downward. That is a guess about shape, not a fitted answer, and the
shape is the part worth re-examining: it assumes the value of a passer grows
geometrically with advancement, which is the usual assumption but not a
measured one.

The first version of this change widened only the bonus table and left the
penalty at three entries with a clamped index, which charged a passer six ranks
out the same 30 centipawns that a passer three ranks out pays -- against a
bonus of 5. That measured -30 Elo over 127 games. Widening both tables together
brought it back to neutral. Worth remembering as the same lesson in a different
costume: extending a term's *scope* is as much a weight change as editing its
value, because every constant that term touches now applies to positions it
never applied to before.

## Outpost defended bonus

`outpost_defended_bonus` is `{0, 4, 3, 0, 0, 0}` -- a knight or bishop on a hole
that one of its own pawns defends. The case is rare (0.34% of minors over a
depth-15 bench) and the two values were chosen to sit beside
`knight_outpost_bonus`, which tops out at 3, rather than from any evidence that
a defended outpost is worth about as much again as the outpost itself.

- **`king_danger_endgame_scale`, 40.** None of the king danger terms -- safe
  checks, the quadratic attacker score, the attack combinations -- were faded
  by game phase, so a rook endgame was charged for an exposed king at exactly
  the rate a middlegame with two queens on was. That is wrong on the chess and
  it fights the endgame king table, which wants the king walking into the
  centre. The shelter term beside them was already tapered, which is what makes
  this an oversight rather than a deliberate choice.

  The taper is the confident part; 40 is not. It says a little over a third of
  the danger survives into a pure endgame, which is a guess at "some danger is
  real even with few pieces, because the pieces that remain still give check".
  100 reproduces the old behaviour exactly, so the tuner can undo this
  completely if the measurement says so.

- **The king zone, and every weight that reads through it.** The attacker
  counting sites now use a band two ranks deep in front of the king instead of
  the eight squares touching it. The shape is not in doubt -- a knight on g5
  aimed at a castled king on g1 was previously worth exactly nothing to the
  danger sum -- but the weights underneath it were all chosen against the old,
  narrower mask.

  Two of them are affected in a way tuning has to sort out. `attacker_weight`
  feeds a term that is squared, so a wider zone that raises the attacker count
  does not raise the penalty proportionally, it raises it quadratically;
  `king_danger_divisor` is the obvious counterweight and was never moved.
  `attack_combos` fires on squares attacked twice, and there are simply more of
  those in a zone half again as large.

  Measured `+3.5 +/- 19.1` over 794 games, i.e. neutral, which is consistent
  with a better-shaped term carrying weights calibrated for the old shape.
  These four -- `attacker_weight`, `king_danger_divisor`, `attack_combos` and
  the `pawn_king` / piece-king count tables -- should be refit together rather
  than one at a time.

- **`fp_base` 100 / `fp_margin` 90 / `fp_max_depth` 6.** Forward futility
  pruning of quiet moves. The rule is standard and was simply missing; these
  three numbers are conventional starting points and nothing more.

  At these values it barely fires -- roughly 1.4% of bench nodes -- and it
  measured `+2.2 +/- 19.2` over 794 games, which is what a nearly inert change
  should measure. Dropping the base to 0 and the margin to 60 cuts about 12% of
  nodes, so the whole useful range of this rule sits below the default it
  shipped with.

  Note also that the condition reads raw `depth`, where the convention is to
  read the LMR-reduced depth. That makes 90 per ply mean something different
  here than it does in the engines the number was borrowed from, and is worth
  changing before the margins are fit.

- **Rejected: futility margins at `fp_base` 0 / `fp_margin` 60.** Recorded
  because the negative result is the useful part. The shipped 100/90 fires on
  about 1.4% of bench nodes; 0/60 removes about 12% of the tree, and measured
  `-15.6 +/- 19.1` over 794 games.

  So the answer is not "prune more of the same thing". The likely cause is that
  the condition reads raw `depth` where every engine this rule was borrowed
  from reads the LMR-reduced depth: at raw depth 6 the margin is being applied
  to a subtree that may really be searched at depth 2, and the rule throws away
  moves the reduced search would have looked at cheaply anyway. Switch to a
  reduced depth before touching the margins again.


## The quiet history table cannot support the rules that read it

Three measurements, all negative, all against the same baseline at 10+0.1.
Recorded together because the conclusion only follows from the set.

The starting observation, already in this document: `lmr_hist_bad` (2000) and
`history_prune_margin` (4096) sit two orders of magnitude outside the range the
table reaches, so neither rule can fire. Measured over a depth-10 bench the
distribution is `n=953, min -187, p5 -90, p10 -62, p50 -4, p90 75, max 8164`.

**1. Retarget all three thresholds onto the observed percentiles**, 60 / 75 /
60, i.e. roughly the worst and best tenth. `-21.6 +/- 21.6` over 592 games,
LOS 2.5%. Bench rose 2% because `lmr_hist_good` at 75 un-reduces about a tenth
of all quiets.

**2. Retarget only the two dead ones**, 60 / 4000 / 60, leaving
`lmr_hist_good` alone. Bench unchanged at 754284, so this isolates the two
rules cleanly. `-29.7 +/- 28.3` over 365 games, LOS 2.0%.

That is the informative one. A history score of -60 in a table whose minimum
over an entire bench is -187 is not evidence a move is bad; it is one or two
fail-lows of noise. Pruning on it discards good moves, and the rules were
harmless only because they were unreachable.

**3. Develop the negative half instead**, `history_malus_pct` 0 -> 100. This
works as designed: the distribution becomes `n=2667, min -8183, p10 -1389,
p50 -56, max 709` and bench falls from 754857 to 728262, so ordering genuinely
improves. It also measured `-67.4 +/- 29.3` over 373 games, LOS 0.0% -- the
worst result of the session.

The reason is visible in the distribution. The malus is charged against every
quiet tried at every fail-low node, and fail-low nodes vastly outnumber
cutoffs, so the whole table slides negative rather than widening: the maximum
collapses from 8164 to 709. The positive signal that quiet ordering actually
depends on is destroyed, and a move is punished for having been tried in a
line that failed low for reasons having nothing to do with that move.

**Later corroboration.** The history-pruning site was instrumented directly
over a full bench: `seen=61534 fire=0 min=-193`. It is reached sixty-one
thousand times and prunes nothing, and the most negative score ever seen there
is -193 against a depth-1 threshold of -4096. This measures the rule rather
than the table and reaches the same conclusion.

That re-derivation cost a session's work because nothing at the parameter
definitions pointed here. The constants in `parameters.hpp` now carry the
measurements and a pointer to this section. If you are reading this because you
just discovered these thresholds are unreachable: that is the documented
finding, not a new one, and both repair directions are already measured.

One real defect did come out of that pass. `lmr_hist_good` was applied as
`R = std::max(0u, R - 1)` with `R` unsigned, so when `reduction()` returned 0 --
which it does for PV nodes at low depth and move count -- `R - 1` wrapped to
`UINT_MAX` before the max ran, and the move was extended a ply instead of
reduced one ply less. Fixed in #123, `+11.4 +/- 14.6` over 1124 games.

**Conclusion.** The cutoff path already does the conventional thing -- bonus to
the move that cut, malus to the quiets tried before it. The fail-low malus is
not conventional and should stay off. The two thresholds reading the negative
half stay where they are, dead, and should be treated as dead code rather than
as parameters awaiting a fit: SPSA cannot rescue them, because both directions
have now been measured. If quiet history is to drive pruning here it needs a
better-conditioned table first -- the current one is keyed on (colour, from,
to) with no piece identity -- not a different threshold.

## Conditioning the quiet history table on the moving piece: measured, negative

The section above ends by saying the prerequisite for any rule reading the
negative half of quiet history is a better-conditioned table, "the current one
is keyed on (colour, from, to) with no piece identity". That prescription has
now been tested directly, and it does not hold. Every way of putting the piece
into the key made move ordering worse.

All figures are bench nodes at the same depth, lower is better, against a
baseline of 694503.

| keying | bench | vs baseline |
| --- | --- | --- |
| `[colour][from][to]` (current) | 694503 | -- |
| `[colour][piece][to]` | 771931 | +11.1% |
| `[colour][piece][from][to]` | 963945 | +38.8% |
| `[colour][from][to]` + summed `[colour][piece][to]` plane | 814764 | +17.3% |
| ...same, piece plane at quarter weight | 909552 | +31.0% |

The runs went through a single `history_slot()` accessor, and a control that
routed the *original* key through that same new plumbing reproduced 694503
byte for byte. So the plumbing is not what moved these numbers.

**Why adding the piece hurts.** Within any one position the from-square already
determines the piece, so the piece is only new information across positions --
where sliding lines are shared and d1-d4 might be a queen in one game and a
rook in the next. That is a real but small gain in resolution, and it is bought
by splitting every counter six ways. The table then needs roughly six times the
samples to reach the same confidence, and quiet history is already sparse: a
cutoff node has tried a mean of 0.34 quiets before it cuts. Dilution dominates
resolution. Dropping the from-square instead (`[piece][to]`) avoids the
dilution but throws away the finer move identity, and also loses: it is close
to an unconditioned marginal of what continuation history already stores.

**A caution on these numbers.** Bench node count is chaotic with respect to
ordering and pruning changes -- small parameter moves swing it 20%, and the
quarter-weight row above is *worse* than the full-weight row, which a smooth
signal would not do. No single row here should be read as a precise effect
size. What is solid is the pattern: four independent variants, none of them
within reach of baseline, against a control that reproduced it exactly. None
of them earned an SPRT.

**Conclusion.** (colour, from, to) is not an oversight, it is a reasonable
optimum for a table this sparse, and "condition it better" is not the
unexplored lever the previous section assumed. If the negative half of quiet
history is ever to drive pruning, the constraint to attack is the *sparsity* --
how few samples each slot gets -- not the key. Nothing here changes the
standing advice that the thresholds themselves stay dead.

## Why Texel tuning never produced anything

Three independent defects, stacked. Any one alone would have been enough to
make the pipeline useless, and none of them announced itself as a failure --
every run printed a decreasing error and reported success.

**1. The objective was inverted for half the data.** Fixed earlier, in
`f5bcd20`, by comparing evaluation and result in the same point of view. The
cost is measurable after the fact: on 287k quiet positions the error under the
old convention is 0.2357, against a predict-0.5 baseline of 0.25. The fit was
pinned to "learned nothing" and could not have been anything else.

**2. The update rule could not move.** The step was

    delta = (int)(-lr * vel[i] * 1e6);   // clamped to +/-8

so any gradient above roughly 2.7e-6 saturated the clamp. Real gradients are
around 1e-4, which means every one of the ~300 stage-2 parameters moved the
full eight centipawns on every iteration at once -- a step of norm
`8*sqrt(300)` = 138cp. That always overshot; the failure branch then reverted
*all* parameters and halved the learning rate. Roughly eight halvings were
needed before the step became sane, and `scripts/tune.sh` runs three
iterations. The tuner reverted every iteration and printed "Converged!"
without having moved a weight.

**3. The parameterisation was not identifiable.** Stage 1 is documented as
"category-level scale factors only" but also contained the sixteen individual
pawn-structure weights. `pawn_table.cpp` sums those into `pe->score_mg/eg` and
`hce.cpp` multiplies the tapered sum by `pawn_structure_category_scale`, so the
contribution is `taper(sum_i w_i) * scale / 100`. Multiplying every weight by
*c* and the scale by *1/c* leaves the evaluation bit-identical: an exact flat
direction in the loss. The optimiser drifts along it, driven by nothing but
the noise in whichever dataset it was given. Two fits landed at
`doubled_pawn_penalty_mg` = 39 and -15 -- a penalty that is a bonus on one
dataset.

A fourth issue was not a defect so much as an unexamined assumption: the scale
bounds `[10, 200]` were binding. Four of eleven stage-1 parameters came to rest
against a cap, so the numbers being reported described the box rather than the
data. Widened to `[0, 800]`, `threat_category_scale` fits to 283.

### What the repaired pipeline says

Two datasets -- 287k self-play positions and 985k CCRL positions -- now agree
on the parameters that both can see, and disagree only where you would expect:

| parameter | self-play | CCRL |
| --- | --- | --- |
| `sq_score_category_scale` | 73 | 73 |
| `pawn_structure_category_scale` | 6 | 11 |
| `king_danger_divisor` | 254 | 269 |
| `space_category_scale` | 104 | 122 |
| `mobility_endgame_scale` | 118 | 235 |
| `king_safety_category_scale` | 198 | 146 |
| `passed_pawn_category_scale` | 0 | 71 |
| `passed_pawn_endgame_scale` | 247 | 104 |
| `king_danger_endgame_scale` | 33 | 94 |

Two things worth keeping. Both datasets independently want the piece-square
tables scaled *down* to 73%, which is the counterpart to the finding that the
positional terms are undersized: the eval leans too heavily on its PSTs. And
every remaining disagreement is a phase-dependent parameter. That is a
property of the data, not of the fit -- the self-play set is generated from
six uniformly random opening plies at depth 4 with early adjudication, so it
barely reaches the endgames those parameters describe. CCRL is the better fit
set until `datagen` uses a book, searches deeper, and stops adjudicating early.

**K differs sharply between the two: 825 on self-play, 477 on CCRL.** The CCRL
figure is close to the conventional ~400. A K that large on self-play data is
consistent with evaluations that are noisy rather than with an eval whose
magnitudes are wrong, and it means the earlier worry -- that K = 825 implied a
magnitude-inflated evaluation -- was reading a property of the dataset.

### Cost, for planning

Each iteration costs `2 * NP + a few` full passes over the data, so time is
linear in both sample count and parameter count. Measured at roughly 0.31
microseconds per position per pass on six threads:

| data | stage 1 (11p) | stage 2 (273p) | stage 4 PST (768p) |
| --- | --- | --- | --- |
| 287k | 2 s/it | 55 s/it | 2.3 min/it |
| 1M | 7 s/it | 3 min/it | 8 min/it |
| 3M | 21 s/it | 9 min/it | 24 min/it |

Memory is the binding constraint, not CPU: one `position` per sample. After
shrinking `position` from 5360 to 2160 bytes that is ~2.2 GB per million
samples, so a 16 GB machine tops out near 3M. The full 11.8M CCRL set needs a
box with 64 GB.

**Texel cannot tune search.** `TuneStage::search` exists, but the objective is
the static evaluation of quiet positions, so every search constant has an
identically zero gradient. The optimiser now detects this and stops rather
than dividing by zero. Search constants need SPSA against game results, which
is why the roadmap puts that after the evaluation fit -- and it must come
after, because every centipawn-denominated margin was fitted against the
current evaluation spread.

## Where the evaluation actually spends its knowledge

`havoc_eval_sensitivity` measures, per parameter, over a corpus of real
positions:

- **worth** -- mean `|eval(param := 0) - eval(param)|`, the centipawns that
  disappear if the term is deleted. What the knowledge is worth to the engine
  as shipped.
- **grad** -- mean `|eval(param + 1) - eval(param)|`, what one unit buys. What
  a tuner can see. Coordinate descent needs a strict improvement, so a term
  with a tiny grad cannot be fitted at all.
- **reach** -- fraction of positions where the parameter changes anything.

Run on 2000 positions of `/tmp/mid.epd` and again on `/tmp/val200.epd`. The two
corpora are drawn from different phases and produce nearly the same table, so
what follows is a property of the evaluation and not of the sample.

### The headline

| group | worth (cp) |
| --- | --- |
| material | 357 |
| passed pawns | ~43 + 23 + 11 |
| piece-square tables | ~87 across six pieces |
| mobility (four tables) | ~50 |
| `restriction_weight` alone | 8.1 |
| `center_influence` | 6.7 |
| **all of king safety, net** | **8.8** |
| **every pawn-structure term, largest single one** | **1.9** |

King safety, as a category, is worth 8.8 centipawns. `restriction_weight` --
one integer counting "I attack more squares than you do" -- is worth 8.1 on its
own, and it is counting squares that mobility has already paid for.

### The specifics that matter

**`king_shelter` is worth 0.20 cp,** at 6.9% reach. The pawn cover in front of
a castled king is the central middlegame king-safety idea and it is worth one
fifth of a centipawn. The storm penalties beside it are 1.17 and 0.80.

**`bishop_king_distance_penalty` is worth 1.94 at 72% reach, and
`knight_king_distance_penalty` 1.63 at 62%.** Each is larger than any single
shelter, storm or open-file term around the king. Both are also malus-only over
a pool that is exactly a piece count, and both fire in the large majority of
positions, which makes them very close to a constant offset per minor -- a
hidden reduction of the knight and bishop material values wearing a positional
costume. The protection bonuses are the same shape with the opposite sign:
`rook_protection_bonus` 0.97 at 56% reach, bishop 0.89 at 58%, knight 0.87 at
55%.

**Nothing in pawn structure exceeds 1.92 cp.** Isolated is about 1.6, doubled
about 0.7, backward 0.52 falling to 0.21 in the endgame. Conventional values
are 10-20. The comment in `parameters.hpp` already worried that the evaluation
was "biased against having pawns at all"; the measurement says the opposite
problem is now the live one -- the terms are too small to express anything.

**Several scales cannot be tuned at all.** `mobility_category_scale`,
`mobility_endgame_scale`, `threat_category_scale`, `space_category_scale` and
all four per-piece mobility scales have grad exactly 0.000 while carrying real
worth: `bishop_mobility_scale` is worth 14.4 cp and a one-unit change to it
moves nothing, because it is a percentage applied by integer division and 1% of
a small term truncates to zero.

**Several knobs are binary.** `restriction_weight`, both king-distance
penalties, all three protection bonuses and `center_influence` have grad equal
to their worth, which means their default is 1. The only settings available are
off and on.

**Four parameters are integer divisors** (`king_danger_divisor`,
`pinned_scaling` for bishop, rook and queen). The smallest change a tuner can
make to `pinned_scaling` is to halve the term.

### What this explains

The single-stage fit cut held-out Texel error by 7.6% relative and then
measured **+1.4 +/- 19.9 Elo over 734 games**, LLR -1.7%. That is not a
contradiction. Texel error is dominated by material and the fit duly repriced
material and the piece-square tables, which is where the worth is. The
positional terms it also moved were, and remain, worth one to two centipawns
each -- below the resolution at which they can change a move choice.

The knowledge budget is spent on counting squares. Mobility, restriction,
centre influence and space together are worth roughly 67 cp; king safety and
pawn structure together are worth under 30. No reweighting fixes that, because
the terms that should carry the difference have gradients around 0.3 cp and
several of the scales that would amplify them cannot be moved by one unit.

## What actually decides moves, and why magnitude work keeps measuring zero

Three instruments were built to answer one question: why does every sweeping
change to this evaluation measure neutral? They agree on an answer.

### Texel cannot help, and the reason is structural

A parameter's gradient does not depend on its current value. Zeroing
`bishop_king_distance_penalty` took its worth from 1.94cp to 0.00 and left its
gradient at 1.938. The tuner therefore sees an identical landscape before and
after any reset of the defaults, its minimum has not moved, and refitting from
better values walks straight back to the fit that measured +1.4 +/- 19.9 Elo
over 781 games.

Texel error is dominated by material, so a fit reprices material and PSTs and
leaves everything else at one or two centipawns -- below the resolution at
which a term can change a move. Its quiet filter also discards the 22% of
positions where tactical terms fire, so those terms are invisible to it by
construction. Use it for material and piece-square tables. Nothing else.

### Most of the evaluation cannot change a move at all

`eval_bench --decisions` generates the legal moves in a position, scores the
children, records which move the evaluation prefers, then deletes a parameter
and counts how often that preference changes. On 300 middlegame positions with
mean branching 29:

| family | flip% |
|---|---|
| `material_value` | 36.7 (~9% per live parameter) |
| `bishop_mobility` | 20.0 |
| `sq_score_category_scale` | 16.3 |
| `passed_pawn_rank_bonus` | 15.0 |
| `king_safety_category_scale` | 8.7 |

606 of 870 live parameters -- 69% -- changed no decision in those 300
positions. **That number is mostly an artifact of the sample size, and taking
it at face value would be a serious mistake.** Repeating the measurement on
larger corpora:

| positions | parameters changing no decision |
|---|---|
| 300 | 606 / 870 (69%) |
| 2000 | 371 / 870 (42%) |
| 10000 | 148 / 870 (17%) |

The count is still falling at 10000 and has not converged. A term that fires in
one position in 500 needs thousands of positions before it flips one decision;
sampling thinly does not find a dead parameter, it manufactures one, and it
does so preferentially for exactly the rare-but-decisive knowledge that
hand-crafted terms exist to encode.

So the evaluation is not mostly dead. It is mostly *rarely decisive*, which is
a different claim with a different remedy. The honest summary is that flip
rates are extremely skewed: `material_value` flips roughly 9% of decisions per
live parameter, while the long tail flips well under 1% each.

The operational consequence is that flip% is a filter on **effort**, not a
licence to delete. Use it to avoid tuning or hand-setting a term that provably
cannot move a decision at the corpus size you measured -- and measure at 10000
positions or more before calling anything dead.

The category scales are single parameters, so zeroing one deletes a whole
category. Removing every square score changes the move in 16% of positions;
removing all of king safety changes it in 9%. Entire categories of chess
knowledge are decision-relevant in fewer than one position in six, because the
argmax over sibling positions is very nearly always settled by material.

Two caveats. This is static evaluation one ply from the root, and search both
amplifies and washes out static differences. And flip% is only ever a statement
about the corpus it was measured on -- see the sample-size table above.

### Search has not saturated, so nodes are expensive

Searching 59 middlegame positions to depth 12 and recording the preferred move
at every depth:

| depth | move changed from previous ply |
|---|---|
| 8 | 29% |
| 10 | 22% |
| 11 | 22% |
| 12 | 12% |

Median settle depth 9; 39% of positions are still changing their move at depth
10 or beyond. Every extra ply still overturns the evaluation in a fifth of
positions.

### The consequence for where to spend effort

An evaluation term is taxed twice: once in generalisation, once in the depth
its cost surrenders. The coherent reset cost +26% nodes at fixed depth, roughly
0.33 ply. Measured both ways it is neutral:

| test | games | result |
|---|---|---|
| fixed time 10+0.1 | 494 | +14.5 +/- 26.9 |
| fixed depth 8 | 576 | -4.5 +/- 25.8 |

Fixed depth removes the node handicap and is the clean measure of evaluation
quality. It says the reset did not improve decisions -- consistent with the
flip% result, since almost every one of its 59 values lives in the
decision-dead 69%.

So the answer to "richer evaluation or better search" is asymmetric rather than
a balance. Terms that cost time must clear a very high bar. Terms that cost
nothing are free to keep. And deleting the decision-dead majority is a pure
win: it removes overfitting surface and returns nodes at the same time.

Run `havoc_eval_bench` before proposing any evaluation change. If the term
cannot flip a decision, it cannot buy Elo, and no amount of tuning will change
that.

### King safety still does not decide king-safety positions

The original discrimination corpus was entirely endgame-phase, so the
middlegame half of the evaluation -- including all of king safety -- had no
coverage at all. Four middlegame pairs now carry full material so that half
actually runs. They are ordered correctly, but attribution says the king safety
family is still not what decides them:

| pair | margin | actually decided by |
|---|---|---|
| intact pawn shield vs h2-h4 | 4cp | `pawn_structure_category_scale`, `pst_mg_pawn`, with `space_category_scale` at -200% |
| castled vs uncastled king | 36cp | `sq_score_category_scale`, `pst_mg_king` |
| king behind pawns vs half-open file | 44cp | `sq_score_category_scale`, `pst_mg_king`; `king_shelter` only 20% |

The coherent reset raised `king_shelter` from {-1,-1,1,1} to {-22,-10,0,6} and
lifted the family's total worth from 8.8cp to 19.5cp, and it is *still* not the
term that decides whether a king is safe. The king piece-square table is. That
is worth understanding before any further king safety work: a term that cannot
win an argument against a PST entry will not repay tuning.

The 4cp margin on the pawn shield pair is the sharpest version of this. Moving
the h-pawn two squares in front of a castled king is worth four centipawns, and
most of that is pawn structure rather than king danger.

Note also what the corpus rules are for. The first draft of the half-open file
pair deleted the f2 pawn to open the file, leaving the sides a pawn apart and
producing a 138cp margin that was entirely material. Equal material is checked
mechanically by the regression test for exactly this reason.

### King shelter: shape fixed, magnitude unresolved

`king_shelter` indexed a four-entry table with a *count* of friendly pawns on
the eight squares touching the king. It could express four states, the whole
gap between a perfect shield and a broken one was one step of that table, and
h3 and h4 were identical because neither touches the king. It is now indexed by
the distance of the nearest friendly pawn in front of the king, per file,
summed over three files.

| | count model | distance model |
|---|---|---|
| pawn shield pair margin | 4cp | 9cp |
| carried by | pawn structure | `king_shelter`, 11cp / 122% |
| pairs decisive | 15/16 | 16/16 |
| bench nodes | 953544 | 697583 |
| SPRT vs previous | - | -11.9 +/- 31.8 over 334 games |

The node result is the interesting one and was not predicted: a shelter that
moves by 11cp between adjacent king positions gives the search something stable
to prune against, where four discrete 6cp steps did not. It returns 27%, more
than the coherent reset spent.

The magnitude is unresolved and should not be hand-tuned further. Summing over
three files triples the range as a side effect of changing the shape: +21/-45
against the count model's +6/-22. Halving to {-8,4,1,-2} was tried and puts the
pawn shield pair back to a 4cp margin that `king_shelter` no longer carries,
with bench back up to 790209. Discrimination and nodes both track the range, so
the two effects cannot be separated by halving, and the wider range is the one
the SPRT actually measured. This is an SPSA problem.

### A correction to earlier attribution figures

`eval_bench` originally ranked individual parameters. A table like
`king_shelter` is one idea spread over several entries, and a pair can move two
of them in opposite directions -- +7 on the near slot, -4 on the far one -- so
no element beats an unrelated scalar even when the family plainly decided the
pair. It now aggregates by family before ranking, which took corpus-wide
correct attribution from 7/14 to 9/14. Attribution claims made before this
change understate table-valued families, including the claim above that king
safety did not carry its own pairs.
