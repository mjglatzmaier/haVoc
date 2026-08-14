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

**Conclusion.** The cutoff path already does the conventional thing -- bonus to
the move that cut, malus to the quiets tried before it. The fail-low malus is
not conventional and should stay off. The two thresholds reading the negative
half stay where they are, dead, and should be treated as dead code rather than
as parameters awaiting a fit: SPSA cannot rescue them, because both directions
have now been measured. If quiet history is to drive pruning here it needs a
better-conditioned table first -- the current one is keyed on (colour, from,
to) with no piece identity -- not a different threshold.
