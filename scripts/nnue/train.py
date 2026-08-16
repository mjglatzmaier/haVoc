"""Train an NNUE on a .hbin corpus.

Stage 0 of the plan in docs/nnue-integration.md is run with `--label-check
hce`: the network is trained to reproduce haVoc's own static evaluation. That
target is deterministic and already in our possession, which makes the run a
*known-answer test* of everything except the idea itself -- the feature
export, the loader, the model shape, the loss, the optimiser, and later the
quantisation and the C++ kernel. If a network cannot learn a function we can
compute, nothing downstream is worth debugging yet, and finding that out costs
hours instead of the days a full run costs.

How the validation set is chosen, and why it is not random
-----------------------------------------------------------
A random split of this corpus does not measure generalisation. The file is
written by self-play datagen in game order, so neighbouring records are
consecutive plies of the same game: sampled 20,000 records apart they share
0.8% of their features, but *adjacent* ones share 85%, and 11.5% of feature
vectors appear more than once outright. A random split therefore puts each
validation position's near-twin -- often its exact twin -- in the training set,
and the resulting number is a measure of memorisation.

That is not a hypothetical. Stage 0 reported a 26.7 cp held-out error this way.
Measured on positions from real games instead, the same network was **82 cp**
off, and the engine using it lost 111 Elo at fixed depth. The split was the bug.

So the validation set is a *contiguous block* at the end of the file, separated
from the training data by a gap of whole games, and any validation record whose
feature vector also occurs in training is dropped. `--val-file` overrides all
of it with a genuinely independent corpus, which is the only fully honest
option and is what a real run should use.

The dataset is held resident on the GPU. At 128 bytes a record, several
million positions fit in VRAM with room to spare, and keeping them there
removes the host-to-device copy that otherwise dominates a network this small.
When the corpus outgrows VRAM this is the thing to change first.
"""

from __future__ import annotations

import argparse
import json
import os
import sys
import time

import numpy as np
import torch

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import dataset as ds_mod  # noqa: E402
from model import NNUE, sanitise  # noqa: E402

# Centipawns per unit of network output. Only a convention, but it must be the
# same one the C++ side uses, so it is written into the checkpoint.
CP_SCALE = 400.0


def build_tensors(records: np.ndarray, device: torch.device):
    """Reorder the two perspectives into (us, them) and move everything over.

    The reordering happens once, here, rather than per batch. `stm` is the
    only thing that decides it, and getting it wrong is silent: the network
    still trains, it just learns to evaluate from the wrong chair.
    """
    feat_w = torch.from_numpy(records["feat_white"].astype(np.int32)).to(device)
    feat_b = torch.from_numpy(records["feat_black"].astype(np.int32)).to(device)
    black_to_move = torch.from_numpy(records["stm"].astype(np.bool_)).to(device)

    swap = black_to_move.unsqueeze(1)
    feat_us = torch.where(swap, feat_b, feat_w)
    feat_them = torch.where(swap, feat_w, feat_b)

    score = torch.from_numpy(records["score"].astype(np.float32)).to(device)

    # The game's outcome, from the side to move's chair. datagen plays every
    # game to the end and writes the result, and nnue_export already stores
    # it; until now the trainer threw it away. It is the only label we have
    # that does not pass through the evaluation, so it is the only one whose
    # quality is not capped by the evaluation's -- and haVoc's static eval is
    # 93.9 cp MAE against its own depth-12 search.
    #
    # 255 marks a result the exporter could not read. Those rows fall back to
    # the score alone rather than being silently scored as a draw.
    raw = records["result"].astype(np.int16)
    known = torch.from_numpy((raw != 255)).to(device)
    white_pov = torch.from_numpy(np.clip(raw, 0, 2).astype(np.float32) / 2.0).to(device)
    outcome = torch.where(black_to_move, 1.0 - white_pov, white_pov)
    return feat_us, feat_them, score, outcome, known


