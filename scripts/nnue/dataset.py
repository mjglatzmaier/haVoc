"""Reader for the .hbin training files written by tools/nnue_export.cpp.

The point of this module is how *little* it does. It does not know what a
feature means, does not parse a FEN, and does not construct an index. All of
that lives in include/havoc/nnue/features.hpp and arrives here already
computed, because a second implementation of the feature encoding is the
single most expensive bug available in an NNUE project: it trains cleanly
against itself and only shows up as a network that is mysteriously weak.

What this module does check is that the file it was handed was produced by the
encoder the engine currently contains. If the feature set has been changed
since the data was generated, that is a hard error, not a warning.
"""

from __future__ import annotations

import dataclasses
import numpy as np

MAGIC = b"HVNN"
FORMAT_VERSION = 1
MAX_ACTIVE = 30
RECORD_BYTES = 128
HEADER_BYTES = 64
NO_FEATURE = 0xFFFF

# Must match include/havoc/nnue/features.hpp.
FEATURE_SET_VERSION = 1
INPUT_DIM = 40960

LABEL_KIND = {0: "hce_static", 1: "search"}

HEADER_DTYPE = np.dtype(
    [
        ("magic", "S4"),
        ("format_version", "<u4"),
        ("feature_set_version", "<u4"),
        ("input_dim", "<u4"),
        ("max_active", "<u4"),
        ("record_bytes", "<u4"),
        ("label_kind", "<u4"),
        ("count", "<u8"),
        ("reserved", "S28"),
    ]
)
assert HEADER_DTYPE.itemsize == HEADER_BYTES

RECORD_DTYPE = np.dtype(
    [
        ("score", "<i2"),
        ("stm", "u1"),
        ("result", "u1"),
        ("n", "u1"),
        ("pad", "S3"),
        ("feat_white", "<u2", (MAX_ACTIVE,)),
        ("feat_black", "<u2", (MAX_ACTIVE,)),
    ]
)
assert RECORD_DTYPE.itemsize == RECORD_BYTES


@dataclasses.dataclass
class Dataset:
    records: np.ndarray
    label_kind: str

    def __len__(self) -> int:
        return len(self.records)


def load(path: str, limit: int | None = None, mmap: bool = False) -> Dataset:
    """Load a .hbin file, refusing anything this trainer cannot interpret."""
    header = np.fromfile(path, dtype=HEADER_DTYPE, count=1)[0]

    if bytes(header["magic"]) != MAGIC:
        raise ValueError(f"{path}: not a haVoc NNUE dataset (bad magic)")
    if header["format_version"] != FORMAT_VERSION:
        raise ValueError(
            f"{path}: format version {header['format_version']}, this trainer speaks "
            f"{FORMAT_VERSION}"
        )
    if header["feature_set_version"] != FEATURE_SET_VERSION:
        raise ValueError(
            f"{path}: generated with feature set v{header['feature_set_version']}, the "
            f"engine now uses v{FEATURE_SET_VERSION}. Regenerate the data; training on "
            f"it would silently learn the wrong board."
        )
    if header["input_dim"] != INPUT_DIM or header["max_active"] != MAX_ACTIVE:
        raise ValueError(f"{path}: geometry mismatch with this trainer")
    if header["record_bytes"] != RECORD_BYTES:
        raise ValueError(f"{path}: record stride mismatch")

    count = int(header["count"])
    if limit is not None:
        count = min(count, limit)

    if mmap:
        records = np.memmap(
            path, dtype=RECORD_DTYPE, mode="r", offset=HEADER_BYTES, shape=(count,)
        )
    else:
        records = np.fromfile(path, dtype=RECORD_DTYPE, count=count, offset=HEADER_BYTES)

    if len(records) != count:
        raise ValueError(
            f"{path}: header claims {count} records, file holds {len(records)}. "
            f"The export was probably interrupted."
        )

    return Dataset(records=records, label_kind=LABEL_KIND[int(header["label_kind"])])


def describe(ds: Dataset) -> str:
    scores = ds.records["score"].astype(np.float64)
    n = ds.records["n"].astype(np.int32)
    return (
        f"{len(ds):,} positions, labels={ds.label_kind}\n"
        f"  score  mean {scores.mean():8.2f}  std {scores.std():8.2f}  "
        f"min {scores.min():6.0f}  max {scores.max():6.0f}\n"
        f"  |score| median {np.median(np.abs(scores)):8.2f}\n"
        f"  men    mean {n.mean():5.1f}  min {n.min()}  max {n.max()}\n"
        f"  stm    {100.0 * ds.records['stm'].mean():.1f}% black to move"
    )
