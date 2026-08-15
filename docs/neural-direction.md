# Neural evaluation: direction and the reasoning behind it

Written 15 August 2026. This records an architectural decision and, more
importantly, *why* — so that a future session can either build on it or
overturn it deliberately rather than by drift.

Companion documents: [`roadmap.md`](roadmap.md) for what to work on,
[`revisit-after-tuning.md`](revisit-after-tuning.md) for the evidence behind
the evaluation work, [`handoff.md`](handoff.md) for setup.

The transformer research lives in a separate repository, `~/code/chess`
(branch `feature/trainer`): `training/ARCHITECTURE.md` for the model design,
`training/PIPELINE.md` for the data pipeline, `paper/PLAN.md` for the paper.

---

## 1. The decision

**haVoc stays a CPU engine: alpha-beta search with an incrementally-updatable
NNUE evaluator. GPU is an optional accelerator, never a requirement.**

The transformer is not abandoned. It is repositioned — from *the evaluator
called at every node*, which the arithmetic in §4 rules out, to a **sparse
policy oracle used for move ordering**, which the same arithmetic says is
comfortably affordable.

Handcrafted evaluation work is **paused**, not deleted. The HCE remains the
correctness reference and the datagen evaluator, and must stay correct and
fast for both roles.

---

## 2. Why handcrafted evaluation is paused

Three independent lines of evidence point the same way, and none of them is a
matter of opinion:

- the coherent evaluation reset measured **neutral** in games;
- the argmax of the evaluation is **dominated by material**, so most terms are
  not deciding positions;
- 120 SPSA iterations over 4800 games on `king_shelter` **never moved the
  objective** — 0.497 mean score across the first forty iterations and 0.497
  across the last forty. That is a random walk on a flat surface, not a
  descent.

The reasonable reading is that the HCE is near its ceiling. Further magnitude
tuning is not where the remaining strength is.

---

## 3. Is attention actually a better evaluator than NNUE?

Yes, and the evidence is strong enough to treat as settled:

- **DeepMind, *Grandmaster-Level Chess Without Search* (Ruoss et al. 2024,
  arXiv:2402.04494)** — a 270M-parameter transformer, one forward pass per
  move and *no search at all*, reached **2895 Lichess blitz**. An NNUE
  evaluated once per move is nowhere near that.
- **Lc0 BT4** is roughly **+300 Elo over the best convolutional net (T78)**
  with fewer parameters and less compute.
- The systemic argument: **Lc0 competes with Stockfish while running ~100k nps
  against Stockfish's ~100M nps.** A ~1000x node deficit is about ten
  doublings — on the order of 500-700 Elo of search — and the eval quality
  repays it.

The architectural reason is that **NNUE is deliberately crippled for speed**.
Its first layer is a linear sum over sparse one-hot features, so there is no
feature interaction whatsoever until the small (16/32-neuron) layers. It is in
effect a very large learned lookup table followed by a tiny MLP. HalfKA
approximates interaction by crossing king square with piece features, which is
why the feature set is enormous and the data requirement so large. Attention
models piece-to-piece relations directly.

**Caveat worth keeping in view:** eval accuracy pays off *sublinearly* inside a
deep search, because search repairs eval error. Per position the transformer
wins clearly. Per unit of compute the answer depends entirely on hardware,
which is what §4 is about.

---

## 4. The cost arithmetic

This is the part that decides the architecture. Figures are MAC counts for the
model configurations in `~/code/chess/training/config.py`, 65 tokens
(64 squares + 1), computed as
`per_token = 3d^2 (QKV) + d^2 (out) + 2Td (attention) + 2*d*ff (FFN)`,
then `x T x layers`. Recompute rather than trust these if the config changes.

| config | MMACs/eval | attention matmul | FFN | projections |
|---|---|---|---|---|
| tiny (64, 2L, ff 256) | 7.5 | 14.5% | 57.0% | 28.5% |
| **default (128, 4L, ff 512)** | **55.4** | **7.8%** | **61.5%** | **30.7%** |
| med (256, 6L, ff 1024) | 319.7 | 4.1% | 64.0% | 32.0% |
| big (512, 8L, ff 2048) | 1670.4 | 2.1% | 65.3% | 32.6% |

An NNUE evaluation, *after* its incremental accumulator update, is
**~0.01-0.02 MMACs** — three to four orders of magnitude cheaper.

The machine (i9-13900KS) sustains roughly **0.5 TMAC/s fp32 (AVX2)** and
**~2 TOP/s int8 (VNNI)**. The engine benches **~1.83M nps single-threaded** on
an idle box. (An earlier draft of this document used 685k nps; that figure was
measured while a 30-way match was running and understates the gap. Re-measure
before quoting either.) Holding 1.83M nps with a transformer value evaluation
at every node would require:

