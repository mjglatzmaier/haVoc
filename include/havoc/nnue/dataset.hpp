#pragma once

/// The on-disk training record, defined once so that the exporter and the
/// trainer cannot disagree about it.
///
/// The layout is fixed-size on purpose. A variable-length record would be a
/// third smaller, but a fixed stride is what lets the Python trainer
/// `np.memmap` the file with a structured dtype and hand slices straight to
/// the GPU, with no parsing loop in the hot path. Disk is cheaper than a
/// data-loading bottleneck: 3.6M positions is 460 MB at this stride.
///
/// Feature indices are written by `features.hpp` and are never recomputed
/// downstream. `feature_set_version` is checked by the loader so that data
/// generated under an older encoding cannot be silently trained on.

#include <cstdint>

#include "havoc/nnue/features.hpp"

namespace havoc::nnue {

inline constexpr uint32_t kDatasetFormatVersion = 1;

/// What produced `Record::score`.
enum class LabelKind : uint32_t {
    hce_static = 0,  ///< The handcrafted evaluation, for known-answer tests.
    search = 1,      ///< A datagen search score.
};

#pragma pack(push, 1)

struct FileHeader {
    char magic[4];  ///< "HVNN"
    uint32_t format_version;
    uint32_t feature_set_version;
    uint32_t input_dim;
    uint32_t max_active;
    uint32_t record_bytes;
    uint32_t label_kind;
    uint64_t count;
    char reserved[28];  ///< Zeroed. Keeps the header at 64 bytes.
};

struct Record {
    int16_t score;   ///< Centipawns, side-to-move point of view.
    uint8_t stm;     ///< 0 white, 1 black. Says which perspective is "us".
    uint8_t result;  ///< White's game result: 0 loss, 1 draw, 2 win, 255 unknown.
    uint8_t n;       ///< Active features; the same count for both perspectives.
    uint8_t pad[3];
    uint16_t feat_white[kMaxActiveFeatures];  ///< Padded with 0xFFFF.
    uint16_t feat_black[kMaxActiveFeatures];
};

#pragma pack(pop)

static_assert(sizeof(FileHeader) == 64, "header layout is part of the file format");
static_assert(sizeof(Record) == 128, "record stride is part of the file format");

inline constexpr uint16_t kNoFeature = 0xFFFF;

}  // namespace havoc::nnue
