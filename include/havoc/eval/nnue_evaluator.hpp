#pragma once

/// @file nnue_evaluator.hpp
/// @brief An IEvaluator that runs the quantised network with an incrementally
///        maintained feature transformer.
///
/// What is incremental and what is not
/// ----------------------------------
/// Only the first layer is incremental, because only the first layer is a
/// plain sum over active features: a move adds and removes a handful of them,
/// so the accumulator is patched rather than rebuilt. Everything after it is
/// dense and must be recomputed every time.
///
/// HalfKP makes one perspective's entire accumulator a function of that side's
/// king square, so when a king moves, that perspective is rebuilt from
/// scratch. The *other* perspective is untouched by it -- kings are not
/// features here, they are the bucket -- so it is still patched normally, and
/// a king capture still removes the captured piece from it. That asymmetry is
/// the part most likely to be got wrong, and it is what
/// `matches_full_recompute` exists to check.
///
/// State, and why it is per-thread
/// -------------------------------
/// Weights are immutable and shared. Accumulators are not: each search thread
/// walks its own line and needs its own stack of them, one frame per ply, so
/// that `pop` is a decrement rather than an inverse-move replay. The frames
/// are indices into one flat buffer so growing it cannot dangle a pointer.

#include <cstring>
#include <memory>
#include <string>
#include <vector>

#include "havoc/eval/evaluator.hpp"
#include "havoc/nnue/features.hpp"
#include "havoc/nnue/network.hpp"
#include "havoc/position.hpp"

namespace havoc {

class NNUEEvaluator : public IEvaluator {
  public:
    /// @param net Must be loaded and must outlive every search that uses it;
    ///            shared ownership is how that is guaranteed across a thread
    ///            count change.
    explicit NNUEEvaluator(std::shared_ptr<const nnue::Network> net)
        : net_(std::move(net)), l1_(net_->l1()) {
        frames_.assign(static_cast<size_t>(kInitialFrames) * frame_stride(), 0);
        capacity_ = kInitialFrames;
    }

    [[nodiscard]] int evaluate(const position& pos, int lazy_margin = -1) override {
        (void)lazy_margin;

        // A dead position is worth exactly nothing, and the network does not
        // know that. Asked about king and bishop against a bare king it
        // reports +252 -- it has learned that a bishop is worth a bishop, and
        // nothing in the training signal separates "ahead" from "ahead in a
        // position where no legal sequence of moves mates".
        //
        // The search cannot cover for it. `search()` tests `is_draw()` before
        // evaluating, but qsearch does not: it is entered on a child that was
        // never draw-checked and recurses through captures without checking
        // either, so a capture into a dead draw stands pat at the network's
        // number. That is the shape of the bug that matters -- the engine
        // trades into a drawn ending believing it is winning.
        //
        // HCEEvaluator has always opened with this test. The network path
        // simply never inherited it, because it is a bare forward pass.
        if (pos.is_material_draw())
            return score::kDraw;

        const int us = static_cast<int>(pos.to_move());
        const int16_t* base = frame(top_);
        return net_->forward(base + static_cast<size_t>(us) * static_cast<size_t>(l1_),
                             base + static_cast<size_t>(us ^ 1) * static_cast<size_t>(l1_));
    }

    [[nodiscard]] std::string name() const override { return "NNUE"; }

    [[nodiscard]] bool wants_deltas() const override { return true; }

    void push(const position& pos, const FeatureDelta& d) override;

    void pop() override {
        if (top_ > 0)
            --top_;
    }

    void refresh(const position& pos) override {
        top_ = 0;
        int16_t* base = frame(0);
        recompute(base, pos, white);
        recompute(base + static_cast<size_t>(l1_), pos, black);
    }

    /// Current stack depth. Should always equal the search's ply. Tests only.
    [[nodiscard]] int stack_depth() const { return top_; }

