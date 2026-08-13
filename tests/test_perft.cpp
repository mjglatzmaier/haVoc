#include "havoc/bitboard.hpp"
#include "havoc/kpk.hpp"
#include "havoc/magics.hpp"
#include "havoc/movegen.hpp"
#include "havoc/position.hpp"
#include "havoc/zobrist.hpp"

#include <cstdint>
#include <sstream>

#include <gtest/gtest.h>

namespace havoc {

static uint64_t perft(position& pos, int depth) {
    if (depth == 0)
        return 1;

    Movegen mvs(pos);
    mvs.generate<pseudo_legal, pieces>();

    uint64_t nodes = 0;
    for (int i = 0; i < mvs.size(); ++i) {
        if (!pos.is_legal(mvs[i]))
            continue;
        pos.do_move(mvs[i]);
        nodes += perft(pos, depth - 1);
        pos.undo_move(mvs[i]);
    }
    return nodes;
}

class PerftTest : public ::testing::Test {
  protected:
    void SetUp() override {
        bitboards::init();
        magics::init();
        zobrist::init();
        kpk::init();
    }
};

// ── Starting position ───────────────────────────────────────────────────────

TEST_F(PerftTest, StartposDepth1) {
    std::istringstream fen("rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1");
    position pos(fen);
    EXPECT_EQ(perft(pos, 1), 20ULL);
}

TEST_F(PerftTest, StartposDepth2) {
    std::istringstream fen("rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1");
    position pos(fen);
    EXPECT_EQ(perft(pos, 2), 400ULL);
}

TEST_F(PerftTest, StartposDepth3) {
    std::istringstream fen("rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1");
    position pos(fen);
    EXPECT_EQ(perft(pos, 3), 8902ULL);
}

TEST_F(PerftTest, StartposDepth4) {
    std::istringstream fen("rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1");
    position pos(fen);
    EXPECT_EQ(perft(pos, 4), 197281ULL);
}

// ── Kiwipete ────────────────────────────────────────────────────────────────

TEST_F(PerftTest, KiwipeteDepth1) {
    std::istringstream fen("r3k2r/p1ppqpb1/bn2pnp1/3PN3/1p2P3/2N2Q1p/PPPBBPPP/R3K2R w KQkq - 0 1");
    position pos(fen);
    EXPECT_EQ(perft(pos, 1), 48ULL);
}

TEST_F(PerftTest, KiwipeteDepth2) {
    std::istringstream fen("r3k2r/p1ppqpb1/bn2pnp1/3PN3/1p2P3/2N2Q1p/PPPBBPPP/R3K2R w KQkq - 0 1");
    position pos(fen);
    EXPECT_EQ(perft(pos, 2), 2039ULL);
}

TEST_F(PerftTest, KiwipeteDepth3) {
    std::istringstream fen("r3k2r/p1ppqpb1/bn2pnp1/3PN3/1p2P3/2N2Q1p/PPPBBPPP/R3K2R w KQkq - 0 1");
    position pos(fen);
    EXPECT_EQ(perft(pos, 3), 97862ULL);
}

// ── Position 3 — en passant / pin edge cases ────────────────────────────────

TEST_F(PerftTest, Position3Depth1) {
    std::istringstream fen("8/2p5/3p4/KP5r/1R3p1k/8/4P1P1/8 w - - 0 1");
    position pos(fen);
    EXPECT_EQ(perft(pos, 1), 14ULL);
}

TEST_F(PerftTest, Position3Depth2) {
    std::istringstream fen("8/2p5/3p4/KP5r/1R3p1k/8/4P1P1/8 w - - 0 1");
    position pos(fen);
    EXPECT_EQ(perft(pos, 2), 191ULL);
}

TEST_F(PerftTest, Position3Depth3) {
    std::istringstream fen("8/2p5/3p4/KP5r/1R3p1k/8/4P1P1/8 w - - 0 1");
    position pos(fen);
    EXPECT_EQ(perft(pos, 3), 2812ULL);
}

// ── Position 4 — promotions ─────────────────────────────────────────────────

TEST_F(PerftTest, Position4Depth1) {
    std::istringstream fen("r3k2r/Pppp1ppp/1b3nbN/nP6/BBP1P3/q4N2/Pp1P2PP/R2Q1RK1 w kq - 0 1");
    position pos(fen);
    EXPECT_EQ(perft(pos, 1), 6ULL);
}

TEST_F(PerftTest, Position4Depth2) {
    std::istringstream fen("r3k2r/Pppp1ppp/1b3nbN/nP6/BBP1P3/q4N2/Pp1P2PP/R2Q1RK1 w kq - 0 1");
    position pos(fen);
    EXPECT_EQ(perft(pos, 2), 264ULL);
}

TEST_F(PerftTest, Position4Depth3) {
    std::istringstream fen("r3k2r/Pppp1ppp/1b3nbN/nP6/BBP1P3/q4N2/Pp1P2PP/R2Q1RK1 w kq - 0 1");
    position pos(fen);
    EXPECT_EQ(perft(pos, 3), 9467ULL);
}

// ── Position 5 — promotion + check ──────────────────────────────────────────

TEST_F(PerftTest, Position5Depth1) {
    std::istringstream fen("rnbq1k1r/pp1Pbppp/2p5/8/2B5/8/PPP1NnPP/RNBQK2R w KQ - 1 8");
    position pos(fen);
    EXPECT_EQ(perft(pos, 1), 44ULL);
}

TEST_F(PerftTest, Position5Depth2) {
    std::istringstream fen("rnbq1k1r/pp1Pbppp/2p5/8/2B5/8/PPP1NnPP/RNBQK2R w KQ - 1 8");
    position pos(fen);
    EXPECT_EQ(perft(pos, 2), 1486ULL);
}

TEST_F(PerftTest, Position5Depth3) {
    std::istringstream fen("rnbq1k1r/pp1Pbppp/2p5/8/2B5/8/PPP1NnPP/RNBQK2R w KQ - 1 8");
    position pos(fen);
    EXPECT_EQ(perft(pos, 3), 62379ULL);
}

// ── Deeper standard positions ───────────────────────────────────────────────
//
// The cases above stop at depth 3, which is shallow enough that a generator
// can be wrong and still pass: a missing en-passant evasion or a castle
// through an attacked square often needs four or more plies to show up in the
// count. These carry the same six positions deeper.

TEST_F(PerftTest, StartposDepth5) {
    std::istringstream fen("rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1");
    position pos(fen);
    EXPECT_EQ(perft(pos, 5), 4865609ULL);
}

TEST_F(PerftTest, KiwipeteDepth4) {
    std::istringstream fen("r3k2r/p1ppqpb1/bn2pnp1/3PN3/1p2P3/2N2Q1p/PPPBBPPP/R3K2R w KQkq - 0 1");
    position pos(fen);
    EXPECT_EQ(perft(pos, 4), 4085603ULL);
}

TEST_F(PerftTest, Position3Depth5) {
    std::istringstream fen("8/2p5/3p4/KP5r/1R3p1k/8/4P1P1/8 w - - 0 1");
    position pos(fen);
    EXPECT_EQ(perft(pos, 5), 674624ULL);
}

TEST_F(PerftTest, Position4Depth4) {
    std::istringstream fen("r3k2r/Pppp1ppp/1b3nbN/nP6/BBP1P3/q4N2/Pp1P2PP/R2Q1RK1 w kq - 0 1");
    position pos(fen);
    EXPECT_EQ(perft(pos, 4), 422333ULL);
}

TEST_F(PerftTest, Position5Depth4) {
    std::istringstream fen("rnbq1k1r/pp1Pbppp/2p5/8/2B5/8/PPP1NnPP/RNBQK2R w KQ - 1 8");
    position pos(fen);
    EXPECT_EQ(perft(pos, 4), 2103487ULL);
}

TEST_F(PerftTest, Position6Depth4) {
    std::istringstream fen("r4rk1/1pp1qppp/p1np1n2/2b1p1B1/2B1P1b1/P1NP1N2/1PP1QPPP/R4RK1 w - - 0 10");
    position pos(fen);
    EXPECT_EQ(perft(pos, 4), 3894594ULL);
}

// ── Rule edge cases ─────────────────────────────────────────────────────────
//
// Each of these isolates one rule that a generator can get wrong without any
// of the positions above noticing, and each is small enough that a failure
// points straight at the rule that broke.

TEST_F(PerftTest, EnPassantExposesOwnKing) {
    std::istringstream fen("3k4/3p4/8/K1P4r/8/8/8/8 b - - 0 1");
    position pos(fen);
    EXPECT_EQ(perft(pos, 6), 1134888ULL);
}

TEST_F(PerftTest, EnPassantCaptureGivesCheck) {
    std::istringstream fen("8/8/4k3/8/2p5/8/B2P2K1/8 w - - 0 1");
    position pos(fen);
    EXPECT_EQ(perft(pos, 6), 1015133ULL);
}

TEST_F(PerftTest, EnPassantAsCheckEvasion) {
    std::istringstream fen("8/8/1k6/2b5/2pP4/8/5K2/8 b - d3 0 1");
    position pos(fen);
    EXPECT_EQ(perft(pos, 6), 1440467ULL);
}

TEST_F(PerftTest, ShortCastleGivesCheck) {
    std::istringstream fen("5k2/8/8/8/8/8/8/4K2R w K - 0 1");
    position pos(fen);
    EXPECT_EQ(perft(pos, 6), 661072ULL);
}

TEST_F(PerftTest, LongCastleGivesCheck) {
    std::istringstream fen("3k4/8/8/8/8/8/8/R3K3 w Q - 0 1");
    position pos(fen);
    EXPECT_EQ(perft(pos, 6), 803711ULL);
}

TEST_F(PerftTest, CastleRightsAreLostCorrectly) {
    std::istringstream fen("r3k2r/1b4bq/8/8/8/8/7B/R3K2R w KQkq - 0 1");
    position pos(fen);
    EXPECT_EQ(perft(pos, 4), 1274206ULL);
}

TEST_F(PerftTest, CastlingThroughAttackIsRejected) {
    std::istringstream fen("r3k2r/8/3Q4/8/8/5q2/8/R3K2R b KQkq - 0 1");
    position pos(fen);
    EXPECT_EQ(perft(pos, 4), 1720476ULL);
}

TEST_F(PerftTest, PromotionOutOfCheck) {
    std::istringstream fen("2K2r2/4P3/8/8/8/8/8/3k4 w - - 0 1");
    position pos(fen);
    EXPECT_EQ(perft(pos, 6), 3821001ULL);
}

TEST_F(PerftTest, DiscoveredCheck) {
    std::istringstream fen("8/8/1P2K3/8/2n5/1q6/8/5k2 b - - 0 1");
    position pos(fen);
    EXPECT_EQ(perft(pos, 5), 1004658ULL);
}

TEST_F(PerftTest, PromotionGivesCheck) {
    std::istringstream fen("4k3/1P6/8/8/8/8/K7/8 w - - 0 1");
    position pos(fen);
    EXPECT_EQ(perft(pos, 6), 217342ULL);
}

TEST_F(PerftTest, UnderPromotionGivesCheck) {
    std::istringstream fen("8/P1k5/K7/8/8/8/8/8 w - - 0 1");
    position pos(fen);
    EXPECT_EQ(perft(pos, 6), 92683ULL);
}

TEST_F(PerftTest, SelfStalemate) {
    std::istringstream fen("K1k5/8/P7/8/8/8/8/8 w - - 0 1");
    position pos(fen);
    EXPECT_EQ(perft(pos, 6), 2217ULL);
}

TEST_F(PerftTest, StalemateAndCheck) {
    std::istringstream fen("8/k1P5/8/1K6/8/8/8/8 w - - 0 1");
    position pos(fen);
    EXPECT_EQ(perft(pos, 7), 567584ULL);
}

TEST_F(PerftTest, DoubleCheckAllowsOnlyKingMoves) {
    std::istringstream fen("8/8/2k5/5q2/5n2/8/5K2/8 b - - 0 1");
    position pos(fen);
    EXPECT_EQ(perft(pos, 4), 23527ULL);
}

} // namespace havoc