def report_wdl_scale(score: torch.Tensor, outcome: torch.Tensor, known: torch.Tensor) -> None:
    """Fit `p = sigmoid(score / scale)` against what actually happened.

    Copying this constant from another engine would be guessing: it depends on
    the draw rate, which depends on the engine, the labelling depth and the
    opening book. Measuring it costs a second and also sanity-checks the
    `result`/`stm` encoding -- if the outcome column were misaligned, the
    curve would be flat and the mean would not sit at 0.5.
    """
    sc = score[known].double()
    ou = outcome[known].double()
    print(f"win-probability fit on {len(sc):,} labelled positions")
    print(f"  outcome mean {ou.mean().item():.4f} from the side to move (0.5 = unbiased)")
    edges = [-2000, -800, -400, -250, -150, -75, -25, 25, 75, 150, 250, 400, 800, 2000]
    xs, ws, ys = [], [], []
    print("      score bucket |       n | empirical win rate")
    for lo, hi in zip(edges[:-1], edges[1:]):
        m = (sc >= lo) & (sc < hi)
        n = int(m.sum().item())
        if n < 200:
            continue
        rate = ou[m].mean().item()
        print(f"  {lo:6d}..{hi:<6d} | {n:7,} | {rate:.3f}")
        xs.append(0.5 * (lo + hi))
        ws.append(float(n))
        ys.append(rate)
    x = np.array(xs)
    w = np.array(ws)
    y = np.array(ys)
    ok = (y > 0.02) & (y < 0.98)
    if ok.sum() < 3:
        print("  too few usable buckets to fit a scale; is the result column populated?")
        return
    logit = np.log(y[ok] / (1.0 - y[ok]))
    denom = float((w[ok] * x[ok] * logit).sum())
    if abs(denom) < 1e-9:
        print("  degenerate fit; the win rate does not track the score at all")
        return
    print(f"  fitted --wdl-scale {float((w[ok] * x[ok] * x[ok]).sum() / denom):.1f}")


