/// @file test_eval_discrimination.cpp
/// @brief Paired quiet positions of equal material that the evaluation must
///        rank correctly.
///
/// Why this file exists
/// --------------------
/// The evaluation already has two measurements pointed at it, and neither one
/// measures the thing that actually chooses moves.
///
/// Texel tuning measures *global* calibration: given a static position, how
/// well does the evaluation predict the eventual game result? That quantity is
/// dominated by material, and a fit can improve it substantially by doing
/// nothing more than repricing the pieces. A recent refit cut held-out error
/// by 7.6% relative, and the single largest change it made was the queen, from
/// 910 to 1078.
///
/// An SPRT measures strength, which is the truth, but it costs tens of
/// thousands of games to resolve a few Elo and says nothing about *which* term
/// is wrong when it comes back negative.
///
/// Between those sits the property that decides moves: the ability to rank two
/// positions that are one ply apart and therefore almost always have identical
/// material. Repricing a queen contributes exactly nothing there. Nothing in
/// the test suite covered it, so an evaluation could lose that ability
/// entirely -- to a mis-signed term, a term scaled to irrelevance, or a term
/// that was never written -- while every existing test stayed green.
///
/// Each case below is a pair of legal, quiet positions with the *same material
/// on both sides*, differing in one positional feature that chess theory has a
/// settled opinion about. Material cancels by construction, so the assertion is
/// a direct statement about positional understanding.
///
/// A failure here is not a tuning problem. It means the feature is either
/// absent from the evaluation, mis-signed, or outweighed by something that
/// should not dominate it -- none of which a weight tuner can discover, since
/// several of these features barely occur in a quiet-filtered training set.
///
/// One trap when adding cases: sparse material draws the position into a
/// specialised endgame evaluator (KRK, KPK, KBNK and friends, hce.cpp), which
/// returns a mate-driving score and ignores the general term under test. The
/// first draft of the rook-on-the-seventh pair was bare KRK and both sides
/// evaluated to exactly the same number for that reason. Keep enough pawns on
/// the board that the ordinary evaluation actually runs.

#include "havoc/bitboard.hpp"
#include "havoc/eval/hce.hpp"
#include "havoc/kpk.hpp"
#include "havoc/magics.hpp"
#include "havoc/material_table.hpp"
#include "havoc/parameters.hpp"
#include "havoc/pawn_table.hpp"
#include "havoc/position.hpp"
#include "havoc/zobrist.hpp"

#include <gtest/gtest.h>

#include <sstream>
#include <string>
#include <vector>

