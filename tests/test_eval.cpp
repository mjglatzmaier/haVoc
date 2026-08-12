#include "havoc/bitboard.hpp"
#include "havoc/book.hpp"
#include "havoc/eval/hce.hpp"
#include "havoc/magics.hpp"
#include "havoc/material_table.hpp"
#include "havoc/parameters.hpp"
#include "havoc/pawn_table.hpp"
#include "havoc/position.hpp"
#include "havoc/tablebase.hpp"
#include "havoc/tt.hpp"
#include "havoc/zobrist.hpp"

#include <cmath>
#include <cstdio>
#include <sstream>
#include <string>

#include <gtest/gtest.h>

namespace {

/// Helper to build a position from a FEN string.
havoc::position make_pos(const std::string& fen) {
    std::istringstream iss(fen);
    return havoc::position(iss);
}

/// Fixture that initializes tables once for all tests.
class EvalTest : public ::testing::Test {
  protected:
    static void SetUpTestSuite() {
        havoc::bitboards::init();
        havoc::magics::init();
        havoc::zobrist::init();
    }
};

// ─── Startpos eval ≈ 0 ─────────────────────────────────────────────────────

TEST_F(EvalTest, StartposIsApproximatelyZero) {
    havoc::parameters params;
    havoc::pawn_table pt(params);
    havoc::material_table mt(params);
    havoc::HCEEvaluator eval(pt, mt, params);

    auto pos = make_pos("rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1");
    int score = eval.evaluate(pos);
    EXPECT_GE(score, -50) << "Startpos eval too low: " << score;
    EXPECT_LE(score, 50) << "Startpos eval too high: " << score;
}

// ─── Extra queen → large advantage ─────────────────────────────────────────

TEST_F(EvalTest, ExtraQueenForWhite) {
    havoc::parameters params;
    havoc::pawn_table pt(params);
    havoc::material_table mt(params);
    havoc::HCEEvaluator eval(pt, mt, params);

    // White has a queen, black doesn't (removed from d8)
    auto pos = make_pos("rnb1kbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1");
    int score = eval.evaluate(pos);
    EXPECT_GT(score, 800) << "Missing black queen eval too low: " << score;
}

// ─── KNK (king+knight vs king) is drawn ─────────────────────────────────────

TEST_F(EvalTest, KingKnightVsKingIsDraw) {
    havoc::parameters params;
    havoc::pawn_table pt(params);
    havoc::material_table mt(params);
    havoc::HCEEvaluator eval(pt, mt, params);

    auto pos = make_pos("8/8/8/8/4k3/8/8/K1N5 w - - 0 1");
    int score = eval.evaluate(pos);
    EXPECT_EQ(score, 0) << "KNK should be drawn, got: " << score;
}

// ─── Eval symmetry ──────────────────────────────────────────────────────────

TEST_F(EvalTest, EvalIsSymmetric) {
    havoc::parameters params;
    havoc::pawn_table pt(params);
    havoc::material_table mt(params);
    havoc::HCEEvaluator eval(pt, mt, params);

    // Symmetric position with white to move
    auto pos_w = make_pos("r1bqkbnr/pppppppp/2n5/8/4P3/5N2/PPPP1PPP/RNBQKB1R w KQkq - 2 3");
    int score_w = eval.evaluate(pos_w);

    // Mirrored position with black to move
    auto pos_b = make_pos("rnbqkb1r/pppp1ppp/5n2/4p3/8/2N5/PPPPPPPP/R1BQKBNR b KQkq - 2 3");
    int score_b = eval.evaluate(pos_b);

    // Both should be similar magnitude (from side-to-move perspective)
    EXPECT_NEAR(score_w, score_b, 30)
        << "White eval: " << score_w << ", Black mirrored eval: " << score_b;
}

// ─── TT basic operations ───────────────────────────────────────────────────

TEST_F(EvalTest, TTStoreAndFetch) {
    havoc::hash_table tt;
    tt.resize(1); // 1 MB

    havoc::Move m(havoc::E2, havoc::E4, havoc::quiet);
    tt.save(0x123456789ABCDEF0ULL, 10, havoc::bound_exact, m, 150, true);

    havoc::hash_data hd;
    bool found = tt.fetch(0x123456789ABCDEF0ULL, hd);
    EXPECT_TRUE(found);
    EXPECT_EQ(hd.depth, 10);
    EXPECT_EQ(hd.bound, havoc::bound_exact);
    EXPECT_EQ(hd.score, 150);
    EXPECT_EQ(hd.move.f, havoc::E2);
    EXPECT_EQ(hd.move.t, havoc::E4);
}

TEST_F(EvalTest, TTNegativeScore) {
    havoc::hash_table tt;
    tt.resize(1);

    havoc::Move m(havoc::D7, havoc::D5, havoc::quiet);
    tt.save(0xFEDCBA9876543210ULL, 5, havoc::bound_low, m, -300, false);

    havoc::hash_data hd;
    bool found = tt.fetch(0xFEDCBA9876543210ULL, hd);
    EXPECT_TRUE(found);
    EXPECT_EQ(hd.score, -300);
}

TEST_F(EvalTest, TTEvictsShallowestOnFullCluster) {
    havoc::hash_table tt;
    tt.resize(1);
    tt.clear();

    // Keys that differ only in their high bits land in the same cluster, since
    // the cluster index is taken from the low bits of the key.
    auto key_for = [](havoc::U64 i) { return (i << 40) | 0x1234ULL; };
    havoc::Move m(havoc::E2, havoc::E4, havoc::quiet);

    const havoc::U8 depths[4] = {20, 15, 3, 18};
    for (havoc::U64 i = 0; i < 4; ++i)
        tt.save(key_for(i + 1), depths[i], havoc::bound_exact, m, 100, false);

    havoc::hash_data hd;
    for (havoc::U64 i = 0; i < 4; ++i)
        EXPECT_TRUE(tt.fetch(key_for(i + 1), hd)) << "entry " << i << " should be stored";

    // A fifth key must evict something; the depth-3 entry is the cheapest loss.
    tt.save(key_for(5), 10, havoc::bound_exact, m, 200, false);

    EXPECT_TRUE(tt.fetch(key_for(5), hd));
    EXPECT_EQ(hd.depth, 10);
    EXPECT_FALSE(tt.fetch(key_for(3), hd)) << "the depth-3 entry should be the victim";
    EXPECT_TRUE(tt.fetch(key_for(1), hd));
    EXPECT_TRUE(tt.fetch(key_for(2), hd));
    EXPECT_TRUE(tt.fetch(key_for(4), hd));
}

TEST_F(EvalTest, TTPrefersEvictingOlderGenerations) {
    havoc::hash_table tt;
    tt.resize(1);
    tt.clear();

    auto key_for = [](havoc::U64 i) { return (i << 40) | 0x5678ULL; };
    havoc::Move m(havoc::E2, havoc::E4, havoc::quiet);

    // A slightly deeper entry from an old search, then shallow ones from a new
    // one. At comparable depths the stale entry should be the one to go.
    tt.save(key_for(1), 8, havoc::bound_exact, m, 100, false);
    tt.new_search();
    for (havoc::U64 i = 2; i <= 4; ++i)
        tt.save(key_for(i), 6, havoc::bound_exact, m, 100, false);

    tt.save(key_for(5), 6, havoc::bound_exact, m, 200, false);

    havoc::hash_data hd;
    EXPECT_TRUE(tt.fetch(key_for(5), hd));
    EXPECT_FALSE(tt.fetch(key_for(1), hd)) << "the stale entry should be the victim";
}

TEST_F(EvalTest, TTHashfull) {    havoc::hash_table tt;
    tt.resize(1);
    tt.clear();
    EXPECT_EQ(tt.hashfull(), 0);
}

// ─── KRK: rook endgame ─────────────────────────────────────────────────────

TEST_F(EvalTest, KRK_WinningForRookSide) {
    havoc::parameters params;
    havoc::pawn_table pt(params);
    havoc::material_table mt(params);
    havoc::HCEEvaluator eval(pt, mt, params);

    // White: Ke1, Ra1; Black: Ke8 — no pawns
    auto pos = make_pos("4k3/8/8/8/8/8/8/R3K3 w - - 0 1");
    int score = eval.evaluate(pos);
    EXPECT_GT(score, 400) << "KRK should be clearly winning for rook side, got: " << score;
}

// ─── KQK: queen endgame ────────────────────────────────────────────────────

TEST_F(EvalTest, KQK_WinningForQueenSide) {
    havoc::parameters params;
    havoc::pawn_table pt(params);
    havoc::material_table mt(params);
    havoc::HCEEvaluator eval(pt, mt, params);

    // White: Ke1, Qd1; Black: Ke8 — no pawns
    auto pos = make_pos("4k3/8/8/8/8/8/8/3QK3 w - - 0 1");
    int score = eval.evaluate(pos);
    EXPECT_GT(score, 800) << "KQK should be very winning for queen side, got: " << score;
}

// ─── Opposite color bishops should be drawish ──────────────────────────────

TEST_F(EvalTest, OppositeColorBishops_Scaled) {
    havoc::parameters params;
    havoc::pawn_table pt(params);
    havoc::material_table mt(params);
    havoc::HCEEvaluator eval(pt, mt, params);

    // Position with opposite color bishops and equal pawns
    auto pos = make_pos("8/pp3p2/2b1k3/8/8/2B1K3/PP3P2/8 w - - 0 1");
    int score = eval.evaluate(pos);
    EXPECT_LT(std::abs(score), 100) << "OCB endgame should be close to drawn, got: " << score;
}

// ─── Parameter round-trip ──────────────────────────────────────────────────

TEST_F(EvalTest, ParameterSaveLoad) {
    havoc::parameters p;
    p.uncastled_penalty = 42;
    p.opposite_bishop_scale = 77;
    p.save("/tmp/havoc_test_params.txt");

    havoc::parameters p2;
    p2.load("/tmp/havoc_test_params.txt");
    EXPECT_EQ(p2.uncastled_penalty, 42);
    EXPECT_EQ(p2.opposite_bishop_scale, 77);

    std::remove("/tmp/havoc_test_params.txt");
}

// ─── Tablebase stub ────────────────────────────────────────────────────────

TEST_F(EvalTest, TablebaseStubNotAvailable) {
    EXPECT_FALSE(havoc::tablebase::available());
    EXPECT_EQ(havoc::tablebase::max_pieces(), 0);
}

// ─── Book stub ─────────────────────────────────────────────────────────────

TEST_F(EvalTest, BookStubNotLoaded) {
    EXPECT_FALSE(havoc::book::is_loaded());
}

// Every parameter that save() writes must survive a load(). These two used
// different parameter sets, so category scales and material values were
// written to disk and then silently ignored on the way back in -- which made
// tuned parameter files unusable even when the tuning itself was sound.
TEST_F(EvalTest, ParameterFileRoundTripsEveryStage) {
    havoc::parameters original;

    original.sq_score_category_scale = 79;
    original.king_safety_category_scale = 108;
    original.passed_pawn_category_scale = 146;
    original.king_danger_divisor = 254;
    original.material_value[havoc::knight] = 321;
    original.material_value[havoc::rook] = 497;
    original.knight_mobility_scale = 117;

    const std::string path = "havoc_param_roundtrip_test.txt";
    ASSERT_TRUE(original.save(path));

    havoc::parameters loaded;
    ASSERT_TRUE(loaded.load(path));

    EXPECT_EQ(loaded.sq_score_category_scale, 79);
    EXPECT_EQ(loaded.king_safety_category_scale, 108);
    EXPECT_EQ(loaded.passed_pawn_category_scale, 146);
    EXPECT_EQ(loaded.king_danger_divisor, 254);
    EXPECT_EQ(loaded.material_value[havoc::knight], 321);
    EXPECT_EQ(loaded.material_value[havoc::rook], 497);
    EXPECT_EQ(loaded.knight_mobility_scale, 117);

    std::remove(path.c_str());
}

TEST_F(EvalTest, LoadingAMissingParameterFileFails) {
    havoc::parameters p;
    EXPECT_FALSE(p.load("this_file_does_not_exist_havoc.txt"));
}

// material_value used to be dead configuration: the real piece values were a
// constexpr table inside material_table.cpp, so the tuner could move these
// numbers all day without changing a single evaluation.
TEST_F(EvalTest, PieceValuesAreLive) {
    auto eval_with = [](int queen_value) {
        havoc::parameters params;
        params.material_value[havoc::queen] = queen_value;
        havoc::pawn_table pt(params);
        havoc::material_table mt(params);
        havoc::HCEEvaluator eval(pt, mt, params);
        // White is a queen up.
        auto pos = make_pos("4k3/8/8/8/8/8/8/3QK3 w - - 0 1");
        return eval.evaluate(pos);
    };

    const int cheap = eval_with(400);
    const int dear = eval_with(1200);

    EXPECT_GT(dear, cheap) << "raising the queen's value must raise the score "
                              "of a position that is a queen up";
    EXPECT_GT(dear - cheap, 100);
}

// The scaling tables are indexed by the Piece enum, which runs pawn..king.
// They were sized 5, so sq_score_scaling[king] read one past the end -- and
// the value that happened to sit there was 0, which silently multiplied the
// entire king piece-square table by zero.
TEST_F(EvalTest, ScalingTablesCoverEveryPiece) {
    havoc::parameters p;
    EXPECT_GT(p.sq_score_scaling.size(), static_cast<size_t>(havoc::king));
    EXPECT_GT(p.mobility_scaling.size(), static_cast<size_t>(havoc::king));
    EXPECT_EQ(p.sq_score_scaling[havoc::king], 1);
    EXPECT_EQ(p.mobility_scaling[havoc::king], 1);
}

TEST_F(EvalTest, KingPieceSquareTableIsLive) {
    auto eval_with = [](int king_scaling) {
        havoc::parameters params;
        params.sq_score_scaling[havoc::king] = king_scaling;
        havoc::pawn_table pt(params);
        havoc::material_table mt(params);
        havoc::HCEEvaluator eval(pt, mt, params);
        // Material on the board, so the pawnless-endgame short circuit does
        // not return a draw before the king table is consulted, and the kings
        // are on *different* table entries -- a symmetric position would have
        // the two sides' king scores cancel exactly.
        auto pos = make_pos("r1bqkb1r/pppppppp/2n2n2/8/8/2N2N2/PPPPPPPP/K1BQ1BNR w kq - 0 1");
        return eval.evaluate(pos);
    };

    EXPECT_NE(eval_with(1), eval_with(4))
        << "scaling the king piece-square table must change the evaluation";
}

// evaluate() returns a side-to-move-relative score, so a position that is
// symmetric under a colour swap must evaluate identically no matter whose turn
// it is. The tempo bonus is a property of *having the move*, so it has to be
// added after the side-to-move flip rather than before it.
TEST_F(EvalTest, TempoFavoursWhicheverSideIsToMove) {
    havoc::parameters params;
    havoc::pawn_table pt(params);
    havoc::material_table mt(params);
    havoc::HCEEvaluator eval(pt, mt, params);

    auto white_to_move = make_pos("rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1");
    auto black_to_move = make_pos("rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR b KQkq - 0 1");

    EXPECT_EQ(eval.evaluate(white_to_move), eval.evaluate(black_to_move));
    EXPECT_EQ(eval.evaluate(white_to_move), params.tempo);
}

// The lazy-eval cutoffs return early from the same function, so they must use
// the same sign convention as the full path.
TEST_F(EvalTest, LazyEvalAgreesWithTheFullEvalOnSign) {
    havoc::parameters lazy;
    havoc::pawn_table lazy_pt(lazy);
    havoc::material_table lazy_mt(lazy);
    havoc::HCEEvaluator lazy_eval(lazy_pt, lazy_mt, lazy);

    // A whole extra queen for white: lopsided enough that the sign is not in
    // doubt regardless of which path produced it.
    auto white_up = make_pos("rnb1kbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1");
    auto black_up = make_pos("rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNB1KBNR b KQkq - 0 1");

    // lazy_margin of 1 cuts out at the earliest opportunity.
    EXPECT_GT(lazy_eval.evaluate(white_up, 1), 0) << "white is up a queen and to move";
    EXPECT_GT(lazy_eval.evaluate(black_up, 1), 0) << "black is up a queen and to move";
    EXPECT_EQ(lazy_eval.evaluate(white_up, 1), lazy_eval.evaluate(black_up, 1));
}

// Every piece with a non-zero default piece-square table must actually have
// that table read. The queen's 128 entries were dead data for exactly this
// reason: eval_queens never called square_score.
TEST_F(EvalTest, EveryPieceSquareTableReachesTheEvaluation) {
    // Two positions differing only in where one side's piece of type `pc`
    // stands, on squares whose default table entries differ. If the table is
    // consulted the evaluations must differ.
    struct Case {
        havoc::Piece pc;
        const char* a;
        const char* b;
    };
    const Case cases[] = {
        // knight b1 (corner-ish, -40) vs e4 (centre, +20)
        {havoc::knight, "4k3/8/8/8/8/8/PPPPPPPP/1N2K3 w - - 0 1",
         "4k3/8/8/8/4N3/8/PPPPPPPP/4K3 w - - 0 1"},
        // bishop a1 (-20) vs d4 (+15)
        {havoc::bishop, "4k3/8/8/8/8/8/PPPPPPPP/B3K3 w - - 0 1",
         "4k3/8/8/8/3B4/8/PPPPPPPP/4K3 w - - 0 1"},
        // rook a1 (0) vs a7 (+5 .. +10 band)
        {havoc::rook, "4k3/8/8/8/8/8/PPPPPPPP/R3K3 w - - 0 1",
         "4k3/R7/8/8/8/8/PPPPPPPP/4K3 w - - 0 1"},
        // queen a1 (-20) vs d4 (+5)
        {havoc::queen, "4k3/8/8/8/8/8/PPPPPPPP/Q3K3 w - - 0 1",
         "4k3/8/8/8/3Q4/8/PPPPPPPP/4K3 w - - 0 1"},
    };

    for (const auto& c : cases) {
        havoc::parameters params;
        havoc::pawn_table pt(params);
        havoc::material_table mt(params);
        havoc::HCEEvaluator eval(pt, mt, params);
        auto pos_a = make_pos(c.a);
        auto pos_b = make_pos(c.b);
        int base_a = eval.evaluate(pos_a);
        int base_b = eval.evaluate(pos_b);

        // Now scale that one piece's table up and confirm the gap widens,
        // which isolates the piece-square term from the mobility and centre
        // terms that also differ between the two placements.
        havoc::parameters scaled;
        scaled.sq_score_scaling[c.pc] = 8;
        havoc::pawn_table spt(scaled);
        havoc::material_table smt(scaled);
        havoc::HCEEvaluator seval(spt, smt, scaled);
        auto spos_a = make_pos(c.a);
        auto spos_b = make_pos(c.b);
        int scaled_gap = seval.evaluate(spos_b) - seval.evaluate(spos_a);

        EXPECT_NE(scaled_gap, base_b - base_a)
            << "piece-square table for piece " << static_cast<int>(c.pc)
            << " is never read by the evaluation";
    }
}

} // namespace
