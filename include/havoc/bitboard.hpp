#pragma once

#include "havoc/types.hpp"

#include <array>
#include <bit>

namespace havoc {

/// Bit manipulation utilities for bitboard operations.
namespace bits {

/// Population count (number of set bits).
[[nodiscard]] inline int count(U64 b) {
    return std::popcount(b);
}

/// Index of least significant set bit.
[[nodiscard]] inline int lsb(U64 b) {
    return std::countr_zero(b);
}

/// Pop and return the index of the least significant set bit.
inline int pop_lsb(U64& b) {
    int s = lsb(b);
    b &= b - 1;
    return s;
}

/// True if more than one bit is set.
[[nodiscard]] inline bool more_than_one(U64 b) {
    return b & (b - 1);
}

/// Print a bitboard as an 8x8 board to stdout.
void print(U64 b);

} // namespace bits

/// Pre-computed bitboard lookup tables.
namespace bitboards {

/// Single-square bitmasks.
extern U64 squares[64];
/// Rank bitmasks.
extern U64 row[8];
/// File bitmasks.
extern U64 col[8];
/// Knight attack masks.
extern U64 nmask[64];
/// King attack masks.
extern U64 kmask[64];
/// Pawn attack masks [color][square].
extern U64 pattks[2][64];
/// Squares between two aligned squares (inclusive).
extern U64 between[64][64];
/// Bishop full-ray attacks (unblocked).
extern U64 battks[64];
/// Rook full-ray attacks (unblocked).
extern U64 rattks[64];
/// Bishop occupancy masks (edges trimmed).
extern U64 bmask[64];
/// Rook occupancy masks (edges trimmed).
extern U64 rmask[64];
/// Edge squares mask.
extern U64 edges;
/// Corner squares mask.
extern U64 corners;
/// Light/dark square masks.
extern U64 colored_sqs[2];
/// Pawn push masks (excludes promotion rank).
extern U64 pawnmask[2];
/// Pawn left-capture masks.
extern U64 pawnmaskleft[2];
/// Pawn right-capture masks.
extern U64 pawnmaskright[2];
/// 4x4 big center mask.
extern U64 big_center_mask;
/// 3x2 small center mask.
extern U64 small_center_mask;
/// Adjacent file masks.
extern U64 neighbor_cols[8];
/// King flank masks for pawn shelter.
extern U64 kflanks[8];
/// Pawn storm detection masks [color][side].
/// King zone masks (extended king area for eval).
/// Check masks for each piece type from king square.
/// Passed pawn detection masks [color][square].
extern U64 passpawn_mask[2][64];
/// Pawn majority region masks (queen/center/king-side).
extern U64 pawn_majority_masks[3];
/// Squares in front of a given square [color][square].
extern U64 front_region[2][64];
/// Late move reduction table [pv][improving][depth][movecount].
extern unsigned reductions[2][2][64][64];

/// Initialize all lookup tables. Must be called once at startup.
void init();

} // namespace bitboards

} // namespace havoc
