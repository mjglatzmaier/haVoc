"""Quantise a trained checkpoint into the .nnue file the engine loads.

The arithmetic here is the *definition* of what the engine will compute, and
`include/havoc/nnue/network.hpp` mirrors it exactly. Not approximately:
`--verify` simulates the integer path in NumPy and writes a case file that a
C++ test replays, and the two are required to agree on every position. Two
implementations that have merely been observed to be similar are two
implementations, and the difference between them will surface as an engine
that plays slightly badly for reasons no test reports.

Derivation of the scales, since getting a bias scale wrong is silent:

    A  = round(ft_b*QA) + sum(round(ft_w*QA))     ~ a  * QA
    X  = clamp(A, 0, QA)                          ~ x  * QA
    Y1 = FC1_W @ X + FC1_B                        ~ y1 * QA*s1   (FC1_W = w*s1)
    H1 = clamp(Y1 // s1, 0, QA)                   ~ h1 * QA
    ...
    eval_cp = O * CP_SCALE / (QA*s_out)

so each dense layer's bias is stored at scale QA*s_L and its weights at s_L,
where s_L is fitted to that layer alone.

One rounding detail that must not drift: C++ integer division truncates
toward zero, Python's `//` floors. They differ only for negative values, and
after the hidden layers the clip to [0, QA] hides the difference -- but the
final output is not clipped, so it is converted here with explicit
truncation-toward-zero to match the engine.
"""

from __future__ import annotations

import argparse
import os
import struct
import sys

import numpy as np
import torch

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import dataset as ds_mod  # noqa: E402
from model import NNUE, PADDING_INDEX, sanitise  # noqa: E402

MAGIC = b"HVNW"
NETWORK_FORMAT_VERSION = 1

# Accumulator/activation scale.
QA = 255

# Dense weights are int8. Each layer's scale is fitted to that layer's own
# largest weight rather than shared, because sharing one was measured to cost
# 46 cp: a scale of 64 (the training clamp, 127/64) left two thirds of the
# int8 range unused on a network whose largest weight was 1.25, and a third of
# fc1's weights -- rms 0.062 -- fell below one quantisation step. Fitting per
# layer cut that to 9.2 cp at no cost.
INT8_MAX = 127


def fit_scale(w) -> int:
    """The largest scale that keeps every weight of `w` inside int8."""
    peak = float(np.abs(w.detach().cpu().numpy()).max())
    if peak == 0.0:
        return 1
    return max(1, int(np.floor(INT8_MAX / peak)))


