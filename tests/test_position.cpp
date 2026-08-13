#include "havoc/bitboard.hpp"
#include "havoc/kpk.hpp"
#include "havoc/magics.hpp"
#include "havoc/movegen.hpp"
#include "havoc/position.hpp"
#include "havoc/zobrist.hpp"

#include <sstream>
#include <string>

#include <gtest/gtest.h>

namespace havoc {

class PositionTest : public ::testing::Test {
  protected:
    void SetUp() override {
        bitboards::init();
        magics::init();
        zobrist::init();
        kpk::init();
    }
};

// ── FEN round-trip ──────────────────────────────────────────────────────────

TEST_F(PositionTest, FenRoundTrip_Startpos) {
    const std::string fen_str = "rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1";
    std::istringstream iss1(fen_str);
    position pos1(iss1);
    std::string out = pos1.to_fen();

    std::istringstream iss2(out);
    position pos2(iss2);
    EXPECT_EQ(pos1.key(), pos2.key());
    EXPECT_EQ(pos1.to_fen(), pos2.to_fen());
}

TEST_F(PositionTest, FenRoundTrip_Kiwipete) {
    const std::string fen_str =
        "r3k2r/p1ppqpb1/bn2pnp1/3PN3/1p2P3/2N2Q1p/PPPBBPPP/R3K2R w KQkq - 0 1";
    std::istringstream iss1(fen_str);
    position pos1(iss1);
    std::string out = pos1.to_fen();

    std::istringstream iss2(out);
    position pos2(iss2);
    EXPECT_EQ(pos1.key(), pos2.key());
    EXPECT_EQ(pos1.to_fen(), pos2.to_fen());
}

// ── do_move / undo_move preserves state ─────────────────────────────────────

// ── the transposition key identifies a position, not a path ─────────────────
//
// move50 and hmvs used to be XORed into ifo.key. They were also never XORed
// back out when they changed, so the key accumulated a term for every value
// the counters took along the path, making it a function of the route rather
// than of the position. Two routes to one position therefore probed different
// entries and never transposed.

namespace {
// Plays a move given in coordinate notation. Only needs to handle the quiet
// knight moves these tests use.
void play(position& pos, const std::string& uci) {
    Movegen mvs(pos);
    mvs.generate<pseudo_legal, pieces>();
    for (int i = 0; i < mvs.size(); ++i) {
        if (!pos.is_legal(mvs[i]))
            continue;
        std::string got;
        got += static_cast<char>('a' + static_cast<int>(util::col(mvs[i].f)));
        got += static_cast<char>('1' + static_cast<int>(util::row(mvs[i].f)));
        got += static_cast<char>('a' + static_cast<int>(util::col(mvs[i].t)));
        got += static_cast<char>('1' + static_cast<int>(util::row(mvs[i].t)));
        if (got == uci) {
            pos.do_move(mvs[i]);
            return;
        }
    }
    ADD_FAILURE() << "no legal move " << uci;
}
} // namespace

TEST_F(PositionTest, KeyIsIndependentOfTheRouteTakenToThePosition) {
    const std::string start = "rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1";

    std::istringstream f1(start);
    position direct(f1);

    // Same board, reached by walking the knights out and back. Nothing is
    // captured and no pawn moves, so the two differ only in how many
    // reversible moves are behind them: move50 is 0 here and 4 there.
    std::istringstream f2(start);
    position wandered(f2);
    play(wandered, "g1f3");
    play(wandered, "g8f6");
    play(wandered, "f3g1");
    play(wandered, "f6g8");

    ASSERT_EQ(direct.to_fen().substr(0, direct.to_fen().find(' ')),
              wandered.to_fen().substr(0, wandered.to_fen().find(' ')))
        << "the two routes must actually reach the same board";

    EXPECT_EQ(direct.key(), wandered.key())
        << "the same position reached two ways must probe the same TT entry";
}

TEST_F(PositionTest, KeyIsIndependentOfMoveOrder) {
    const std::string start = "rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1";

    std::istringstream f1(start);
    position a(f1);
    play(a, "g1f3");
    play(a, "g8f6");
    play(a, "b1c3");
    play(a, "b8c6");

    std::istringstream f2(start);
    position b(f2);
    play(b, "b1c3");
    play(b, "b8c6");
    play(b, "g1f3");
    play(b, "g8f6");

    EXPECT_EQ(a.key(), b.key()) << "transposed move orders must share a key";
}

TEST_F(PositionTest, DoUndo_PreservesKey) {
    std::istringstream fen("rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1");
    position pos(fen);
    U64 key_before = pos.repkey();

    Movegen mvs(pos);
    mvs.generate<pseudo_legal, pieces>();

    for (int i = 0; i < mvs.size(); ++i) {
        if (!pos.is_legal(mvs[i]))
            continue;
        pos.do_move(mvs[i]);
        pos.undo_move(mvs[i]);
        EXPECT_EQ(pos.repkey(), key_before) << "repkey mismatch after do/undo move " << i;
    }
}

TEST_F(PositionTest, DoUndo_PreservesAllPieces) {
    std::istringstream fen("r3k2r/p1ppqpb1/bn2pnp1/3PN3/1p2P3/2N2Q1p/PPPBBPPP/R3K2R w KQkq - 0 1");
    position pos(fen);
    U64 pieces_before = pos.all_pieces();

    Movegen mvs(pos);
    mvs.generate<pseudo_legal, pieces>();

    for (int i = 0; i < mvs.size(); ++i) {
        if (!pos.is_legal(mvs[i]))
            continue;
        pos.do_move(mvs[i]);
        pos.undo_move(mvs[i]);
        EXPECT_EQ(pos.all_pieces(), pieces_before) << "pieces mismatch after do/undo move " << i;
    }
}

// ── null move do/undo ───────────────────────────────────────────────────────

TEST_F(PositionTest, NullMove_PreservesState) {
    std::istringstream fen("rnbqkbnr/pppppppp/8/8/4P3/8/PPPP1PPP/RNBQKBNR b KQkq e3 0 1");
    position pos(fen);
    U64 key_before = pos.repkey();
    Color stm_before = pos.to_move();

    pos.do_null_move();
    EXPECT_NE(pos.to_move(), stm_before);
    pos.undo_null_move();

    EXPECT_EQ(pos.to_move(), stm_before);
    EXPECT_EQ(pos.repkey(), key_before);
}

// ── is_draw: repetition ─────────────────────────────────────────────────────

TEST_F(PositionTest, IsDraw_Repetition) {
    std::istringstream fen("rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1");
    position pos(fen);

    // Ng1-f3
    Move m1(G1, F3, quiet);
    // Ng8-f6
    Move m2(G8, F6, quiet);
    // Nf3-g1
    Move m3(F3, G1, quiet);
    // Nf6-g8
    Move m4(F6, G8, quiet);

    EXPECT_FALSE(pos.is_draw());

    pos.do_move(m1);
    pos.do_move(m2);
    pos.do_move(m3);
    pos.do_move(m4);

    // Position repeats — should be draw
    EXPECT_TRUE(pos.is_draw());
}

// ── is_draw: 50-move rule ───────────────────────────────────────────────────

TEST_F(PositionTest, IsDraw_50MoveRule) {
    // Both sides keep a rook. A bare king-versus-king position would now be
    // drawn on insufficient material regardless of the counter, which would stop
    // this test from saying anything about the fifty move rule.
    std::istringstream fen("r7/8/8/4k3/8/8/8/R3K3 w - - 99 100");
    position pos(fen);
    EXPECT_FALSE(pos.is_draw());

    // One more quiet move -> move50 becomes 100
    Move m(E1, D1, quiet);
    pos.do_move(m);
    EXPECT_TRUE(pos.is_draw());
}

// ── in_check ────────────────────────────────────────────────────────────────

TEST_F(PositionTest, InCheck_No) {
    std::istringstream fen("rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1");
    position pos(fen);
    EXPECT_FALSE(pos.in_check());
}

TEST_F(PositionTest, InCheck_Yes) {
    std::istringstream fen("rnbqkbnr/ppppp1pp/8/5p1Q/4P3/8/PPPP1PPP/RNB1KBNR b KQkq - 1 2");
    position pos(fen);
    EXPECT_TRUE(pos.in_check());
}

// ── piece counts ────────────────────────────────────────────────────────────

TEST_F(PositionTest, PieceCounts_Startpos) {
    std::istringstream fen("rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1");
    position pos(fen);

    EXPECT_EQ(pos.number_of(white, pawn), 8u);
    EXPECT_EQ(pos.number_of(black, pawn), 8u);
    EXPECT_EQ(pos.number_of(white, knight), 2u);
    EXPECT_EQ(pos.number_of(black, knight), 2u);
    EXPECT_EQ(pos.number_of(white, bishop), 2u);
    EXPECT_EQ(pos.number_of(black, bishop), 2u);
    EXPECT_EQ(pos.number_of(white, rook), 2u);
    EXPECT_EQ(pos.number_of(black, rook), 2u);
    EXPECT_EQ(pos.number_of(white, queen), 1u);
    EXPECT_EQ(pos.number_of(black, queen), 1u);
    EXPECT_EQ(pos.number_of(white, king), 1u);
    EXPECT_EQ(pos.number_of(black, king), 1u);
}

TEST_F(PositionTest, KingSquare_Startpos) {
    std::istringstream fen("rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1");
    position pos(fen);
    EXPECT_EQ(pos.king_square(white), E1);
    EXPECT_EQ(pos.king_square(black), E8);
}


// ── Static exchange evaluation ──────────────────────────────────────────────

namespace {

// Generate the position's moves and return the one matching from/to. Using the
// generator rather than a hand-built Move keeps the movetype (ep, capture,
// capture-promotion) consistent with what the search actually passes to see().
Move find_move(position& pos, Square from, Square to) {
    Movegen mvs(pos);
    mvs.generate<pseudo_legal, pieces>();
    for (int i = 0; i < mvs.size(); ++i) {
        if (mvs[i].f == static_cast<U8>(from) && mvs[i].t == static_cast<U8>(to))
            return mvs[i];
    }
    return Move{};
}

int see_of(const std::string& fen_str, Square from, Square to) {
    std::istringstream fen(fen_str);
    position pos(fen);
    const Move m = find_move(pos, from, to);
    EXPECT_NE(m.type, static_cast<U8>(no_type)) << "move not generated for " << fen_str;
    return pos.see(m);
}

} // namespace

TEST_F(PositionTest, SeeWinsAnUndefendedPawn) {
    // exd5 with nothing defending d5.
    EXPECT_EQ(see_of("4k3/8/8/3p4/4P3/8/8/4K3 w - - 0 1", E4, D5), 100);
}

TEST_F(PositionTest, SeeEqualPawnTradeIsZero) {
    // exd5 cxd5.
    EXPECT_EQ(see_of("4k3/8/2p5/3p4/4P3/8/8/4K3 w - - 0 1", E4, D5), 0);
}

TEST_F(PositionTest, SeeQueenTakesDefendedPawn) {
    // Qxd5 cxd5 loses a queen for a pawn.
    EXPECT_EQ(see_of("4k3/8/2p5/3p4/8/8/8/3QK3 w - - 0 1", D1, D5), 100 - 910);
}

// The two standard exchange positions from the Chess Programming Wiki.

TEST_F(PositionTest, SeeCpwRookTakesUndefendedPawn) {
    EXPECT_EQ(see_of("1k1r4/1pp4p/p7/4p3/8/P5P1/1PP4P/2K1R3 w - - 0 1", E1, E5), 100);
}

TEST_F(PositionTest, SeeCpwKnightTakesDefendedPawn) {
    // Nxe5 Nxe5 Rxe5 Bxe5 Qxe5 Qxe5, with the queen on h8 x-raying through the
    // bishop on f6.
    EXPECT_EQ(see_of("1k1r3q/1ppn3p/p4b2/4p3/8/P2N2P1/1PP1R1BP/2K1Q3 w - - 0 1", D3, E5),
              100 - 300);
}

TEST_F(PositionTest, SeeEvaluatesEnPassant) {
    // exd6 e.p. removes the pawn on d5; nothing recaptures.
    EXPECT_EQ(see_of("4k3/8/8/3pP3/8/8/8/4K3 w - d6 0 2", E5, D6), 100);
}

TEST_F(PositionTest, SeeWorksWhileInCheck) {
    // White is in check from the queen on e2 and simply wins it with the rook.
    EXPECT_EQ(see_of("4k3/8/8/8/8/4R3/4q3/4K3 w - - 0 1", E3, E2), 910);
}

TEST_F(PositionTest, SeeRefusesKingCaptureOfADefendedPiece) {
    // Kxe2 is not actually available: the rook on e8 recaptures.
    EXPECT_LT(see_of("4r3/8/8/8/8/8/4r3/4K3 w - - 0 1", E1, E2), 0);
}

// Capture-promotions are the one path in see() that changes both the value of
// the initial gain and the piece left standing on the target square, and they
// were the only move type it handles specially that had no test.

TEST_F(PositionTest, SeeCapturePromotionWinsTheRookAndKeepsTheQueen) {
    // exd8=Q takes a rook and promotes, and nothing can recapture on d8.
    // Gained: the rook, plus a queen in place of the pawn.
    EXPECT_EQ(see_of("3r4/4P3/8/8/4k3/8/8/4K3 w - - 0 1", E7, D8),
              480 + 910 - 100);
}

TEST_F(PositionTest, SeeCapturePromotionAccountsForLosingTheNewQueen) {
    // exd8=Q Kxd8 wins a rook for a pawn: the promoted queen is captured, so it
    // is the rook minus the pawn that is left, not the queen.
    EXPECT_EQ(see_of("3rk3/4P3/8/8/8/8/8/4K3 w - - 0 1", E7, D8), 480 - 100);
}

TEST_F(PositionTest, RecognisesDeadDrawnMaterial) {
    auto material_draw = [](const std::string& fen) {
        std::istringstream ss(fen);
        position p(ss);
        return p.is_material_draw();
    };

    // No sequence of legal moves mates in any of these.
    EXPECT_TRUE(material_draw("8/8/4k3/8/8/4K3/8/8 w - - 0 1"));    // K vs K
    EXPECT_TRUE(material_draw("8/8/4k3/8/8/3BK3/8/8 w - - 0 1"));   // KB vs K
    EXPECT_TRUE(material_draw("8/8/4k3/8/8/3NK3/8/8 w - - 0 1"));   // KN vs K
    EXPECT_TRUE(material_draw("8/8/2b1k3/8/8/3NK3/8/8 w - - 0 1")); // KN vs KB
    // Two knights cannot force mate, and no arbiter will ever award it.
    EXPECT_TRUE(material_draw("8/8/4k3/8/8/2NNK3/8/8 w - - 0 1")); // KNN vs K

    // These are genuinely winnable and must not be written off.
    EXPECT_FALSE(material_draw("8/8/4k3/8/8/3BK3/4P3/8 w - - 0 1")); // KBP vs K
    EXPECT_FALSE(material_draw("8/8/4k3/8/8/3BKB2/8/8 w - - 0 1"));  // KBB vs K
    EXPECT_FALSE(material_draw("8/8/4k3/8/8/3NKB2/8/8 w - - 0 1"));  // KBN vs K
    EXPECT_FALSE(material_draw("8/8/4k3/8/8/3RK3/8/8 w - - 0 1"));   // KR vs K
    EXPECT_FALSE(material_draw("8/8/4k3/8/8/3QK3/8/8 w - - 0 1"));   // KQ vs K
    EXPECT_FALSE(material_draw("8/8/4k3/8/8/4K3/4P3/8 w - - 0 1"));  // KP vs K
}

} // namespace havoc