    /// Whether the live accumulator equals a from-scratch rebuild of `pos`.
    ///
    /// This is the only check that can tell "updated correctly" from
    /// "happened to be refreshed anyway", which is why it compares raw
    /// accumulators and not evaluations: two different accumulators can round
    /// to the same centipawn and a comparison of scores would pass.
    [[nodiscard]] bool matches_full_recompute(const position& pos) const;

  private:
    /// One frame per ply of the search. Grown, never assumed: a search that
    /// exceeds it would otherwise write past the end.
    static constexpr int kInitialFrames = 192;

    [[nodiscard]] size_t frame_stride() const { return static_cast<size_t>(2 * l1_); }

    [[nodiscard]] int16_t* frame(int i) {
        return frames_.data() + static_cast<size_t>(i) * frame_stride();
    }
    [[nodiscard]] const int16_t* frame(int i) const {
        return frames_.data() + static_cast<size_t>(i) * frame_stride();
    }

    /// Rebuild one perspective from the board. `dst` is `l1_` wide.
    void recompute(int16_t* dst, const position& pos, Color perspective) const {
        const int16_t* bias = net_->ft_bias();
        for (int i = 0; i < l1_; ++i)
            dst[i] = bias[i];
        nnue::for_each_active(pos, perspective, [&](int f) {
            const int16_t* row = net_->ft_row(f);
            for (int i = 0; i < l1_; ++i)
                dst[i] += row[i];
        });
    }

    std::shared_ptr<const nnue::Network> net_;
    int l1_ = 0;
    int top_ = 0;
    int capacity_ = 0;
    std::vector<int16_t> frames_;
};

inline void NNUEEvaluator::push(const position& pos, const FeatureDelta& d) {
    if (top_ + 1 >= capacity_) {
        capacity_ *= 2;
        frames_.resize(static_cast<size_t>(capacity_) * frame_stride(), 0);
    }
    const int16_t* prev = frame(top_);
    ++top_;
    int16_t* cur = frame(top_);

    // A king move rebuilds only its own perspective. Detect it before touching
    // anything, because the events for the two perspectives are the same list
    // read twice under different rules.
    bool king_moved[2] = {false, false};
    for (const auto& e : d)
        if (e.p == king)
            king_moved[static_cast<int>(e.c)] = true;

    for (int ci = 0; ci < 2; ++ci) {
        const Color perspective = static_cast<Color>(ci);
        int16_t* dst = cur + static_cast<size_t>(ci) * static_cast<size_t>(l1_);
        if (king_moved[ci]) {
            recompute(dst, pos, perspective);
            continue;
        }
        std::memcpy(dst, prev + static_cast<size_t>(ci) * static_cast<size_t>(l1_),
                    static_cast<size_t>(l1_) * sizeof(int16_t));

        // This perspective's king did not move, so its bucket is the same
        // before and after -- reading it from the post-move board is safe.
        const Square ksq = pos.king_square(perspective);
        for (const auto& e : d) {
            if (e.p == king)
                continue;
            const int16_t* row = net_->ft_row(nnue::index(perspective, ksq, e.c, e.p, e.sq));
            if (e.added)
                for (int i = 0; i < l1_; ++i)
                    dst[i] += row[i];
            else
                for (int i = 0; i < l1_; ++i)
                    dst[i] -= row[i];
        }
    }
}

inline bool NNUEEvaluator::matches_full_recompute(const position& pos) const {
    std::vector<int16_t> want(static_cast<size_t>(l1_));
    for (int ci = 0; ci < 2; ++ci) {
        recompute(want.data(), pos, static_cast<Color>(ci));
        const int16_t* have = frame(top_) + static_cast<size_t>(ci) * static_cast<size_t>(l1_);
        for (int i = 0; i < l1_; ++i)
            if (have[i] != want[i])
                return false;
    }
    return true;
}

/// Read a `.nnue` file. @return The network, or nullptr with `err` set.
[[nodiscard]] std::shared_ptr<const nnue::Network> load_network_file(const std::string& path,
                                                                    std::string& err);

} // namespace havoc
