#pragma once

/// The HalfKP feature set, defined once, in C++, for everyone.
///
/// This header is deliberately the *only* place where a board becomes a list
/// of network input indices. The trainer does not re-implement it in Python:
/// `tools/nnue_export.cpp` writes the indices produced here straight into the
/// training file, so the Python side never parses a FEN and never learns what
/// a feature means. A mismatch between the encoder that made the data and the
/// encoder that runs in the engine is the classic way an NNUE project loses a
/// week, and it is not expressible here.
///
/// If the feature set is ever changed, everything downstream must be
/// regenerated. Bump `kFeatureSetVersion` when that happens; the export file
/// carries it and the loader refuses a mismatch.

#include <cstdint>

#include "havoc/bitboard.hpp"
#include "havoc/position.hpp"
#include "havoc/types.hpp"

namespace havoc::nnue {

/// Bumped whenever the meaning of an index changes.
inline constexpr uint32_t kFeatureSetVersion = 1;

/// 10 piece kinds (pawn..queen, own/enemy) on 64 squares, per king square.
inline constexpr int kPieceKinds = 10;
inline constexpr int kFeaturesPerKing = kPieceKinds * 64;
inline constexpr int kInputDim = 64 * kFeaturesPerKing;  // 40960

/// Kings are not features in HalfKP; they are the bucket. At most 30 of the
/// 32 men are therefore encoded, which fixes the record size in the export.
inline constexpr int kMaxActiveFeatures = 30;

/// Squares are named from the point of view of the perspective being encoded,
/// so that a position and its colour-swapped mirror produce the same indices.
[[nodiscard]] inline constexpr Square orient(Color perspective, Square s) {
    return perspective == white ? s : static_cast<Square>(static_cast<int>(s) ^ 56);
}

/// @param perspective Whose accumulator this index belongs to.
/// @param king_sq     That side's king square, unoriented.
/// @param pc          Colour of the piece being encoded, unoriented.
/// @param pt          Type of the piece being encoded; must not be `king`.
/// @param sq          Square of the piece, unoriented.
[[nodiscard]] inline constexpr int index(Color perspective, Square king_sq, Color pc, Piece pt,
                                         Square sq) {
    const int oriented_king = static_cast<int>(orient(perspective, king_sq));
    const int oriented_sq = static_cast<int>(orient(perspective, sq));
    const int kind = static_cast<int>(pt) * 2 + (pc == perspective ? 0 : 1);
    return oriented_king * kFeaturesPerKing + kind * 64 + oriented_sq;
}

/// Write the active features for one perspective into `out`.
/// @return How many were written; never more than `kMaxActiveFeatures`.
template <typename Emit>
inline int for_each_active(const position& pos, Color perspective, Emit&& emit) {
    const Square king_sq = pos.king_square(perspective);
    const U64 white_men = pos.get_pieces<white>();
    int n = 0;
    for (int s = 0; s < 64; ++s) {
        const Square sq = static_cast<Square>(s);
        const Piece p = pos.piece_on(sq);
        if (p == no_piece || p == king)
            continue;
        if (n >= kMaxActiveFeatures)
            return n;
        const Color pc = (white_men & bitboards::squares[s]) ? white : black;
        emit(index(perspective, king_sq, pc, p, sq));
        ++n;
    }
    return n;
}

}  // namespace havoc::nnue
