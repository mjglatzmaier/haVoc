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

- **`isolated` implies `backward`.** `backward_pawn()` treats a pawn whose
  neighbouring file is empty as backward, so every isolated pawn is charged
  both penalties, and both are doubled again on a semi-open file. At the
  current 1cp backward penalty the Elo cost is negligible, but the two weights
  are collinear, which is a problem for a fit rather than for play. Separating
  them changes what the two parameters mean, so it should be done *before* the
  tuning run whose output anyone intends to keep.
- **Terms left unregistered on purpose.** `sq_score_scaling`, `attack_scaling`
  and `mobility_scaling` are all-ones multipliers in front of terms that are
  themselves tuned, so fitting both sides of the product is rank deficient.
  `pinned_scaling` is a divisor the tuner could drive to zero. The reasoning is
  recorded in `src/parameters.cpp`; revisit only if a fit wants that freedom.

## Test curation that depends on evaluation values

`SearchTest.ExactSearchIsMirrorSymmetric` asserts a curated list of positions.
The search it uses is not exact in the strict sense -- the transposition table,
the aspiration window and the quiescence SEE filter all survive the knobs it
turns off -- so roughly one position in seventy disagrees with its mirror by a
few centipawns, and *which* positions do depends on the evaluation's actual
numbers. Adding queen mobility moved one position into that set and it was
replaced.

This is worth removing rather than curating around. Making `baseDelta` and
`smallDelta` in `search.cpp` into parameters, so a test can open the aspiration
window fully, and giving the test a way to run without the table, would turn
the assertion from a curated observation into something that follows from a
symmetric evaluation. Until then, expect to re-curate this list whenever the
evaluation changes, and check eval-level symmetry first: if the position's
evaluation still equals minus its mirror's, the search residual is the cause.
