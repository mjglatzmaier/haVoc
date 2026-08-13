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

} // namespace havoc
