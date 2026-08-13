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

- **`open_file_bonus` (1 -> 12) and `semiopen_file_bonus` (new, 6).** A rook's
  half-open file was not scored at all, and the fully open case was worth one
  centipawn. Both numbers are now guesses in the right ballpark rather than one
  guess in the wrong one. They are strongly collinear with the rook mobility
  table -- an open file is mostly why a rook has moves -- so expect a fit to
  move all three together, and do not read either in isolation.
- **`king_storm_rank_penalty` {32, 20, 8, 2, 0, 0}.** A new six-entry table
  replacing an ungraded count. The shape (steeply decaying with distance) is
  the confident part; the magnitudes are not. It overlaps `king_storm_penalty`,
  which still charges for the number of storming pawns, and both overlap the
  shelter and king-zone open-file terms.
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
