"""The network.

Shape is the standard NNUE one and is deliberately unoriginal: two
perspective accumulators sharing one feature transformer, concatenated
side-to-move first, then a small dense head.

    40960 --(shared)--> L1        per perspective
    [us | them] = 2*L1 --> L2 --> L3 --> 1

Two details are load-bearing rather than stylistic:

*   **The feature transformer is shared between perspectives.** Both are the
    same function of "the board as seen by whoever is looking", which is what
    makes the orientation in features.hpp meaningful and halves the parameters
    that matter most.

*   **The concatenation is side-to-move first**, never white first. The
    network must be able to express "it is my move", and it can only do that
    if the ordering carries the information. Getting this backwards produces a
    network that trains to a plausible loss and evaluates the wrong side.

The activation is clipped ReLU in [0, 1]. That is not an aesthetic choice: it
is what makes the later int8 quantisation lossless in the activation range,
because a bounded activation maps onto a fixed-point range exactly. Training
with plain ReLU and quantising afterwards is how a network loses most of its
strength between Python and C++.
"""

from __future__ import annotations

import torch
import torch.nn as nn

from dataset import INPUT_DIM, MAX_ACTIVE, NO_FEATURE

# The padding row: inactive feature slots in a fixed-stride record point here
# and contribute nothing. It is a real row so that indexing stays branch-free.
PADDING_INDEX = INPUT_DIM


# Accumulator/activation scale, mirrored from quantise.py and network.hpp.
QA = 255
INT8_MAX = 127


def clipped_relu(x: torch.Tensor) -> torch.Tensor:
    return torch.clamp(x, 0.0, 1.0)


def _round_ste(x: torch.Tensor) -> torch.Tensor:
    """Round in the forward pass, pass the gradient through unchanged.

    The straight-through estimator. Rounding has zero gradient almost
    everywhere, so training through it directly would stop the layer learning;
    pretending the derivative is 1 is the standard and effective lie.
    """
    return x + (x.round() - x).detach()


def fake_quant_weight(w: torch.Tensor) -> torch.Tensor:
    """Round a weight tensor onto the int8 grid it will actually be stored on.

    The scale is fitted to the tensor's own peak, exactly as `quantise.py`
    does, so what the network sees during training is what the engine will
    later compute with. Without this the network is free to rely on precision
    that does not survive export -- which cost 46 cp before the per-layer
    scales landed, and still costs about 9 cp after them.
    """
    peak = w.abs().max().clamp(min=1e-8)
    scale = torch.floor(INT8_MAX / peak).clamp(min=1.0)
    return _round_ste(w * scale) / scale


def fake_quant_activation(x: torch.Tensor) -> torch.Tensor:
    return _round_ste(x * QA) / QA


class NNUE(nn.Module):
    def __init__(self, l1: int = 256, l2: int = 32, l3: int = 32, qat: bool = False):
        super().__init__()
        self.l1, self.l2, self.l3 = l1, l2, l3
        self.qat = qat
        self.transformer = nn.EmbeddingBag(
            INPUT_DIM + 1, l1, mode="sum", padding_idx=PADDING_INDEX
        )
        self.transformer_bias = nn.Parameter(torch.zeros(l1))
        self.fc1 = nn.Linear(2 * l1, l2)
        self.fc2 = nn.Linear(l2, l3)
        self.out = nn.Linear(l3, 1)

        # Small init on the transformer: an accumulator is a sum of ~30 rows,
        # so the default per-row scale lands the pre-activation far outside
        # the [0, 1] clip and the whole layer starts saturated and dead.
        nn.init.normal_(self.transformer.weight, std=0.01)
        with torch.no_grad():
            self.transformer.weight[PADDING_INDEX].zero_()

    def accumulate(self, feats: torch.Tensor) -> torch.Tensor:
        return self.transformer(feats) + self.transformer_bias

    def _dense(self, layer: nn.Linear, x: torch.Tensor) -> torch.Tensor:
        w = fake_quant_weight(layer.weight) if self.qat else layer.weight
        return torch.nn.functional.linear(x, w, layer.bias)

    def forward(self, feat_us: torch.Tensor, feat_them: torch.Tensor) -> torch.Tensor:
        acc_us = clipped_relu(self.accumulate(feat_us))
        acc_them = clipped_relu(self.accumulate(feat_them))
        x = torch.cat([acc_us, acc_them], dim=1)
        if self.qat:
            x = fake_quant_activation(x)
        x = clipped_relu(self._dense(self.fc1, x))
        if self.qat:
            x = fake_quant_activation(x)
        x = clipped_relu(self._dense(self.fc2, x))
        if self.qat:
            x = fake_quant_activation(x)
        return self._dense(self.out, x).squeeze(1)

    @torch.no_grad()
    def clamp_for_quantisation(self, weight_max: float = 127.0 / 64.0) -> None:
        """Keep the dense layers inside the range int8 quantisation can hold.

        Applied every step rather than once at the end, so the network trains
        *within* the representable set instead of being projected into it
        afterwards and losing accuracy it was relying on.
        """
        for layer in (self.fc1, self.fc2, self.out):
            layer.weight.clamp_(-weight_max, weight_max)


def sanitise(feats: torch.Tensor) -> torch.Tensor:
    """Map the export's 0xFFFF padding sentinel onto the padding row."""
    return torch.where(
        feats == NO_FEATURE, torch.full_like(feats, PADDING_INDEX), feats
    )
