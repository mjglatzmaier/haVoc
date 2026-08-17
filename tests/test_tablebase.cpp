/// @file test_tablebase.cpp
/// @brief Syzygy probing.
///
/// The tables themselves are ~1 GB and are not in the repository, so the tests
/// that need real data are skipped unless HAVOC_SYZYGY_PATH points at a
/// directory of them. Everything that can be checked without the data -- the
/// guards, the failure contract, the behaviour when nothing is loaded -- runs
/// unconditionally, because those are the paths a default build actually
/// takes.

#include "havoc/bitboard.hpp"
#include "havoc/magics.hpp"
#include "havoc/position.hpp"
#include "havoc/tablebase.hpp"
#include "havoc/zobrist.hpp"

#include <cstdlib>
#include <sstream>
#include <string>

#include <gtest/gtest.h>

using namespace havoc;

namespace {

position make_position(const std::string& fen) {
    std::istringstream ss(fen);
    return position(ss);
}

/// The directory of .rtbw/.rtbz files, or empty when the suite should skip.
/// A build without HAVOC_SYZYGY has no probing code to exercise, so the
/// data-backed cases skip there even if the path is set.
std::string tb_path() {
#if HAVOC_SYZYGY
    const char* p = std::getenv("HAVOC_SYZYGY_PATH");
    return p ? std::string(p) : std::string();
#else
    return {};
#endif
}

struct TablebaseEnv : ::testing::Test {
    void SetUp() override {
        static bool once = [] {
            bitboards::init();
            magics::init();
            zobrist::init();
            return true;
        }();
        (void)once;
    }
    void TearDown() override { tablebase::shutdown(); }
};

} // namespace

// ─── Without tables ─────────────────────────────────────────────────────────

TEST_F(TablebaseEnv, AnEmptyPathUnloadsRatherThanFailing) {
    // A GUI clearing SyzygyPath is not an error, and reporting it as one would
    // make an operator chase a problem that does not exist.
    EXPECT_TRUE(tablebase::init(""));
    EXPECT_FALSE(tablebase::available());
    EXPECT_EQ(tablebase::max_pieces(), 0);
}

TEST_F(TablebaseEnv, ANonexistentPathFailsAndLeavesNothingLoaded) {
    EXPECT_FALSE(tablebase::init("/nonexistent/havoc/syzygy/definitely-not-here"));
    EXPECT_FALSE(tablebase::available());
}

TEST_F(TablebaseEnv, ProbingWithNoTablesFailsRatherThanClaimingADraw) {
    // kProbeFailed has to be distinguishable from 0. If a failed probe were
    // reported as a draw the search would return score::kDraw for every
    // endgame it reached, which is the worst possible failure mode: silent,
    // and confined to won positions.
    tablebase::init("");
    const position p = make_position("8/8/8/8/8/4k3/8/R3K3 w - - 0 1");
    EXPECT_EQ(tablebase::probe_wdl(p), tablebase::kProbeFailed);
    EXPECT_NE(tablebase::kProbeFailed, 0);
}

// ─── With tables ────────────────────────────────────────────────────────────

TEST_F(TablebaseEnv, LoadsRealTables) {
    if (tb_path().empty())
        GTEST_SKIP() << "set HAVOC_SYZYGY_PATH to run";
    ASSERT_TRUE(tablebase::init(tb_path()));
    EXPECT_TRUE(tablebase::available());
    EXPECT_GE(tablebase::max_pieces(), 3);
}

TEST_F(TablebaseEnv, KnowsWonDrawnAndLostPositions) {
    if (tb_path().empty())
        GTEST_SKIP() << "set HAVOC_SYZYGY_PATH to run";
    ASSERT_TRUE(tablebase::init(tb_path()));

    struct Case {
        const char* fen;
        int wdl;
        const char* what;
    };
    const Case cases[] = {
        {"8/8/8/8/8/4k3/8/R3K3 w - - 0 1", 1, "KR vs K is won for the rook"},
        {"8/8/8/8/8/4k3/8/r3K3 w - - 0 1", -1, "the same position with the rook reversed is lost"},
        {"8/8/8/8/8/4k3/8/4K3 w - - 0 1", 0, "bare kings are drawn"},
        {"8/8/8/8/8/2nk4/8/4K3 w - - 0 1", 0, "K vs KN is drawn"},
        // A rook pawn with the defending king in front is the textbook draw,
        // and the position an evaluation is most likely to get wrong.
        {"8/8/8/8/8/k7/p7/K7 w - - 0 1", 0, "KP vs K with the king trapped in front is drawn"},
    };

    for (const auto& c : cases) {
        const position p = make_position(c.fen);
        EXPECT_EQ(tablebase::probe_wdl(p), c.wdl) << c.what << " -- " << c.fen;
    }
}

TEST_F(TablebaseEnv, RefusesPositionsTheTablesDoNotDescribe) {
    if (tb_path().empty())
        GTEST_SKIP() << "set HAVOC_SYZYGY_PATH to run";
    ASSERT_TRUE(tablebase::init(tb_path()));

    // Castling rights: the tables do not encode them, so a position holding
    // any is a different position from the one the table describes.
    const position castling = make_position("4k3/8/8/8/8/8/8/R3K3 w Q - 0 1");
    EXPECT_EQ(tablebase::probe_wdl(castling), tablebase::kProbeFailed);

    // A non-zero fifty-move counter: the tables assume no progress has been
    // made toward the draw, and a win they report may already be unreachable.
    const position halfway = make_position("8/8/8/8/8/4k3/8/R3K3 w - - 30 40");
    EXPECT_EQ(tablebase::probe_wdl(halfway), tablebase::kProbeFailed);

    // Too many pieces for any table that exists.
    const position full =
        make_position("rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1");
    EXPECT_EQ(tablebase::probe_wdl(full), tablebase::kProbeFailed);
}

TEST_F(TablebaseEnv, VerdictsAreFromTheSideToMove) {
    if (tb_path().empty())
        GTEST_SKIP() << "set HAVOC_SYZYGY_PATH to run";
    ASSERT_TRUE(tablebase::init(tb_path()));

    // The same board, both sides to move. Whoever owns the rook is winning, so
    // the sign has to follow the side to move rather than the colour. Getting
    // this backwards would make the search walk into lost endgames on purpose,
    // and it would not show up in any position where the answer is a draw.
    const position white_to_move = make_position("8/8/8/8/8/4k3/8/R3K3 w - - 0 1");
    const position black_to_move = make_position("8/8/8/8/8/8/4k3/R3K3 b - - 0 1");
    EXPECT_EQ(tablebase::probe_wdl(white_to_move), 1);
    EXPECT_EQ(tablebase::probe_wdl(black_to_move), -1);
}

TEST_F(TablebaseEnv, ReloadingReplacesThepreviousTables) {
    if (tb_path().empty())
        GTEST_SKIP() << "set HAVOC_SYZYGY_PATH to run";
    ASSERT_TRUE(tablebase::init(tb_path()));
    ASSERT_TRUE(tablebase::available());

    // Setting an empty path has to actually unload. If it did not, an operator
    // who cleared SyzygyPath would keep probing tables they thought were gone.
    EXPECT_TRUE(tablebase::init(""));
    EXPECT_FALSE(tablebase::available());

    EXPECT_TRUE(tablebase::init(tb_path()));
    EXPECT_TRUE(tablebase::available());
}
