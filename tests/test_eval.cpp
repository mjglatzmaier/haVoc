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
#include <iostream>
#include <set>

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
    // Force a non-zero tempo: the shipped default is 0, which would make this
    // invariant hold trivially and stop it guarding anything.
    params.tempo = 25;
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
    lazy.tempo = 25;
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

// The pawn hash is keyed on pawn structure alone, so the pawn piece-square
// term used to be evaluated at a hard-coded phase of 0 -- pure middlegame --
// which meant the endgame pawn table was never read. That table is where pawn
// advancement is rewarded: a pawn on the seventh rank is worth 100 there
// against 50 in the middlegame table.
TEST_F(EvalTest, PawnPieceSquareTableTapersWithPhase) {
    auto eval_pos = [](const char* fen, bool zero_endgame_pawn_table) {
        havoc::parameters params;
        if (zero_endgame_pawn_table)
            params.pst_eg[havoc::pawn].fill(0);
        havoc::pawn_table pt(params);
        havoc::material_table mt(params);
        havoc::HCEEvaluator eval(pt, mt, params);
        auto pos = make_pos(fen);
        return eval.evaluate(pos);
    };

    // Kings and pawns only: maximum phase, so the endgame table should
    // dominate. White's pawns are far advanced, black's are on their home
    // rank, so zeroing the endgame table must move the score.
    const char* endgame = "4k3/pppppppp/8/8/PPPPPPPP/8/8/4K3 w - - 0 1";
    EXPECT_NE(eval_pos(endgame, false), eval_pos(endgame, true))
        << "the endgame pawn piece-square table is never read";

    // Full material: phase 0, pure middlegame, so the endgame table must have
    // no influence at all. This is the other half of the taper.
    const char* middlegame = "rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1";
    EXPECT_EQ(eval_pos(middlegame, false), eval_pos(middlegame, true))
        << "the endgame pawn table must not apply at phase 0";
}

