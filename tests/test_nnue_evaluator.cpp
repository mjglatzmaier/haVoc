/// @file test_nnue_evaluator.cpp
/// @brief Stage 1b: prove the incremental accumulator equals a full rebuild.
///
/// The oracle here is not a second implementation of anything: it is the same
/// `recompute` the evaluator itself uses at a refresh, run against the board
/// as it actually is. So a mismatch means the *incremental path* is wrong,
/// which is the only thing that can be wrong once the from-scratch path is
/// pinned by `test_nnue_features.cpp`.
///
/// Why raw accumulators and not evaluations
/// ---------------------------------------
/// Two different accumulators routinely round to the same centipawn, so a
/// test that compared scores would pass with a desynchronised first layer and
/// only fail once the error grew large enough to change the truncated output.
/// `matches_full_recompute` compares every one of the 2*l1 integers.
///
/// Why king moves get their own tests
/// ---------------------------------
/// In HalfKP a side's king square is the bucket for that side's entire
/// accumulator, so a king move invalidates one perspective completely and
/// leaves the other one needing an ordinary incremental update. A generic
/// sync check cannot tell "rebuilt correctly" from "updated correctly",
/// because rebuilding is always right; what it cannot catch on its own is the
/// converse -- the perspective that should have been *updated* being rebuilt,
/// or the perspective that should have been *rebuilt* being updated. The
/// mutation tests below break each of those deliberately and require the
/// check to notice.

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <mutex>
#include <sstream>
#include <string>
#include <vector>

#include "havoc/bitboard.hpp"
#include "havoc/eval/nnue_evaluator.hpp"
#include "havoc/kpk.hpp"
#include "havoc/magics.hpp"
#include "havoc/movegen.hpp"
#include "havoc/position.hpp"
#include "havoc/search.hpp"
#include "havoc/utils.hpp"
#include "havoc/zobrist.hpp"

#include <gtest/gtest.h>

namespace havoc {
namespace {

// Chosen so the vectorised kernel's width conditions are met: the search-
// driven tests below are the only place it runs against real board states, and
// dimensions it rejects would silently exercise the reference instead.
constexpr int kL1 = 16, kL2 = 16, kL3 = 4;

/// A network whose weights are arbitrary but distinct.
///
/// Distinctness is the requirement, not realism. Every feature row must differ
/// from every other, or an accumulator that added the wrong row would still
/// hold the right numbers and the whole file would prove nothing.
std::shared_ptr<const nnue::Network> make_test_network() {
    nnue::NetworkHeader h{};
    std::memcpy(h.magic, "HVNW", 4);
    h.format_version = nnue::kNetworkFormatVersion;
    h.feature_set_version = nnue::kFeatureSetVersion;
    h.input_dim = nnue::kInputDim;
    h.l1 = kL1;
    h.l2 = kL2;
    h.l3 = kL3;
    h.cp_scale = nnue::kDefaultCpScale;
    h.qa = nnue::kQA;
    h.s_fc1 = 64;
    h.s_fc2 = 64;
    h.s_out = 64;

    std::string s(reinterpret_cast<const char*>(&h), sizeof(h));
    auto append = [&s](const auto& v) {
        s.append(reinterpret_cast<const char*>(v.data()),
                 v.size() * sizeof(typename std::decay_t<decltype(v)>::value_type));
    };

    // A cheap full-period-ish mixer; the values only have to be varied and
    // reproducible, and a std::mt19937 would make the file depend on the
    // library's generator.
    uint32_t state = 0x2545f491u;
    auto next = [&state]() {
        state ^= state << 13;
        state ^= state >> 17;
        state ^= state << 5;
        return state;
    };

    std::vector<int16_t> ft_w(static_cast<size_t>(nnue::kInputDim) * kL1);
    for (auto& w : ft_w)
        w = static_cast<int16_t>(static_cast<int32_t>(next() % 63u) - 31);
    std::vector<int16_t> ft_b(kL1);
    for (auto& b : ft_b)
        b = static_cast<int16_t>(static_cast<int32_t>(next() % 41u) - 20);
    std::vector<int8_t> fc1_w(kL2 * 2 * kL1);
    for (auto& w : fc1_w)
        w = static_cast<int8_t>(static_cast<int32_t>(next() % 127u) - 63);
    std::vector<int32_t> fc1_b(kL2, 0);
    std::vector<int8_t> fc2_w(kL3 * kL2);
    for (auto& w : fc2_w)
        w = static_cast<int8_t>(static_cast<int32_t>(next() % 127u) - 63);
    std::vector<int32_t> fc2_b(kL3, 0);
    std::vector<int8_t> out_w(kL3);
    for (auto& w : out_w)
        w = static_cast<int8_t>(static_cast<int32_t>(next() % 127u) - 63);
    const int32_t out_b = 0;

    append(ft_w);
    append(ft_b);
    append(fc1_w);
    append(fc1_b);
    append(fc2_w);
    append(fc2_b);
    append(out_w);
    s.append(reinterpret_cast<const char*>(&out_b), sizeof(out_b));

    auto net = std::make_shared<nnue::Network>();
    std::istringstream in(s, std::ios::binary);
    const std::string err = net->load(in);
    EXPECT_EQ(err, "") << "the test network is malformed";
    return net;
}

/// Checks the accumulator against a full rebuild everywhere it is observable.
class CheckedNNUE : public NNUEEvaluator {
  public:
    using NNUEEvaluator::NNUEEvaluator;