| config | required throughput | available |
|---|---|---|
| tiny | 13.7 TMAC/s | ~0.5 fp32 / ~2 int8 |
| default | 101.4 TMAC/s | ~0.5 fp32 / ~2 int8 |
| med | 585.1 TMAC/s | ~0.5 fp32 / ~2 int8 |

Equivalently, the default config yields ~9,000 evals/sec in fp32 and ~36,000
in int8, against 1,830,000 needed — **51x short even with perfect int8**.

**Even the tiny two-layer net misses by 6.9x against int8 and 27x against
fp32, and it is too small to be worth training.** This is not an optimisation
gap. It is the architecture being wrong for the deployment.

### Why "not incrementally updatable" is the root cause

NNUE's first layer is a *sum over active features*, so a move subtracts three
or four features and adds three or four — a few hundred adds. Self-attention
re-mixes every token as soon as one square changes, so every evaluation is a
full forward pass. This is structural and cannot be engineered away.

Precise boundary, which is useful and slightly encouraging: the **input
projection** (`Linear(27 -> d)` per square) *is* incrementally updatable, since
it is per-token with no mixing. Incrementality dies at the first attention
layer. A hybrid-depth net — several per-square MLP layers, then one or two
attention layers — would recover part of it, but the attention layers still
dominate the cost. This helps; it does not rescue CPU alpha-beta.

---

## 5. Two efficiency ideas that do not work, and why

Recording these so they are not re-proposed.

**Linearised attention (Performer, Linformer, Mamba/SSM) is not worth
pursuing here.** These target the O(T^2) attention matmul. At T=64 that term is
**7.8% of the default config's cost**. FFN (61.5%) and the QKV/output
projections (30.7%) are O(T*d^2) and completely untouched. The best case is
under 8%, paid for in accuracy. Linear attention pays off at thousands of
tokens; a chess board is 64 tokens forever.

**Most LLM inference literature does not transfer.** KV-caching, paged
attention, and speculative decoding all target *autoregressive generation*.
Chess evaluation is a single non-autoregressive forward pass over a fixed-size
board. What does transfer is narrow: quantisation (2-4x), distillation
(5-10x), operator fusion, and batching. Note that even perfect int8 leaves the
default config **51x short**, so **efficiency engineering alone cannot rescue
the per-node design**.

---

## 6. The four candidate designs

| design | eval calls/move | hardware | verdict |
|---|---|---|---|
| alpha-beta + NNUE | 10^6-10^7 | CPU | **chosen** — proven route past 3000 |
| MCTS + transformer | 10^4-10^5 batched | GPU | viable, proven by Lc0, but see §7 |
| alpha-beta + transformer value at every node | 10^6-10^7 | CPU | **ruled out** — 51x to 1200x short |
| alpha-beta + NNUE value + **sparse transformer policy** | 10^6 cheap + 10^3 expensive | CPU | **the differentiator**, see §8 |

---

## 7. Why CPU rather than GPU

The decisive point is not the performance ceiling. **The strongest engine in
the world is the CPU path** — Stockfish, alpha-beta plus NNUE, ~3600 CCRL.
Lc0 on GPU is in the same tier, not above it. Choosing CPU sacrifices no
ceiling that matters at our current ~2500.

What actually differs is cost, and it is lopsided:

| | CPU / NNUE | GPU / MCTS transformer |
|---|---|---|
| user hardware | any x86-64, single binary | decent GPU for competitive nps |
| our training compute | modest: one consumer GPU, hours to days | very large: Lc0 used years of distributed volunteer GPU; DeepMind used TPUs and 15B positions |
| search rewrite | none, keep alpha-beta | full MCTS rewrite, most search work discarded |
| deployment | one binary | CUDA/TensorRT/ONNX backends, drivers, cross-platform |
| solo-developer risk | low, well-trodden | high |

A GPU is needed for *training* on either path — but only once, by us, not by
every user.

On whether a high-end GPU is required for the GPU path: for competitive
strength, effectively yes. Large nets want 4GB+ VRAM. Small nets run on modest
GPUs, but that is the trap — shrinking the net discards the eval advantage
that justified the GPU path to begin with.

---

## 8. The hybrid: transformer as a policy oracle

Move ordering sets the *shape* of the alpha-beta tree: with perfect ordering
the tree is b^(d/2) instead of b^d. That is an exponential lever, and unlike
evaluation it does **not** need to be consulted at every node — the root and
the first few plies are where ordering decides tree size.

