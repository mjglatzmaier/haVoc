#pragma once

#include "havoc/types.hpp"

namespace havoc::zobrist {

/// Initialize zobrist hash tables. Must be called once at startup.
void init();

/// Zobrist key for a piece on a square.
[[nodiscard]] U64 piece(Square sq, Color c, Piece p);

/// Zobrist key for castling rights.
[[nodiscard]] U64 castle(Color c, U16 rights);

/// Key contribution of a complete castling-rights mask.
///
/// The rights have to be hashed as one value rather than per-right, because
/// the only place that knows which individual rights went away is the code
/// that clears them, and it clears with masks rather than with the right bits
/// that were originally hashed in. Hashing the whole mask makes the term a
/// function of the rights themselves, so XORing the old mask out and the new
/// mask in is exact no matter how the change came about.
[[nodiscard]] U64 castle_rights(U16 mask);

/// Zobrist key for en-passant file.
[[nodiscard]] U64 ep(int file);

/// Zobrist key for side to move.
[[nodiscard]] U64 stm(Color c);

/// Zobrist key for 50-move rule counter.


} // namespace havoc::zobrist
