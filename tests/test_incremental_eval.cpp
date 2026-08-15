/// @file test_incremental_eval.cpp
/// @brief Proves the incremental-evaluation seam end to end.
///
/// Step 4 of `docs/nnue-integration.md`. The mechanism an NNUE accumulator will
/// ride on -- `FeatureDelta`, `IEvaluator::push`/`pop`/`refresh`, and the
/// search calling them around every move -- is exercised here with an
/// evaluation whose correct answer is independently known: material.
///
/// Doing it with material rather than a network is the whole point. Material
/// can be computed from scratch at any moment, so every incremental step has an
/// oracle, and a desynchronisation is caught at the node it happens on rather
/// than surfacing much later as an evaluation that is wrong by an
/// uncharacterisable amount. A bug found here is a bug that would otherwise be
/// found by staring at a network that plays slightly badly.

#include "havoc/bitboard.hpp"
#include "havoc/eval/evaluator.hpp"
#include "havoc/kpk.hpp"
#include "havoc/magics.hpp"
#include "havoc/position.hpp"
#include "havoc/search.hpp"
#include "havoc/zobrist.hpp"

#include <memory>
#include <sstream>
#include <string>
#include <vector>

#include <gtest/gtest.h>

namespace havoc {
namespace {

int piece_value(Piece p) {
    switch (p) {
    case pawn:
        return 100;
    case knight:
        return 300;
    case bishop:
        return 315;
    case rook:
        return 480;
    case queen:
        return 910;
    default:
        return 0; // kings never leave the board, so their value cannot matter
    }
}

/// Material from White's point of view, computed from the board with no
/// incremental state involved at all. This is the oracle.
int material_from_scratch(const position& p) {
    int total = 0;
    for (int s = 0; s < squares; ++s) {
        const Piece pc = p.piece_on(Square(s));
        if (pc == no_piece)
            continue;
        const int v = piece_value(pc);
        total += p.color_on(Square(s)) == white ? v : -v;
    }
    return total;
}

/// Maintains the same number incrementally, through the seam, and checks it
/// against the oracle every time anyone can observe it.
class CheckedIncrementalMaterial : public IEvaluator {
  public:
    [[nodiscard]] std::string name() const override { return "checked-incremental-material"; }

    [[nodiscard]] bool wants_deltas() const override { return true; }

    void refresh(const position& pos) override {
        acc_ = material_from_scratch(pos);
        stack_.clear();
        ++refreshes_;
    }

    void push(const position& pos, const FeatureDelta& d) override {
        stack_.push_back(acc_);
        for (const auto& e : d) {
            const int v = e.c == white ? piece_value(e.p) : -piece_value(e.p);
            acc_ += e.added ? v : -v;
        }
        ++pushes_;
        verify(pos, "after push");
    }

    void pop() override {
        if (stack_.empty()) {
            ++unbalanced_pops_;
            return;
        }
        acc_ = stack_.back();
        stack_.pop_back();
        ++pops_;
    }

    int evaluate(const position& pos, int = -1) override {
        verify(pos, "at evaluate");
        // Sign convention: the interface is side-to-move relative.
        return pos.to_move() == white ? acc_ : -acc_;
    }

    [[nodiscard]] long pushes() const { return pushes_; }
    [[nodiscard]] long pops() const { return pops_; }
    [[nodiscard]] long refreshes() const { return refreshes_; }
    [[nodiscard]] long checks() const { return checks_; }
    [[nodiscard]] long unbalanced_pops() const { return unbalanced_pops_; }
    [[nodiscard]] const std::string& first_mismatch() const { return first_mismatch_; }

  private:
    void verify(const position& pos, const char* when) {
        ++checks_;
        const int truth = material_from_scratch(pos);
        if (acc_ != truth && first_mismatch_.empty())
            first_mismatch_ = std::string(when) + ": incremental material is " +
                              std::to_string(acc_) + " but the board says " +
                              std::to_string(truth);
    }

