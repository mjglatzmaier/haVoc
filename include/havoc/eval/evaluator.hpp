#pragma once

/// @file evaluator.hpp
/// @brief Abstract evaluation interface.

#include <string>

namespace havoc {

class position;

/// Abstract evaluation interface. Implementations: HCE, NNUE (future).
class IEvaluator {
  public:
    virtual ~IEvaluator() = default;

    /// Evaluate the position from the side-to-move perspective.
    /// @param pos The position to evaluate.
    /// @param lazy_margin Vestigial and ignored. The evaluation is always
    ///        exact; see the note in HCEEvaluator::evaluate for why the lazy
    ///        cutoff that used to read this was removed.
    /// @return Score in centipawns (positive = good for side to move).
    [[nodiscard]] virtual int evaluate(const position& pos, int lazy_margin = -1) = 0;

    /// Human-readable name of this evaluator.
    [[nodiscard]] virtual std::string name() const = 0;
};

} // namespace havoc
