# Picking this work up on another machine

Everything needed to continue is in this repository. This page is the checklist.

## Verify the handoff landed intact

`bench` is deterministic and machine-independent: it fixes the search to a set
of positions and counts nodes. The node count is therefore an exact checksum on
"same code, same parameters, same build configuration". The *time* and *nps*
will differ between machines; the node count must not.

**Fetch the network first.** Networks are not kept in git, so a fresh clone has
none, and an engine that cannot find one silently evaluates with the
handcrafted evaluation instead. That is a perfectly working engine reporting a
completely different node count, which looks exactly like the build difference
this check exists to catch.

```sh
./scripts/fetch-net.sh                      # ~20 MB, hash-checked against nets/default.txt
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DHAVOC_BUILD_TOOLS=ON
cmake --build build -j
printf 'bench\nquit\n' | ./build/havoc      # expect 776644 nodes, eval NNUE
./build/tests/havoc_tests                   # expect 219 tests, 0 failing
```

`bench` prints which evaluation it used. If it says `eval HCE` the network was
not found: check `scripts/fetch-net.sh` actually placed it in `nets/`. For
reference the handcrafted evaluation benches **1028826 nodes** on the same
positions, so that number is a missing network rather than a broken port.

Six tablebase tests skip when no Syzygy tables are present, so 213 passing and
6 skipped is the expected result on a machine without them. Skips are fine;
failures are not.

A different node count, with the network loaded, means a build difference
rather than a porting success. Track it down before measuring anything.

## What does not travel

- **cutechess-cli** — build or install it, then `export CUTECHESS=...`.
- **Anchor engines** — see [`scripts/testing/README.md`](../scripts/testing/README.md).
  They must be rebuilt; `gauntlet-anchors.sh` reports how many it found and
  refuses to run with fewer than two.
- **Baseline binaries** — the per-experiment snapshots used as SPRT baselines are
  build artifacts. Rebuild any baseline you need from its commit, and confirm it
  by bench node count.

## Elo numbers are local to a machine, a time control and a book

Ratings are not portable. The rows in the top-level README are comparable to
each other because anchors, hardware, book and time control were held fixed; they
are not comparable to a run on different hardware. On a new box, re-establish a
baseline before comparing anything to a number measured elsewhere.

Concurrency is the one setting worth tuning per machine. `common.sh` defaults to
physical cores minus two. Oversubscribing does not bias a result when both
engines are starved equally, but it inflates per-game variance, which costs
games-to-decision.

## Merge policy in force

- **Structural** changes that measure neutral-to-slightly-negative may merge, and
  are recorded in `revisit-after-tuning.md` for revisit after tuning. The
  reasoning is that a correct-but-unoptimised structure is a prerequisite for
  tuning to find anything.
- **Pure parameter** changes must earn their keep with an SPRT. They have no
  structural argument to fall back on.

That policy has a failure mode, and it must be actively guarded. A change that
measures −5 ± 25 is indistinguishable from neutral, so several can merge in a row
while all being real, small regressions. Periodically SPRT current `main` against
a snapshot 5–10 PRs back: the accumulated delta is larger than any individual
one, so it resolves faster and in a single direction.

## Where the effort should go

Ordered by expected return. The reasoning behind each is in
`revisit-after-tuning.md`; this is only the queue.

1. **SPSA the search margins, using games as the objective.** The clearest
   remaining headroom. Search is demonstrably not saturated (29% of positions
   change their preferred move at depth 8, 12% still at depth 12), and bench node
   count is proven unusable as a proxy — `rfp_margin` moves it non-monotonically
   between 539k and 953k over a 50-point sweep. A screening gauntlet found no
   margin candidate grossly wrong, with one weak but directionally consistent
   hint that a larger `qs_delta_margin` helps.
2. **SPSA the king shelter magnitude.** The shape was fixed by indexing on
   distance per file rather than counting pawns near the king, which was worth
   −27% bench nodes. But summing over three files tripled the dynamic range as a
   side effect, and halving the values by hand loses both the discrimination and
   the node win. It needs a search, not a guess.
3. **Build a defensible dead-term list before trimming anything.** Use
   `eval_bench --decisions` on **at least 10000 positions**. The fraction of
   parameters that change no decision falls from 69% at 300 positions to 17% at
   10000 and was still falling. Small samples manufacture dead parameters, and
   they do it preferentially to rare-but-decisive knowledge.
4. **Fix the mis-attributed discrimination pairs** — 5 of 14 are decided by
   `sq_score_category_scale` or `pst_mg_king` rather than the term they were
   written to test.

## What not to spend time on

Texel tuning cannot help this evaluation, and the reason is structural rather
than a matter of setup. A parameter's gradient is independent of its current
value, so resetting the defaults leaves the optimiser's landscape and its minimum
exactly where they were: zeroing `bishop_king_distance_penalty` took its measured
worth from 1.94 to 0.00 while its gradient moved 1.935 to 1.938. The fit it
converges to measured +1.4 ± 19.9 Elo over 781 games. The error is dominated by
material, and the quiet filter discards the positions where tactical terms fire.

Use it for material and piece-square tables. Nothing else.

## The larger question

The coherent parameter reset — 59 values rewritten onto consistent scales, with
double-counting removed — measured neutral, twice (+14.5 ± 26.9 at fixed time,
−4.5 ± 25.8 at fixed depth). Sizeable, well-motivated changes to hand-crafted
evaluation keep landing inside the noise. That is itself evidence about where the
ceiling of this approach sits, and an argument for the NNUE work rather than
against it.