// Every parameter the tuner is allowed to move must actually reach the
// evaluation. A parameter that is declared, saved, loaded and handed to the
// optimiser but never read is worse than useless: the tuner spends two full
// passes over the training set computing a gradient that is exactly zero, and
// the resulting file looks like a successful tune while changing nothing.
//
// This is not a hypothetical failure mode. material_value, the king
// piece-square scaling and the queen piece-square table were all dead in
// exactly this way, and the neutral result of a full three-stage tuning run is
// partly explained by it.
//
// Piece-square entries are excluded because a single position cannot exercise
// all 64 squares for all 6 pieces; EveryPieceSquareTableReachesTheEvaluation
// covers those separately.
TEST_F(EvalTest, EveryTunableParameterReachesTheEvaluation) {
    // Deliberately varied, to exercise the terms that only fire in particular
    // structures: open and closed centres, castled and uncastled kings,
    // passed pawns at several ranks, opposite-coloured bishops, pawnless
    // endgames and a bare king-and-pawn ending.
    const std::vector<std::string> fens = {
        // Openings and middlegames, symmetric and not
        "rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1",
        "r1bqk2r/pppp1ppp/2n2n2/2b1p3/2B1P3/2N2N2/PPPP1PPP/R1BQK2R w KQkq - 0 1",
        "r2q1rk1/pp2bppp/2n1bn2/2pp4/3P4/2P1PN2/PP1NBPPP/R1BQ1RK1 w - - 0 1",
        "2rq1rk1/pb1nbppp/1p2pn2/3p4/2PP4/1PN1PN2/PB2BPPP/R2Q1RK1 w - - 0 1",
        "r1bq1rk1/pp1nbppp/2p1pn2/3p4/2PP4/2N1PN2/PPQ1BPPP/R1B2RK1 w - - 0 1",
        "5rk1/1b3ppp/p7/1p1qP3/8/P4N2/1P3PPP/2RQ2K1 w - - 0 1",
        // Wide-open boards, to reach the top of the mobility tables
        "3q1rk1/5ppp/8/3B4/3R4/8/5PPP/3Q1RK1 w - - 0 1",
        "6k1/6pp/8/3B4/8/8/6PP/3R2K1 w - - 0 1",
        "8/8/4k3/8/3QB3/8/4K3/8 w - - 0 1",
        "3rr1k1/5ppp/8/3B1B2/3RR3/8/5PPP/6K1 w - - 0 1",
        // Cramped, to reach the bottom of the mobility tables
        "rnbqkbnr/pppppppp/8/8/8/PPPPPPPP/RNBQKBNR/8 w kq - 0 1",
        "1nb1kb2/1ppppp2/8/8/8/8/PPPPPP2/1NB1KB2 w - - 0 1",
        // Material imbalances, so the material values do not cancel
        "rnb1kbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1",
        "r1bqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1",
        "rnbqkb1r/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1",
        "rnbqkbn1/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1",
        "rnbqkbnr/pppppppp/8/8/8/8/PPPPPPP1/RNBQKBNR w KQkq - 0 1",
        // Exposed and uncastled kings, with real attackers
        "r1bqk2r/pppp1ppp/2n5/4P3/1bB5/2N2Q2/PPP2PPP/R1B1K2R w KQkq - 0 1",
        "r3k2r/8/8/8/8/8/8/R3K2R w KQkq - 0 1",
        "4k3/8/8/8/8/8/8/R3K3 w Q - 0 1",
        "6k1/5p1p/6p1/8/8/1Q6/5PPP/6K1 w - - 0 1",
        "r4rk1/ppp2ppp/8/8/8/6Q1/PPP2PPP/R4RK1 w - - 0 1",
        // Asymmetric pawn structures: doubled, isolated, backward, majorities
        "4k3/pp3ppp/8/8/8/8/P1P1PP1P/4K3 w - - 0 1",
        "4k3/1p1p1p1p/8/8/8/8/PPP2PPP/4K3 w - - 0 1",
        "6k1/2p2ppp/8/8/8/8/PP4PP/6K1 w - - 0 1",
        // Passed pawns at several ranks
        "6k1/5ppp/8/8/8/8/PPP5/6K1 w - - 0 1",
        "8/1P6/8/8/8/8/6p1/K6k w - - 0 1",
        "8/8/1P6/8/8/6p1/8/K6k w - - 0 1",
        "8/8/8/1P6/8/8/6p1/K6k w - - 0 1",
        "4k3/pppppppp/8/8/PPPPPPPP/8/8/4K3 w - - 0 1",
        // Endgame scale factors: opposite bishops, pawnless, lone minor
        "6k1/5ppp/8/8/3b4/8/2B2PPP/6K1 w - - 0 1",
        "6k1/8/8/3b4/8/8/2B5/6K1 w - - 0 1",
        "6k1/8/8/8/8/8/2B5/6K1 w - - 0 1",
        "6k1/8/8/8/8/8/2N5/6K1 w - - 0 1",
        "6k1/8/8/8/8/8/2R5/6K1 w - - 0 1",
        "6k1/5ppp/8/8/8/8/5PPP/2R3K1 w - - 0 1",
        // King and pawn endings
        "8/3k4/8/8/3P4/3K4/8/8 w - - 0 1",
        "8/2p5/3p4/KP5r/1R3p1k/8/4P1P1/8 w - - 0 1",
        "6k1/5ppp/8/8/8/8/1P6/1K6 w - - 0 1",
    };

    // The pawn and material caches are keyed on structure, not on parameter
    // values, so they must be cleared after every mutation. They are also tens
    // of megabytes each, so they are allocated once and reused rather than
    // rebuilt for every probe.
    havoc::parameters params;
    havoc::pawn_table pt(params);
    havoc::material_table mt(params);
    havoc::HCEEvaluator ev(pt, mt, params);

    auto eval_all = [&]() {
        pt.clear();
        mt.clear();
        std::vector<int> out;
        out.reserve(fens.size());
        for (const auto& f : fens) {
            auto pos = make_pos(f.c_str());
            out.push_back(ev.evaluate(pos));
        }
        return out;
    };

    const std::vector<int> baseline = eval_all();

    auto slots = params.every_param();
    std::vector<std::string> dead;

    for (auto& [name, slot] : slots) {
        if (name.rfind("pst_", 0) == 0)
            continue;

        const int original = *slot;
        bool moved = false;
        // Both directions, because a parameter can sit at a clamp, and a large
        // step, because integer division can swallow a small one.
        for (int delta : {64, -64}) {
            *slot = original + delta;
            if (eval_all() != baseline) {
                moved = true;
                break;
            }
        }
        *slot = original;
        if (!moved)
            dead.push_back(name);
    }

    // Parameters this position set provably cannot exercise, or that are
    // deliberately inert. The assertion is that `dead` is a SUBSET of this
    // list, so wiring one up does not break the test but introducing a new
    // dead parameter does.
    //
    // Two groups, and the distinction matters:
    //
    //   (a) Coverage gaps. Mobility tables are indexed by the number of safe
    //       squares a piece has, and no finite set of positions hits every
    //       bucket for every piece; likewise the king-safety tables are
    //       indexed by attacker and shelter counts. These entries are read by
    //       live code, they just are not reached from here.
    //
    //   (b) Genuinely dead. These reach no code at all. They are still
    //       registered with the tuner, which means it spends two full passes
    //       over the training set per iteration computing a gradient that is
    //       identically zero. That is a real cost and a real source of false
    //       confidence in a tuned parameter file.
    const std::set<std::string> known_unreached = {
        // (a) coverage gaps
        "knight_mobility_0", "knight_mobility_6", "knight_mobility_7", "knight_mobility_8",
        "bishop_mobility_5", "bishop_mobility_9", "bishop_mobility_11", "bishop_mobility_12",
        "bishop_mobility_14", "rook_mobility_3", "rook_mobility_11", "rook_mobility_13",
        "attacker_weight_1", "king_shelter_0", "king_shelter_3", "king_safe_sqs_0",
        "king_safe_sqs_4", "king_safe_sqs_5", "king_safe_sqs_6", "king_safe_sqs_7",
        "no_pawn_scale", "minor_advantage_no_pawn_scale",
        // (b) inert by design: pawns are valued by the pawn hash, and a king
        // is never exchanged, so neither value can move an evaluation
        "material_value_0", "material_value_5",
        // (b) genuinely dead: declared, tuned and saved, but read by nothing.
        // attacker_weight_0 is the pawn entry of the king-danger sum, whose
        // loop starts at knight. passed_pawn_rank_bonus and uncastled_penalty
        // are features that were never implemented -- eval_passed_pawns uses
        // its own hard-coded constants and eval_king has a hard-coded castling
        // bonus with no matching penalty.
        "attacker_weight_0", "passed_pawn_rank_bonus_0", "passed_pawn_rank_bonus_1",
        "passed_pawn_rank_bonus_2", "passed_pawn_rank_bonus_3", "uncastled_penalty",
    };

    std::vector<std::string> unexpected;
    for (const auto& d : dead)
        if (!known_unreached.count(d))
            unexpected.push_back(d);

    std::string report;
    for (const auto& d : unexpected)
        report += "\n  " + d;
    EXPECT_TRUE(unexpected.empty())
        << unexpected.size()
        << " tunable parameter(s) newly fail to reach the evaluation:" << report
        << "\n\nA parameter the tuner can move but the evaluation never reads costs two"
           "\nfull passes over the training set per iteration and returns a zero gradient.";
}

} // namespace