Cost of sparse policy calls, as a percentage of a 1-second move:

| net | hardware | 100 calls | 1k | 10k |
|---|---|---|---|---|
| tiny (64, 2L) | int8 VNNI | 0.0% | **0.4%** | 3.8% |
| default (128, 4L) | int8 VNNI | 0.3% | **2.8%** | 27.7% |
| default (128, 4L) | fp32 AVX2 | 1.1% | **11.1%** | 110.8% |

Calling the existing default net at the root plus the first two or three plies
(~10^3 nodes) costs about **3% of a move** in int8. Practical today.

This uses the policy head already present in
`~/code/chess/training/model.py` (`Linear(embed_dim, 64)` producing a [64,64]
move matrix). It keeps alpha-beta, keeps CPU, and degrades gracefully: if the
policy net is unavailable or too slow on a given machine, the engine falls
back to conventional ordering and still works.

It is also, encouragingly, already **RQ3** in `paper/PLAN.md` — "Does adding a
policy head improve move ordering enough to offset extra inference cost?" —
and one of the plan's defensible claims. The two lines of work converged
independently, which is a reason to take it seriously.

---

## 9. Two objectives that must not be conflated

`paper/PLAN.md` proposes *Latency-Constrained Transformer Evaluation in an
Alpha-Beta Chess Engine*: a study of the strength-versus-latency curve for
transformer evaluators inside alpha-beta, including RQ4, "how close can it get
to an NNUE baseline".

That is legitimate research **even though §4 says such an engine loses to
NNUE**. Measuring where a tradeoff curve sits is a contribution; the plan's
own "claims to avoid" list is already honest about this, and its limitations
section already names "not incrementally updatable".

But it is a **different objective** from making haVoc strong:

- **Engine strength (3000+):** CPU, alpha-beta, NNUE value, optional sparse
  transformer policy. This document.
- **Research contribution:** the latency/strength curve for transformer
  evaluation in alpha-beta. `paper/PLAN.md`.

Both are worth doing and they share almost all infrastructure. They should not
share a success metric. A future session that conflates them will either spend
engine-strength effort on a design that cannot reach 3000, or judge the paper
by whether it beat Stockfish.

---

## 10. What this means for engine work

The reassuring part: **every foundation item below carries over unchanged
under all four designs in §6.** Nothing here is a bet on the eval choice.

**Tier 1 — do now, carries over completely**

1. **Datagen throughput and correctness.** The long pole. A good NNUE needs on
   the order of 10^9 well-labelled positions, and wrong labels poison
   everything downstream. This is CPU work on a 32-thread box.
2. **Thread scaling.** Fully eval-agnostic and it multiplies datagen
   throughput directly. `kMaxThreads` is 1024 with a default of 1, which
   suggests Lazy SMP has never been seriously validated. Measure nps *and*
   Elo against thread count before assuming it scales.
3. **Transposition table** — replacement policy, bucket layout, aging.
4. **Move ordering** — SEE, history, killers, countermoves are all
   eval-independent, and this is where the policy hybrid eventually plugs in.
5. **The incremental-evaluation seam.** `do_move(const Move&)` exposes no
   piece delta. An accumulator needs `(piece, from, to, captured)`. Adding
   that seam now, while the evaluation is cheap and every change is testable
   against the HCE, is far safer than surgery on the hottest path later.

**Tier 2 — deprioritised, with reasons**

- **SPSA of search margins (roadmap §4.1).** RFP, futility, delta and
  aspiration margins are calibrated to the *scale and noise* of the current
  evaluation. Replacing the evaluator invalidates all of them. Tuning them now
  is work with a scheduled expiry date.
- **Movegen micro-optimisation, justified by engine Elo.** Amdahl cuts against
  it once a heavier evaluation dominates node cost. Still worth doing — but
  measure it in *datagen positions/sec*, which is the reason it matters.
- **HCE magnitude work** (roadmap §4.2, §4.4, §4.5). See §2.

---

## 11. What would overturn this decision

State these plainly so the decision can be revisited on evidence:

- A measured NNUE that lands well short of the ~+300-400 Elo the literature
  suggests, implying our data or training is the binding constraint rather
  than the architecture.
- Hardware change: an int4/sparse path, or a machine with an order of
  magnitude more CPU matmul throughput, moves the §4 table.
- Evidence that the sparse policy hybrid gives a large enough tree reduction
  that a slower, better evaluation becomes affordable after all.
- A decision to accept GPU as a hard requirement, which makes MCTS plus a
  large transformer the obvious choice instead.
