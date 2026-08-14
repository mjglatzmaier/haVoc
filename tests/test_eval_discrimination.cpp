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
#include "havoc/eval/eval_pairs.hpp"
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

using havoc::eval_pairs::Pair;


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

    for (const auto& c : havoc::eval_pairs::all()) {
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
                          std::to_string(havoc::eval_pairs::all().size()) +
                          " equal-material pairs are ranked wrongly:\n";
        for (const auto& f : failures)
            msg += "\n  - " + f + "\n";
        FAIL() << msg;
    }
}

} // namespace
