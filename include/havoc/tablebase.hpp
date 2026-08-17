#pragma once

/// @file tablebase.hpp
/// @brief Syzygy tablebase probing.
///
/// Backed by the vendored Fathom library when built with HAVOC_SYZYGY (the
/// default); every entry point degrades to "unavailable" when it is not, so
/// callers never need to know which build they are in.
///
/// There is deliberately no DTZ entry point. The previous version of this
/// header declared one, it was never implemented, and nothing called it.
/// Using DTZ correctly means filtering the root move list -- probing it and
/// playing the suggested move is unsound, because DTZ moves optimise distance
/// to a zeroing move and can look absurd. Until that filtering exists, an
/// unimplemented declaration is worse than no declaration.

#include "havoc/position.hpp"

#include <string>

namespace havoc::tablebase {

/// Returned by probe_wdl when the position is not in the tables, or no tables
/// are loaded. Distinct from 0, which is a genuine drawn verdict.
inline constexpr int kProbeFailed = -2;

/// Initialize tablebase access, replacing any previously loaded set.
/// @param path Directory containing Syzygy tablebase files. An empty path
///             unloads the tables and succeeds.
/// @return true if the path was accepted and at least one table was found.
bool init(const std::string& path);

/// Release the tables. Safe to call when none are loaded.
void shutdown();

/// Probe Win/Draw/Loss from the perspective of the side to move.
///
/// Positions with castling rights or a non-zero fifty-move counter always
/// fail: the tables do not model either. A cursed win (one the fifty-move
/// rule takes away) is reported as a draw, because that is what it is worth
/// in a game this engine plays.
///
/// @return +1 (win), 0 (draw), -1 (loss), or kProbeFailed.
[[nodiscard]] int probe_wdl(const position& pos);

/// @return true if tablebase files are loaded and available.
[[nodiscard]] bool available();

/// @return the largest number of pieces the loaded tables cover, 0 if none.
[[nodiscard]] int max_pieces();

} // namespace havoc::tablebase