def drop_leaked(records: np.ndarray, train_idx: np.ndarray, val_idx: np.ndarray) -> np.ndarray:
    """Remove validation records whose exact position also occurs in training.

    A gap between the two blocks separates games, not positions: common
    openings and simplified endgames recur across games, and a repeated
    position is a memorised one. Comparing the raw feature bytes is exact and
    costs a second, which is cheap next to reporting a number that is wrong.
    """
    view = np.ascontiguousarray(
        np.stack(
            [records["feat_white"].view(np.uint8), records["feat_black"].view(np.uint8)], axis=1
        )
    ).reshape(len(records), -1)
    key = view.view([("", view.dtype)] * view.shape[1]).ravel()

    train_keys = np.unique(key[train_idx])
    leaked = np.isin(key[val_idx], train_keys)
    n = int(leaked.sum())
    if n:
        print(f"dropped {n:,} of {len(val_idx):,} validation positions that also occur in training")
    return val_idx[~leaked]


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--data", required=True)
    ap.add_argument("--out", default="nnue.pt")
    ap.add_argument("--epochs", type=int, default=20)
    ap.add_argument("--batch", type=int, default=16384)
    ap.add_argument("--lr", type=float, default=1e-3)
    ap.add_argument("--l1", type=int, default=256)
    ap.add_argument("--l2", type=int, default=32)
    ap.add_argument("--l3", type=int, default=32)
    ap.add_argument("--limit", type=int, default=None)
    ap.add_argument("--val-frac", type=float, default=0.05)
    ap.add_argument(
        "--val-file",
        default=None,
        help="An independent .hbin to validate on. Overrides --val-frac and is "
        "the only measurement that is honest by construction.",
    )
    ap.add_argument(
        "--val-limit",
        type=int,
        default=None,
        help="Cap on records taken from --val-file. The validation set is "
        "re-scored every epoch, so an oversized one buys no precision and "
        "costs real time on a long run.",
    )
    ap.add_argument(
        "--val-gap",
        type=int,
        default=200_000,
        help="Records dropped between the training block and the validation "
        "block, so the two cannot share a game.",
    )
    ap.add_argument(
        "--wdl-lambda",
        type=float,
        default=None,
        help="Blend the game's outcome into the target: "
        "lambda * sigmoid(score/scale) + (1 - lambda) * outcome, with the loss "
        "taken in win-probability space. 1.0 is the search score alone (but "
        "still in probability space); 0.0 is the outcome alone. Omit the flag "
        "entirely to keep the plain centipawn regression, which is what every "
        "measurement before this used.",
    )
    ap.add_argument(
        "--wdl-scale",
        type=float,
        default=166.0,
        help="Centipawns per unit of logit: p = sigmoid(score / scale). The "
        "default is fitted to haVoc's own depth-12 self-play, where the "
        "empirical win rate runs 0.498 at 0 cp and 0.810 at +200 cp. Refit it "
        "with --fit-wdl-scale if the labelling depth or engine changes.",
    )
    ap.add_argument(
        "--fit-wdl-scale",
        action="store_true",
        help="Report the scale implied by this corpus's own scores and "
        "results, then exit. Costs nothing and beats copying a constant out "
        "of another engine.",
    )
    ap.add_argument("--seed", type=int, default=1)
    ap.add_argument(
        "--qat",
        action="store_true",
        help="Quantisation-aware training: round the weights onto the int8 "
        "grid in the forward pass, so the network learns weights that survive "
        "export instead of relying on precision it will not have.",
    )
    ap.add_argument(
        "--expect-label",
        choices=["hce_static", "search"],
        default=None,
        help="Refuse to train if the file's labels are not these. Stops a "
        "known-answer run from quietly becoming a real one, or vice versa.",
    )
    args = ap.parse_args()

    torch.manual_seed(args.seed)
    np.random.seed(args.seed)

    if not torch.cuda.is_available():
        print("no CUDA device; this is trainable on CPU but not on this timescale")
    device = torch.device("cuda" if torch.cuda.is_available() else "cpu")

    data = ds_mod.load(args.data, limit=args.limit)
    print(ds_mod.describe(data))
    if args.expect_label and data.label_kind != args.expect_label:
        print(
            f"refusing: --expect-label {args.expect_label} but file holds "
            f"{data.label_kind} labels"
        )
        return 1

    if args.val_file:
        val_data = ds_mod.load(args.val_file, limit=args.val_limit)
        if val_data.label_kind != data.label_kind:
            print(
                f"refusing: training on {data.label_kind} labels but validating "
                f"on {val_data.label_kind}"
            )
            return 1
        records = np.concatenate([data.records, val_data.records])
        train_idx = np.arange(len(data), dtype=np.int64)
        val_idx = np.arange(len(data), len(records), dtype=np.int64)
        print(f"validating on {args.val_file}: {len(val_idx):,} independent positions")
    else:
        records = data.records
        n_val = max(1, int(len(records) * args.val_frac))
        gap = min(args.val_gap, max(0, len(records) - n_val - 1))
        val_idx = np.arange(len(records) - n_val, len(records), dtype=np.int64)
        train_idx = np.arange(0, len(records) - n_val - gap, dtype=np.int64)
        print(
            f"contiguous split: train [0,{train_idx[-1]:,}]  gap {gap:,}  "
            f"val [{val_idx[0]:,},{val_idx[-1]:,}]"
        )

    val_idx = drop_leaked(records, train_idx, val_idx)

    feat_us, feat_them, score, outcome, outcome_known = build_tensors(records, device)
    del data
    tr = torch.from_numpy(train_idx.astype(np.int64)).to(device)
    va = torch.from_numpy(val_idx.astype(np.int64)).to(device)
    print(f"train {len(tr):,}  val {len(va):,}  device {device}")
    # --val-gap defaults to 200k records, which is right for a real corpus and
    # silently destroys a small one: on a 199k-record file it left *one*
    # training position, and two different losses then produced identical
    # numbers because neither had trained. Fail loudly instead.
    if len(tr) < max(1000, len(records) // 100):
        print(
            f"refusing: only {len(tr):,} training records survive a --val-gap of "
            f"{args.val_gap:,} on a {len(records):,}-record corpus. Lower --val-gap "
            f"or pass --val-file."
        )
        return 1

    if args.fit_wdl_scale:
        # Every record, not the training split: this is a property of the
        # corpus, and the split may legitimately be tiny.
        report_wdl_scale(score, outcome, outcome_known)
        return 0

    def target_probability(b: torch.Tensor) -> torch.Tensor:
        """lambda * sigmoid(score/scale) + (1 - lambda) * outcome.

        Rows whose outcome the exporter could not read fall back to the score
        alone. Scoring them as a draw instead would be a quiet lie in exactly
        the direction the blend is meant to correct.
        """
        p_score = torch.sigmoid(score[b] / args.wdl_scale)
        lam = torch.where(
            outcome_known[b], torch.full_like(p_score, args.wdl_lambda), torch.ones_like(p_score)
        )
        return lam * p_score + (1.0 - lam) * outcome[b]

    net = NNUE(args.l1, args.l2, args.l3, qat=args.qat).to(device)
    opt = torch.optim.AdamW(net.parameters(), lr=args.lr, weight_decay=0.0)
    sched = torch.optim.lr_scheduler.CosineAnnealingLR(opt, T_max=args.epochs)

    def evaluate(idx: torch.Tensor) -> float:
        """Mean absolute error, in centipawns, which is the unit we care about."""
        net.eval()
        total, n = 0.0, 0
        with torch.no_grad():
            for lo in range(0, len(idx), args.batch):
                b = idx[lo : lo + args.batch]
                pred = net(sanitise(feat_us[b]).long(), sanitise(feat_them[b]).long())
                total += (pred * CP_SCALE - score[b]).abs().sum().item()
                n += len(b)
        return total / max(1, n)

    best = float("inf")
    for epoch in range(args.epochs):
        net.train()
        shuffled = tr[torch.randperm(len(tr), device=device)]
        t0, running, batches = time.time(), 0.0, 0
        for lo in range(0, len(shuffled), args.batch):
            b = shuffled[lo : lo + args.batch]
            pred = net(sanitise(feat_us[b]).long(), sanitise(feat_them[b]).long())
            if args.wdl_lambda is None:
                loss = torch.nn.functional.huber_loss(pred, score[b] / CP_SCALE, delta=1.0)
            else:
                # In probability space a blunder near 0 cp matters and a
                # rounding error at +900 does not, which is the weighting the
                # centipawn loss gets backwards.
                loss = torch.nn.functional.mse_loss(
                    torch.sigmoid(pred * CP_SCALE / args.wdl_scale), target_probability(b)
                )
            opt.zero_grad(set_to_none=True)
            loss.backward()
            opt.step()
            net.clamp_for_quantisation()
            running += loss.item()
            batches += 1
        sched.step()

        val_mae = evaluate(va)
        flag = ""
        if val_mae < best:
            best = val_mae
            torch.save(
                {
                    "state_dict": net.state_dict(),
                    "arch": {"l1": args.l1, "l2": args.l2, "l3": args.l3},
                    "cp_scale": CP_SCALE,
                    "feature_set_version": ds_mod.FEATURE_SET_VERSION,
                    "qat": args.qat,
                    "val_mae_cp": val_mae,
                    "epoch": epoch,
                },
                args.out,
            )
            flag = "  *"
        print(
            f"epoch {epoch:3d}  loss {running / max(1, batches):.5f}  "
            f"val MAE {val_mae:7.2f} cp  {time.time() - t0:5.1f}s{flag}"
        )

    print(f"\nbest val MAE {best:.2f} cp -> {args.out}")
    meta = args.out + ".json"
    with open(meta, "w") as f:
        json.dump({"val_mae_cp": best, "args": vars(args)}, f, indent=2)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
