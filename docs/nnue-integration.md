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