    int acc_ = 0;
    std::vector<int> stack_;
    long pushes_ = 0, pops_ = 0, refreshes_ = 0, checks_ = 0, unbalanced_pops_ = 0;
    std::string first_mismatch_;
};

class IncrementalEvalTest : public ::testing::Test {
  protected:
    void SetUp() override {
        bitboards::init();
        magics::init();
        zobrist::init();
        kpk::init();
    }
};

} // namespace

// The search prunes, reduces, re-searches, makes null moves and drops into
// quiescence, and every one of those paths makes and unmakes moves. A hand
// walk over legal moves would miss the asymmetric ones. So this runs the real
// search and lets it drive the seam.
TEST_F(IncrementalEvalTest, SearchKeepsAnIncrementalEvaluatorInSyncWithTheBoard) {
    const std::vector<std::string> fens{
        "rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1",
        // Kiwipete: castling both ways, many captures.
        "r3k2r/p1ppqpb1/bn2pnp1/3PN3/1p2P3/2N2Q1p/PPPBBPPP/R3K2R w KQkq - 0 1",
        // Promotions and promotion-captures.
        "n1n5/PPPk4/8/8/8/8/4Kppp/5N1N b - - 0 1",
        // An en passant is available immediately.
        "rnbqkbnr/ppp1p1pp/8/3pPp2/8/8/PPPP1PPP/RNBQKBNR w KQkq f6 0 3",
    };

    for (const auto& fen : fens) {
        SearchEngine engine;
        engine.set_threads(1);

        CheckedIncrementalMaterial* probe = nullptr;
        engine.set_evaluator_factory([&probe](Searchthread&) {
            auto e = std::make_unique<CheckedIncrementalMaterial>();
            probe = e.get();
            return e;
        });

        std::istringstream ss(fen);
        position p(ss);

        SearchLimits lims;
        lims.depth = 7;
        engine.start(p, lims, /*silent=*/true);
        engine.wait();

        ASSERT_NE(probe, nullptr) << "the factory was never used for " << fen;
        EXPECT_TRUE(probe->first_mismatch().empty()) << "in " << fen << "\n"
                                                     << probe->first_mismatch();
        EXPECT_EQ(probe->unbalanced_pops(), 0)
            << "in " << fen << ": the search popped more times than it pushed";
        EXPECT_GT(probe->pushes(), 1000) << "in " << fen << ": the search barely ran";
        EXPECT_EQ(probe->pushes(), probe->pops())
            << "in " << fen << ": every move made must be taken back exactly once";
        EXPECT_GT(probe->refreshes(), 0) << "in " << fen << ": the root was never refreshed";
    }
}

// The oracle above only proves agreement if disagreement is detectable, and a
// seam that is never invoked agrees trivially. This checks the probe's own
// alarm works by desynchronising it deliberately.
TEST_F(IncrementalEvalTest, TheSyncCheckActuallyDetectsDesynchronisation) {
    class BrokenIncrementalMaterial : public CheckedIncrementalMaterial {
      public:
        // Ignores the delta entirely: the accumulator stays at the root value
        // while the board moves on beneath it.
        void push(const position& pos, const FeatureDelta&) override {
            CheckedIncrementalMaterial::push(pos, FeatureDelta{});
        }
    };

    SearchEngine engine;
    engine.set_threads(1);

    BrokenIncrementalMaterial* probe = nullptr;
    engine.set_evaluator_factory([&probe](Searchthread&) {
        auto e = std::make_unique<BrokenIncrementalMaterial>();
        probe = e.get();
        return e;
    });

    std::istringstream ss("r3k2r/p1ppqpb1/bn2pnp1/3PN3/1p2P3/2N2Q1p/PPPBBPPP/R3K2R w KQkq - 0 1");
    position p(ss);

    SearchLimits lims;
    lims.depth = 5;
    engine.start(p, lims, /*silent=*/true);
    engine.wait();

    ASSERT_NE(probe, nullptr);
    EXPECT_FALSE(probe->first_mismatch().empty())
        << "an evaluator that ignores the delta went undetected, so the check above proves nothing";
}

// Installing an evaluator must not be a one-way door, and it must survive the
// pool being torn down and rebuilt -- which is what a Threads change does.
TEST_F(IncrementalEvalTest, EvaluatorFactorySurvivesThreadCountChangeAndCanBeCleared) {
    SearchEngine engine;
    engine.set_threads(1);
    engine.set_evaluator_factory(
        [](Searchthread&) { return std::make_unique<CheckedIncrementalMaterial>(); });

    engine.set_threads(2);
    for (unsigned i = 0; i < engine.threads(); ++i)
        EXPECT_EQ(engine.evaluator_name(i), "checked-incremental-material")
            << "thread " << i << " lost the installed evaluator when Threads changed";

    engine.set_evaluator_factory(nullptr);
    for (unsigned i = 0; i < engine.threads(); ++i)
        EXPECT_NE(engine.evaluator_name(i), "checked-incremental-material")
            << "thread " << i << " kept the installed evaluator after it was cleared";
}

} // namespace havoc
