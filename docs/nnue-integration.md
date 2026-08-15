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

`IEvaluator` gains two optional hooks with empty defaults, so the HCE is
unaffected and the interface stays honest about what it needs:

```
virtual void push(const position& after, const FeatureDelta&) {}
virtual void pop() {}
```

## 4. Order of work

Each step is verifiable on its own, against the HCE, before the next begins.

1. **Record the delta.** Populate `FeatureDelta` in the three primitives.
   Nothing consumes it. Verify: bench node count unchanged, and measure the
   nps cost — if recording alone is not free, the design is wrong.
2. **Assert the delta is complete.** A debug-only check that replaying the
   recorded events against the pre-move board reproduces the post-move board
   exactly, run over a perft or the bench suite. This is the step that catches
   a missed case in castling or en passant, and it is much cheaper to do here
   than to debug as a wrong evaluation later.
3. **Wire the hooks.** `IEvaluator::push`/`pop` called from the search
   alongside `do_move`/`undo_move`. Still no consumer.
4. **A trivial incremental evaluator.** Implement material counting *twice* —
   once from scratch, once incrementally through the accumulator hooks — and
   assert they agree at every node. This exercises the whole mechanism with an
   evaluation whose correct answer is known, and it is the last point at which
   a bug is easy to find.
5. Only then: the real accumulator, the network, and the king-bucket refresh
   policy.

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