def trunc_div(x: np.ndarray, d: int) -> np.ndarray:
    """Integer division that truncates toward zero, as C++ does."""
    return np.sign(x) * (np.abs(x) // d)


class Quantised:
    def __init__(self, ckpt: dict):
        sd = ckpt["state_dict"]
        arch = ckpt["arch"]
        self.l1, self.l2, self.l3 = arch["l1"], arch["l2"], arch["l3"]
        self.cp_scale = int(ckpt["cp_scale"])
        self.feature_set_version = int(ckpt["feature_set_version"])

        def q(t, scale, dtype, lo, hi, name):
            a = np.round(t.detach().cpu().numpy().astype(np.float64) * scale)
            if a.min() < lo or a.max() > hi:
                clipped = int((a < lo).sum() + (a > hi).sum())
                print(
                    f"  warning: {name} saturated on {clipped} of {a.size} weights "
                    f"(range {a.min():.0f}..{a.max():.0f}, storable {lo}..{hi})"
                )
            return np.clip(a, lo, hi).astype(dtype)

        ft = sd["transformer.weight"]
        assert ft.shape[0] == ds_mod.INPUT_DIM + 1, "unexpected transformer size"
        # Drop the padding row: it is structurally zero and the engine has no
        # slot for it, since an inactive feature is simply not accumulated.
        self.ft_w = q(ft[:PADDING_INDEX], QA, np.int16, -32768, 32767, "ft.weight")
        self.ft_b = q(sd["transformer_bias"], QA, np.int16, -32768, 32767, "ft.bias")

        self.s_fc1 = fit_scale(sd["fc1.weight"])
        self.s_fc2 = fit_scale(sd["fc2.weight"])
        self.s_out = fit_scale(sd["out.weight"])
        print(f"  int8 scales: fc1={self.s_fc1} fc2={self.s_fc2} out={self.s_out}")

        self.fc1_w = q(sd["fc1.weight"], self.s_fc1, np.int8, -127, 127, "fc1.weight")
        self.fc1_b = q(sd["fc1.bias"], QA * self.s_fc1, np.int32, -(2**31), 2**31 - 1, "fc1.bias")
        self.fc2_w = q(sd["fc2.weight"], self.s_fc2, np.int8, -127, 127, "fc2.weight")
        self.fc2_b = q(sd["fc2.bias"], QA * self.s_fc2, np.int32, -(2**31), 2**31 - 1, "fc2.bias")
        self.out_w = q(sd["out.weight"], self.s_out, np.int8, -127, 127, "out.weight")
        self.out_b = int(
            q(sd["out.bias"], QA * self.s_out, np.int64, -(2**31), 2**31 - 1, "out.bias")[0]
        )

    def evaluate(self, feat_us: np.ndarray, feat_them: np.ndarray) -> np.ndarray:
        """The exact integer forward pass, batched. Returns centipawns."""

        def accumulate(feats: np.ndarray) -> np.ndarray:
            acc = np.tile(self.ft_b.astype(np.int32), (feats.shape[0], 1))
            active = feats != ds_mod.NO_FEATURE
            for slot in range(feats.shape[1]):
                col = feats[:, slot]
                m = active[:, slot]
                if not m.any():
                    continue
                acc[m] += self.ft_w[col[m].astype(np.int64)].astype(np.int32)
            return acc

        x = np.concatenate(
            [
                np.clip(accumulate(feat_us), 0, QA),
                np.clip(accumulate(feat_them), 0, QA),
            ],
            axis=1,
        ).astype(np.int32)

        y1 = x @ self.fc1_w.astype(np.int32).T + self.fc1_b
        h1 = np.clip(trunc_div(y1, self.s_fc1), 0, QA).astype(np.int32)
        y2 = h1 @ self.fc2_w.astype(np.int32).T + self.fc2_b
        h2 = np.clip(trunc_div(y2, self.s_fc2), 0, QA).astype(np.int32)
        o = (h2 @ self.out_w.astype(np.int64).T).ravel() + self.out_b
        return trunc_div(o * self.cp_scale, QA * self.s_out).astype(np.int64)

    def write(self, path: str) -> None:
        with open(path, "wb") as f:
            header = struct.pack(
                "<4sIIIIIIiiiii16s",
                MAGIC,
                NETWORK_FORMAT_VERSION,
                self.feature_set_version,
                ds_mod.INPUT_DIM,
                self.l1,
                self.l2,
                self.l3,
                self.cp_scale,
                QA,
                self.s_fc1,
                self.s_fc2,
                self.s_out,
                b"\0" * 16,
            )
            assert len(header) == 64, len(header)
            f.write(header)
            for arr in (
                self.ft_w,
                self.ft_b,
                self.fc1_w,
                self.fc1_b,
                self.fc2_w,
                self.fc2_b,
                self.out_w,
            ):
                f.write(np.ascontiguousarray(arr).tobytes())
            f.write(struct.pack("<i", self.out_b))


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--ckpt", required=True)
    ap.add_argument("--out", required=True)
    ap.add_argument("--verify-data", default=None, help=".hbin to verify against")
    ap.add_argument("--verify-n", type=int, default=10000)
    ap.add_argument(
        "--cases",
        default=None,
        help="Write the verification positions and their integer evaluations "
        "for the C++ exactness test to replay.",
    )
    args = ap.parse_args()

    ckpt = torch.load(args.ckpt, map_location="cpu", weights_only=False)
    print(f"checkpoint: arch={ckpt['arch']}  val MAE {ckpt.get('val_mae_cp', float('nan')):.2f} cp")
    qn = Quantised(ckpt)
    qn.write(args.out)
    print(f"wrote {args.out} ({os.path.getsize(args.out) / 1e6:.1f} MB)")

    if not args.verify_data:
        return 0

    data = ds_mod.load(args.verify_data, limit=args.verify_n)
    rec = data.records
    black = rec["stm"].astype(bool)
    feat_us = np.where(black[:, None], rec["feat_black"], rec["feat_white"])
    feat_them = np.where(black[:, None], rec["feat_white"], rec["feat_black"])

    int_eval = qn.evaluate(feat_us, feat_them)

    net = NNUE(qn.l1, qn.l2, qn.l3)
    net.load_state_dict(ckpt["state_dict"])
    net.eval()
    with torch.no_grad():
        f_us = sanitise(torch.from_numpy(feat_us.astype(np.int64)))
        f_them = sanitise(torch.from_numpy(feat_them.astype(np.int64)))
        float_eval = (net(f_us, f_them) * qn.cp_scale).numpy()

    label = rec["score"].astype(np.float64)
    d = np.abs(int_eval - float_eval)
    print(
        f"\nquantisation cost, {len(rec):,} positions:\n"
        f"  float  vs label   MAE {np.abs(float_eval - label).mean():7.2f} cp\n"
        f"  int8   vs label   MAE {np.abs(int_eval - label).mean():7.2f} cp\n"
        f"  int8   vs float   MAE {d.mean():7.2f} cp   max {d.max():.0f} cp"
    )

    if args.cases:
        # Fixed-stride case file: n, feat_us[30], feat_them[30], expected cp.
        with open(args.cases, "wb") as f:
            f.write(struct.pack("<I", len(rec)))
            for i in range(len(rec)):
                f.write(np.ascontiguousarray(feat_us[i].astype(np.uint16)).tobytes())
                f.write(np.ascontiguousarray(feat_them[i].astype(np.uint16)).tobytes())
                f.write(struct.pack("<i", int(int_eval[i])))
        print(f"  wrote {args.cases} for the C++ exactness test")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
