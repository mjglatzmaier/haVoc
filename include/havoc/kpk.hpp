#pragma once

#include "havoc/types.hpp"

/// Exact solution of the king-and-pawn-versus-king ending.
///
/// KPK is small enough to solve outright -- 24 pawn squares by 64 by 64 king
/// squares by 2 sides to move -- so there is no reason for an evaluation to
/// guess at it. haVoc used to: it scored `7k/8/7K/7P/8/8/8/8 w`, a textbook
/// draw, at +118, and would trade into it believing it was a pawn up.
///
/// The table is built once at startup by value iteration over the whole game
/// graph, which takes well under a millisecond, so there is no data file to
/// ship or load.
namespace havoc::kpk {

/// Builds the bitbase. Idempotent, and must be called before `probe`.
void init();

/// True when the side with the pawn wins with best play.
///
/// The caller must normalise first: `strong_king`, `pawn` and `weak_king` are
/// squares as seen by a *white* pawn, and `pawn` must lie on files a-d of ranks
/// 2-7. `stm` is white when the side holding the pawn is to move.
[[nodiscard]] bool probe(int strong_king, int pawn, int weak_king, Color stm);

/// Number of positions the table classifies as won. Exposed for testing.
[[nodiscard]] unsigned won_count();

}  // namespace havoc::kpk
