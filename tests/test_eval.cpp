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
    havoc::material_table mt;
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
    havoc::material_table mt;
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
    havoc::material_table mt;
    havoc::HCEEvaluator eval(pt, mt, params);

    auto pos = make_pos("8/8/8/8/4k3/8/8/K1N5 w - - 0 1");
    int score = eval.evaluate(pos);
    EXPECT_EQ(score, 0) << "KNK should be drawn, got: " << score;
}

// ─── Eval symmetry ──────────────────────────────────────────────────────────

TEST_F(EvalTest, EvalIsSymmetric) {
    havoc::parameters params;
    havoc::pawn_table pt(params);
    havoc::material_table mt;
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
    havoc::material_table mt;
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
    havoc::material_table mt;
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
    havoc::material_table mt;
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

} // namespace
