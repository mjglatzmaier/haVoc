"""Train an NNUE on a .hbin corpus.

Stage 0 of the plan in docs/nnue-integration.md is run with `--label-check
hce`: the network is trained to reproduce haVoc's own static evaluation. That
target is deterministic and already in our possession, which makes the run a
*known-answer test* of everything except the idea itself -- the feature
export, the loader, the model shape, the loss, the optimiser, and later the
quantisation and the C++ kernel. If a network cannot learn a function we can
compute, nothing downstream is worth debugging yet, and finding that out costs
hours instead of the days a full run costs.

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
    return feat_us, feat_them, score


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

    perm = np.random.permutation(len(data))
    n_val = max(1, int(len(data) * args.val_frac))
    val_idx, train_idx = perm[:n_val], perm[n_val:]

    feat_us, feat_them, score = build_tensors(data.records, device)
    del data
    tr = torch.from_numpy(train_idx.astype(np.int64)).to(device)
    va = torch.from_numpy(val_idx.astype(np.int64)).to(device)
    print(f"train {len(tr):,}  val {len(va):,}  device {device}")

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
            loss = torch.nn.functional.huber_loss(pred, score[b] / CP_SCALE, delta=1.0)
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