// The pawn hash is keyed on pawn structure alone, so nothing that depends on
// anything else may be cached in a pawn_entry. Castling is the case that makes
// this concrete: it moves the king two squares and leaves every pawn where it
// was, so it hits the entry the pre-castling position filled.
//
// The king shelter mask used to be built in evaluate_pawns() from
// p.king_square(c) and stored in the entry, so the shelter term was scored
// against whichever king square happened to fill the slot first. Evaluating the
// castled position on a cold table and again after the uncastled one had
// polluted it gave two different numbers.
//
// Stated as an invariant that holds for any cache: evaluating a position must
// not depend on what was evaluated before it.
TEST_F(EvalTest, EvaluationDoesNotDependOnWhatWasEvaluatedBefore) {
    havoc::parameters params;
    havoc::pawn_table pt(params);
    havoc::material_table mt(params);
    havoc::HCEEvaluator eval(pt, mt, params);

    // Identical pawn structures, king on a different square.
    const std::vector<std::pair<std::string, std::string>> pairs = {
        {"r1bqk2r/pppp1ppp/2n2n2/2b1p3/2B1P3/2N2N2/PPPP1PPP/R1BQ1RK1 w kq - 0 1",
         "r1bqk2r/pppp1ppp/2n2n2/2b1p3/2B1P3/2N2N2/PPPP1PPP/R1BQK2R w KQkq - 0 1"},
        {"6k1/pp3ppp/8/8/8/8/PP3PPP/2K5 w - - 0 1",
         "6k1/pp3ppp/8/8/8/8/PP3PPP/6K1 w - - 0 1"},
        {"r4rk1/1pp2ppp/8/8/8/8/1PP2PPP/R4RK1 w - - 0 1",
         "r4rk1/1pp2ppp/8/8/8/8/1PP2PPP/R2K3R w - - 0 1"},
    };

    for (const auto& [a_fen, b_fen] : pairs) {
        auto a = make_pos(a_fen);
        auto b = make_pos(b_fen);
        ASSERT_EQ(a.pawnkey(), b.pawnkey()) << "test positions must share a pawn structure";

        pt.clear();
        mt.clear();
        const float b_cold = eval.evaluate(b, -1.0f);

        pt.clear();
        mt.clear();
        eval.evaluate(a, -1.0f); // fills the shared pawn entry from a
        const float b_warm = eval.evaluate(b, -1.0f);

        EXPECT_FLOAT_EQ(b_cold, b_warm)
            << "evaluation of " << b_fen << " changed after evaluating " << a_fen;
    }
}
