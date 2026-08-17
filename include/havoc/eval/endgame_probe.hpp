#pragma once

/// @file endgame_probe.hpp
/// @brief Exactly-solved endings, shared by every evaluator.
///
/// The handcrafted evaluation has always routed king-and-pawn-versus-king
/// through the KPK bitbase. The network never did, because it is a bare
/// forward pass -- and it shows. Graded against Syzygy over 300 sampled KPvK
/// positions at depth 10, the handcrafted evaluation scores every theoretically
/// drawn one at 0 cp; the network averages 122 cp on them and puts 28% above
/// 150 cp. That is the same defect the bitbase was written to fix, reappearing
/// the moment a network is loaded.
///
/// This lives in its own header rather than inside either evaluator because the
/// square normalisation the bitbase requires is fiddly enough that two copies
/// of it would eventually disagree.

#include "havoc/position.hpp"

namespace havoc::eval {

/// True when `p` is king-and-pawn-versus-king, in which case `strong_wins` is
/// set to the bitbase's verdict for the side holding the pawn.
///
/// `kpk::init()` must have run. Callers that are not KPvK are told so and left
/// alone rather than given a meaningless verdict.
[[nodiscard]] bool probe_kpk(const position& p, bool& strong_wins);

}  // namespace havoc::eval