    void push(const position& pos, const FeatureDelta& d) override {
        NNUEEvaluator::push(pos, d);
        ++pushes_;
        verify(pos, "after push");
    }

    void pop() override {
        NNUEEvaluator::pop();
        ++pops_;
    }

    void refresh(const position& pos) override {
        NNUEEvaluator::refresh(pos);
        ++refreshes_;
        verify(pos, "after refresh");
    }

    int evaluate(const position& pos, int lazy = -1) override {
        verify(pos, "at evaluate");
        return NNUEEvaluator::evaluate(pos, lazy);
    }

    [[nodiscard]] long pushes() const { return pushes_; }
    [[nodiscard]] long pops() const { return pops_; }
    [[nodiscard]] long refreshes() const { return refreshes_; }
    [[nodiscard]] const std::string& first_mismatch() const { return first_mismatch_; }

  protected:
    void verify(const position& pos, const char* when) {
        if (!first_mismatch_.empty())
            return;
        if (!matches_full_recompute(pos))
            first_mismatch_ = std::string(when) +
                              ": the incremental accumulator differs from a full rebuild, fen " +
                              pos.to_fen();
    }

  private:
    long pushes_ = 0, pops_ = 0, refreshes_ = 0;
    std::string first_mismatch_;
};

/// Re-record one event through the public API, so the mutation tests below
/// build their broken deltas out of the same primitives move-making uses.
void append(FeatureDelta& d, const FeatureDelta::Event& e) {
    if (e.added)
        d.add(e.c, e.p, e.sq);
    else
        d.remove(e.c, e.p, e.sq);
}

class NnueEvaluatorTest : public ::testing::Test {
  protected:
    void SetUp() override {
        bitboards::init();
        magics::init();
        zobrist::init();
        kpk::init();
        net_ = make_test_network();
    }
    std::shared_ptr<const nnue::Network> net_;
};

const std::vector<std::string>& sync_fens() {
    static const std::vector<std::string> fens{
        "rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1",
        // Kiwipete: castling both ways, many captures, both kings can move.
        "r3k2r/p1ppqpb1/bn2pnp1/3PN3/1p2P3/2N2Q1p/PPPBBPPP/R3K2R w KQkq - 0 1",
        // Promotions and promotion-captures.
        "n1n5/PPPk4/8/8/8/8/4Kppp/5N1N b - - 0 1",
        // En passant available immediately.
        "rnbqkbnr/ppp1p1pp/8/3pPp2/8/8/PPPP1PPP/RNBQKBNR w KQkq f6 0 3",
        // Bare kings and a queen: almost every move is a king move, so the
        // bucket-crossing path dominates instead of being a rare event.
        "8/2k5/8/8/8/5Q2/8/4K3 w - - 0 1",
        // A king capture is available at once: the moving side's perspective
        // is rebuilt while the other's must still lose the captured pawn.
        "8/8/8/3k4/3P4/8/8/4K3 b - - 0 1",
    };
    return fens;
}

} // namespace

// The search prunes, reduces, re-searches, makes null moves and drops into
// quiescence. Driving the seam with the real search is the only way to reach
// all of those paths, and it is where a bucket-crossing king move happens
// thousands of times.
TEST_F(NnueEvaluatorTest, AccumulatorSurvivesARealSearchOnEveryLine) {
    // A per-position floor of 1000 pushes was tried and is the wrong guard: a
    // king-and-pawn ending at depth 6 has a genuinely small tree, and the
    // exact size moves whenever the evaluation does -- lowering kQA changed it
    // from just over the line to just under, with nothing actually wrong. The
    // guard has to rule out a *vacuous* test, so it asks for a small floor
    // everywhere and a large one somewhere.
    long deepest = 0;
    for (const auto& fen : sync_fens()) {
        SearchEngine engine;
        engine.set_threads(1);

        CheckedNNUE* probe = nullptr;
        engine.set_evaluator_factory([&](Searchthread&) {
            auto e = std::make_unique<CheckedNNUE>(net_);
            probe = e.get();
            return e;
        });

        std::istringstream ss(fen);
        position p(ss);

        SearchLimits lims;
        lims.depth = 6;
        engine.start(p, lims, /*silent=*/true);
        engine.wait();

        ASSERT_NE(probe, nullptr) << "the factory was never used for " << fen;
        EXPECT_TRUE(probe->first_mismatch().empty()) << "in " << fen << "\n"
                                                     << probe->first_mismatch();
        EXPECT_GT(probe->pushes(), 100) << "in " << fen << ": the search barely ran";
        EXPECT_EQ(probe->pushes(), probe->pops()) << "in " << fen;
        EXPECT_GT(probe->refreshes(), 0) << "in " << fen;
        deepest = std::max(deepest, probe->pushes());
    }
    EXPECT_GT(deepest, 1000) << "no position searched a tree big enough for this to mean anything";
}

// Every king move in a set of positions, walked by hand, so the bucket-
// crossing case is exercised exhaustively rather than incidentally -- and so
// the count is asserted, since a search could in principle prune them all.
TEST_F(NnueEvaluatorTest, EveryKingMoveIsCheckedAgainstAFullRebuild) {
    int king_moves = 0, king_captures = 0, castles = 0;

    for (const auto& fen : sync_fens()) {
        std::istringstream ss(fen);
        position p(ss);

        CheckedNNUE ev(net_);
        ev.refresh(p);
        ASSERT_TRUE(ev.first_mismatch().empty()) << fen << "\n" << ev.first_mismatch();

        Movegen mvs(p);
        mvs.generate<pseudo_legal, pieces>();
        for (int i = 0; i < mvs.size(); ++i) {
            const Move m = mvs[i];
            if (p.piece_on(static_cast<Square>(m.f)) != king)
                continue;
            if (!p.is_legal(m))
                continue;
            const bool capture = p.piece_on(static_cast<Square>(m.t)) != no_piece;
            const int file_jump = std::abs(static_cast<int>(util::col(m.t)) -
                                           static_cast<int>(util::col(m.f)));

            p.do_move(m);
            ev.push(p, p.delta());
            ++king_moves;
            king_captures += capture ? 1 : 0;
            castles += file_jump > 1 ? 1 : 0;

            // And one reply, so the *other* perspective is updated across a
            // frame in which its own king did not move.
            Movegen replies(p);
            replies.generate<pseudo_legal, pieces>();
            for (int j = 0; j < replies.size(); ++j) {
                if (!p.is_legal(replies[j]))
                    continue;
                p.do_move(replies[j]);
                ev.push(p, p.delta());
                ev.pop();
                p.undo_move(replies[j]);
                break;
            }

            ev.pop();
            p.undo_move(m);
        }
        EXPECT_TRUE(ev.first_mismatch().empty()) << fen << "\n" << ev.first_mismatch();
    }

    EXPECT_GT(king_moves, 20) << "the positions chosen do not exercise king moves";
    EXPECT_GT(king_captures, 0) << "no king capture was tested: the enemy perspective must be "
                                   "updated across a move that also rebuilds ours";
    EXPECT_GT(castles, 0) << "no castling was tested: it is the only four-event delta";
}

// Rebuilding is always correct, so the interesting failure is the perspective
// that should have been rebuilt being incrementally updated instead. In HalfKP
// that means every one of its 30-odd feature indices is now computed against
// the wrong king bucket. If the check could not see this, it would be
// certifying nothing about the case it exists for.
TEST_F(NnueEvaluatorTest, TheCheckDetectsAKingMoveThatWasNotRebuilt) {
    class NeverRebuilds : public CheckedNNUE {
      public:
        using CheckedNNUE::CheckedNNUE;
        void push(const position& pos, const FeatureDelta& d) override {
            // Strip the king events, which is exactly what an implementation
            // that forgot the bucket would effectively do: the accumulator
            // carries on from the previous king square.
            FeatureDelta stripped;
            for (const auto& e : d)
                if (e.p != king)
                    append(stripped, e);
            CheckedNNUE::push(pos, stripped);
        }
    };

    std::istringstream ss("8/2k5/8/8/8/5Q2/8/4K3 w - - 0 1");
    position p(ss);
    NeverRebuilds ev(net_);
    ev.refresh(p);

    Movegen mvs(p);
    mvs.generate<pseudo_legal, pieces>();
    bool tried = false;
    for (int i = 0; i < mvs.size(); ++i) {
        if (p.piece_on(static_cast<Square>(mvs[i].f)) != king || !p.is_legal(mvs[i]))
            continue;
        p.do_move(mvs[i]);
        ev.push(p, p.delta());
        ev.pop();
        p.undo_move(mvs[i]);
        tried = true;
    }
    ASSERT_TRUE(tried) << "no king move was available to break";
    EXPECT_FALSE(ev.first_mismatch().empty())
        << "a king move that skipped the rebuild went undetected, so the sync check above "
           "proves nothing about bucket crossing";
}

// The converse mutation: the non-moving side's perspective silently rebuilt
// rather than updated. That is *not* wrong -- it is merely slow -- so this
// instead breaks the update itself, dropping the events the other perspective
// still needs across a king move. A king capture is the case that exposes it.
TEST_F(NnueEvaluatorTest, TheCheckDetectsALostUpdateOnTheOtherPerspective) {
    class DropsCaptures : public CheckedNNUE {
      public:
        using CheckedNNUE::CheckedNNUE;
        void push(const position& pos, const FeatureDelta& d) override {
            FeatureDelta kept;
            for (const auto& e : d)
                if (e.added || e.p == king)
                    append(kept, e);
            CheckedNNUE::push(pos, kept);
        }
    };

    std::istringstream ss("r3k2r/p1ppqpb1/bn2pnp1/3PN3/1p2P3/2N2Q1p/PPPBBPPP/R3K2R w KQkq - 0 1");
    position p(ss);
    DropsCaptures ev(net_);
    ev.refresh(p);

    Movegen mvs(p);
    mvs.generate<pseudo_legal, pieces>();
    for (int i = 0; i < mvs.size(); ++i) {
        if (!p.is_legal(mvs[i]))
            continue;
        p.do_move(mvs[i]);
        ev.push(p, p.delta());
        ev.pop();
        p.undo_move(mvs[i]);
    }
    EXPECT_FALSE(ev.first_mismatch().empty())
        << "an evaluator that never removed a feature went undetected";
}

// Pop must restore the previous frame exactly, not approximately: the search
// makes and unmakes millions of moves along one line, and an error that
// survived a pop would accumulate rather than cancel.
TEST_F(NnueEvaluatorTest, PopRestoresThePreviousFrameExactly) {
    std::istringstream ss("r3k2r/p1ppqpb1/bn2pnp1/3PN3/1p2P3/2N2Q1p/PPPBBPPP/R3K2R w KQkq - 0 1");
    position p(ss);
    NNUEEvaluator ev(net_);
    ev.refresh(p);
    const int before = ev.evaluate(p);

    Movegen mvs(p);
    mvs.generate<pseudo_legal, pieces>();
    int played = 0;
    for (int i = 0; i < mvs.size(); ++i) {
        if (!p.is_legal(mvs[i]))
            continue;
        p.do_move(mvs[i]);
        ev.push(p, p.delta());
        ev.pop();
        p.undo_move(mvs[i]);
        ++played;
        ASSERT_EQ(ev.evaluate(p), before) << "pop did not restore the frame after move " << i;
    }
    EXPECT_GT(played, 20);
    EXPECT_EQ(ev.stack_depth(), 0);
}

// A null move pushes an empty delta so that the evaluator's stack depth
// tracks the search's. The accumulators must be unchanged, but the evaluation
// must not be: side to move has flipped, and it is the ordering of the two
// accumulators that carries it.
TEST_F(NnueEvaluatorTest, ANullMoveKeepsTheAccumulatorAndFlipsThePerspective) {
    std::istringstream ss("r3k2r/p1ppqpb1/bn2pnp1/3PN3/1p2P3/2N2Q1p/PPPBBPPP/R3K2R w KQkq - 0 1");
    position p(ss);
    CheckedNNUE ev(net_);
    ev.refresh(p);
    const int white_view = ev.evaluate(p);

    p.do_null_move();
    ev.push(p, p.delta());
    const int black_view = ev.evaluate(p);
    EXPECT_TRUE(ev.first_mismatch().empty()) << ev.first_mismatch();
    EXPECT_EQ(ev.stack_depth(), 1);
    EXPECT_NE(white_view, black_view)
        << "a null move left the evaluation unchanged, so the accumulators are either identical "
           "or their order is not being read";

    ev.pop();
    p.undo_null_move();
    EXPECT_EQ(ev.evaluate(p), white_view);
}

// Refresh is what the search calls at every root, and between two unrelated
// positions there is no delta to follow. It must discard the whole stack, not
// just overwrite the top of it.
TEST_F(NnueEvaluatorTest, RefreshDiscardsTheWholeStack) {
    std::istringstream ss("rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1");
    position p(ss);
    NNUEEvaluator ev(net_);
    ev.refresh(p);

    Movegen mvs(p);
    mvs.generate<pseudo_legal, pieces>();
    for (int i = 0; i < mvs.size() && i < 3; ++i) {
        if (!p.is_legal(mvs[i]))
            continue;
        p.do_move(mvs[i]);
        ev.push(p, p.delta());
    }
    ASSERT_GT(ev.stack_depth(), 0);

    std::istringstream ss2("8/2k5/8/8/8/5Q2/8/4K3 w - - 0 1");
    position other(ss2);
    ev.refresh(other);
    EXPECT_EQ(ev.stack_depth(), 0);
    EXPECT_TRUE(ev.matches_full_recompute(other));
}

// Every search thread walks its own line, so accumulators cannot be shared.
// One shared const network across all of them must be, and the thread_local
// scratch inside `forward` is the part of that claim which is easy to get
// wrong.
TEST_F(NnueEvaluatorTest, SeveralThreadsShareOneNetworkSafely) {
    SearchEngine engine;
    engine.set_threads(4);

    std::vector<CheckedNNUE*> probes;
    std::mutex probes_mutex;
    engine.set_evaluator_factory([&](Searchthread&) {
        auto e = std::make_unique<CheckedNNUE>(net_);
        {
            std::lock_guard<std::mutex> lock(probes_mutex);
            probes.push_back(e.get());
        }
        return e;
    });

    std::istringstream ss("r3k2r/p1ppqpb1/bn2pnp1/3PN3/1p2P3/2N2Q1p/PPPBBPPP/R3K2R w KQkq - 0 1");
    position p(ss);
    SearchLimits lims;
    lims.depth = 7;
    engine.start(p, lims, /*silent=*/true);
    engine.wait();

    ASSERT_GE(probes.size(), 4u);
    long total_pushes = 0;
    for (auto* probe : probes) {
        EXPECT_TRUE(probe->first_mismatch().empty()) << probe->first_mismatch();
        total_pushes += probe->pushes();
    }
    EXPECT_GT(total_pushes, 1000);
    EXPECT_EQ(engine.evaluator_name(0), "NNUE");
}


// A network has no way to learn that a bishop is worthless when no legal
// sequence of moves can mate. Asked about king and bishop against a bare king
// the trained net reported +252, and the search cannot cover for it: search()
// tests is_draw() before evaluating, but qsearch does not, so a capture into a
// dead draw stands pat at whatever the network says. The engine then trades
// into a drawn ending believing it is winning.
//
// HCEEvaluator has opened with this test since it was written. The network
// path is a bare forward pass and never inherited it.
TEST_F(NnueEvaluatorTest, DeadMaterialIsWorthNothingWhateverTheNetworkThinks) {
    struct Case {
        const char* fen;
        const char* what;
    };
    const Case dead[] = {
        {"8/8/8/4k3/8/8/8/2B1K3 w - - 0 1", "king and bishop against a bare king"},
        {"8/8/8/4k3/8/8/8/2N1K3 w - - 0 1", "king and knight against a bare king"},
        {"8/8/8/4k3/8/8/8/4K3 w - - 0 1", "bare kings"},
        {"8/8/8/2b1k3/8/8/8/2B1K3 w - - 0 1", "a bishop each, same colour complex"},
    };

    for (const auto& c : dead) {
        std::istringstream ss(c.fen);
        position p(ss);
        NNUEEvaluator ev(net_);
        ev.refresh(p);
        EXPECT_EQ(ev.evaluate(p), score::kDraw) << c.what << " -- " << c.fen;
    }

    // The guard must not swallow positions that are merely quiet. A rook is
    // still a rook, and returning a draw here would be a far worse bug than
    // the one being fixed.
    std::istringstream ss("8/8/8/4k3/8/8/8/R3K3 w - - 0 1");
    position alive(ss);
    NNUEEvaluator ev(net_);
    ev.refresh(alive);
    EXPECT_NE(ev.evaluate(alive), score::kDraw) << "king and rook against a bare king is not dead";
}

} // namespace havoc
