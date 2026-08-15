#pragma once

/// @file evaluator.hpp
/// @brief Abstract evaluation interface.

#include "havoc/feature_delta.hpp"

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

    /// Whether this evaluator maintains state incrementally and therefore
    /// needs to be told about every move.
    ///
    /// This exists so that the hooks below cost nothing at all when they are
    /// not wanted. The search caches the answer once per thread and skips the
    /// calls entirely, which matters because an unconditional virtual call on
    /// every `do_move` would be an indirect branch the compiler cannot inline
    /// away, taken millions of times a second, in exchange for an empty body.
    ///
    /// The answer must not change over an evaluator's lifetime.
    [[nodiscard]] virtual bool wants_deltas() const { return false; }

    /// A move was made. `pos` is the position after it, `d` the features it
    /// changed. Called only when `wants_deltas()` is true, and paired with
    /// exactly one `pop()`.
    ///
    /// Null moves push an empty delta rather than being skipped, so that the
    /// evaluator's stack depth always matches the search's. An evaluator whose
    /// output depends on side to move must therefore not infer "nothing
    /// changed" from an empty delta.
    virtual void push(const position& pos, const FeatureDelta& d) {
        (void)pos;
        (void)d;
    }

    /// The move most recently pushed was taken back.
    virtual void pop() {}

    /// Discard all incremental state and rebuild it from `pos`.
    ///
    /// Needed whenever the board changes by something other than a move --
    /// setting up a new position, or replaying a game from UCI -- since there
    /// is no delta to follow across such a jump. The search calls this once per
    /// thread at the start of every search, unconditionally: unlike push/pop it
    /// is not per-node, so an evaluator that keeps state without wanting deltas
    /// still gets it.
    virtual void refresh(const position& pos) { (void)pos; }
};

} // namespace havoc