namespace {

havoc::position make_pos(const std::string& fen) {
    std::istringstream iss(fen);
    return havoc::position(iss);
}

/// One discrimination case.
///
/// `better` and `worse` must have identical material for both colours and the
/// same side to move, so that the difference in score is attributable to the
/// named feature alone.
struct Pair {
    const char* feature;
    const char* better;
    const char* worse;
    const char* rationale;
};

const std::vector<Pair> kPairs = {
    {"connected pawns beat isolated pawns",
     "4k3/8/8/8/8/8/PP6/4K3 w - - 0 1",
     "4k3/8/8/8/8/8/P1P5/4K3 w - - 0 1",
     "a2+b2 defend each other; a2+c2 are both isolated and need pieces to hold them"},

    {"healthy pawns beat doubled pawns",
     "4k3/8/8/8/8/8/PP6/4K3 w - - 0 1",
     "4k3/8/8/8/P7/8/P7/4K3 w - - 0 1",
     "doubled a-pawns cover fewer files and cannot defend one another"},

    {"a passed pawn beats a blocked pawn",
     "4k3/p7/8/4P3/8/8/8/4K3 w - - 0 1",
     "4k3/8/4p3/4P3/8/8/8/4K3 w - - 0 1",
     "e5 with no black pawn ahead is passed; e5 facing e6 is permanently blocked"},

    {"a rook prefers an open file",
     "4k3/8/8/8/8/8/PP6/3RK3 w - - 0 1",
     "4k3/8/8/8/8/8/3PP3/3RK3 w - - 0 1",
     "the d-rook is unobstructed in the first, staring at its own d2 pawn in the second"},

    {"a castled king beats a king in the centre",
     "4k2r/5ppp/8/8/8/8/5PPP/5RK1 w k - 0 1",
     "4k2r/5ppp/8/8/8/8/5PPP/4KR2 w k - 0 1",
     "same pieces; g1 sits behind an intact f2-g2-h2 shelter, e1 does not"},

    {"a knight prefers an advanced supported outpost",
     "4k3/pp6/8/3N4/4P3/8/8/4K3 w - - 0 1",
     "4k3/pp6/8/8/4P3/3N4/8/4K3 w - - 0 1",
     "d5 is supported by e4 and cannot be driven off by a pawn; d3 is passive"},

    {"a rook prefers the seventh rank",
     "4k3/3R1ppp/8/8/8/8/5PPP/4K3 w - - 0 1",
     "4k3/5ppp/8/8/8/8/3R1PPP/4K3 w - - 0 1",
     "d7 rakes the seventh rank and ties black to f7; d2 is passive"},

    {"an endgame king prefers the centre",
     "7k/8/8/3K4/8/8/8/R6r w - - 0 1",
     "7k/8/8/8/8/8/K7/R6r w - - 0 1",
     "with queens off, a centralised king is worth roughly a third of a pawn"},

    {"an intact shelter beats an advanced one",
     "6k1/5ppp/8/8/8/8/5PPP/6K1 w - - 0 1",
     "6k1/5ppp/8/8/5P1P/8/6P1/6K1 w - - 0 1",
     "f4/h4 have left holes on g3 and the third rank in front of the king"},

    {"a bishop prefers pawns on the opposite colour",
     "4k3/pp6/8/8/3P4/4P3/8/4KB2 w - - 0 1",
     "4k3/pp6/8/8/4P3/3P4/8/4KB2 w - - 0 1",
     "the f1 bishop is light-squared; d4/e3 are dark, while e4/d3 block it"},

    {"a rook belongs behind its own passed pawn",
     "5k2/8/8/4P3/8/8/8/4RK2 w - - 0 1",
     "5k2/8/4R3/4P3/8/8/8/5K2 w - - 0 1",
     "the rook supports the advance from behind instead of blocking it"},

    {"a protected passer beats a lone passer",
     "4k3/8/8/3PP3/8/8/8/4K3 w - - 0 1",
     "4k3/8/8/4P3/8/3P4/8/4K3 w - - 0 1",
     "d5 and e5 defend each other as they advance; d3 and e5 are split"},
};

class EvalDiscriminationTest : public ::testing::Test {
  protected:
    static void SetUpTestSuite() {
        havoc::bitboards::init();
        havoc::magics::init();
        havoc::zobrist::init();
        havoc::kpk::init();
    }

    /// Assert that both sides really do have identical material, so that a
    /// score difference cannot be explained by the piece values. A case that
    /// fails this check is a broken test, not a broken evaluation.
    static void require_equal_material(const havoc::position& a,
                                       const havoc::position& b,
                                       const std::string& feature) {
        for (int c = 0; c < 2; ++c)
            for (int pc = 0; pc < havoc::pieces; ++pc)
                ASSERT_EQ(a.number_of(havoc::Color(c), havoc::Piece(pc)),
                          b.number_of(havoc::Color(c), havoc::Piece(pc)))
                    << feature << ": the two positions do not have the same material, "
                    << "so this case cannot isolate the feature";
    }
};

TEST_F(EvalDiscriminationTest, RanksEqualMaterialPositionsByPositionalMerit) {
    havoc::parameters params;
    havoc::pawn_table pt(params);
    havoc::material_table mt(params);
    havoc::HCEEvaluator eval(pt, mt, params);

    std::vector<std::string> failures;

    for (const auto& c : kPairs) {
        auto better = make_pos(c.better);
        auto worse = make_pos(c.worse);
        require_equal_material(better, worse, c.feature);

        const int sb = eval.evaluate(better);
        const int sw = eval.evaluate(worse);

        if (sb <= sw) {
            failures.push_back(std::string(c.feature) + "\n      better " + c.better +
                               " -> " + std::to_string(sb) + "\n      worse  " + c.worse +
                               " -> " + std::to_string(sw) +
                               "\n      margin " + std::to_string(sb - sw) +
                               "\n      why: " + c.rationale);
        }
    }

    if (!failures.empty()) {
        std::string msg = std::to_string(failures.size()) + " of " +
                          std::to_string(kPairs.size()) +
                          " equal-material pairs are ranked wrongly:\n";
        for (const auto& f : failures)
            msg += "\n  - " + f + "\n";
        FAIL() << msg;
    }
}

} // namespace
