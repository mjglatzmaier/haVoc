#include "havoc/bitboard.hpp"
#include "havoc/kpk.hpp"
#include "havoc/magics.hpp"
#include "havoc/movegen.hpp"
#include "havoc/position.hpp"
#include "havoc/zobrist.hpp"

#include <bit>
#include <sstream>
#include <string>
#include <map>
#include <random>
#include <vector>
#include <set>
#include <algorithm>

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
    // The black king is on a8 only to make the position legal. Without it
    // king_square(black) returns no_square, which is 65, and pinned() indexes
    // bitboards::battks[65] -- a global buffer overflow that UBSan and ASan both
    // flag. It is off the board for every real game, but it kept the sanitizer
    // build from getting past this test.
    EXPECT_LT(see_of("k3r3/8/8/8/8/8/4r3/4K3 w - - 0 1", E1, E2), 0);
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

// The material key must identify material and nothing else. The material table
// caches an entry per key and validates it by comparing the full key, so any
// two positions sharing a key are treated as having identical material: the
// cached material score, phase interpolant and endgame classification of the
// first are handed to the second.
//
// This used to fail in both directions. mkey was XORed with the square-indexed
// zobrist::piece(square, colour, piece) in add_piece and remove_piece, exactly
// as the position key is, but do_quiet updates the position key and
// deliberately leaves mkey alone -- quiet moves do not change material. So a
// piece was registered in the key at its *starting* square and, when captured
// later somewhere else, unregistered at the square it happened to die on. The
// two terms did not cancel: the key kept a stale term for the origin and
// gained a spurious one for the grave.
//
// The result was a key that tracked capture squares rather than material.
// Identical material reached by different move orders produced different keys,
// and -- because two pieces of the same colour and type captured on the same
// square contribute the identical term twice, which cancels -- genuinely
// different material could produce the *same* key. Over the sample below the
// old scheme produced 6,167 keys for 4,238 distinct materials, with 19 outright
// collisions between different materials.
TEST_F(PositionTest, MaterialKeyIdentifiesMaterialAndNothingElse) {
    std::mt19937 rng(4242u);
    std::map<std::string, U64> key_of_material;
    std::map<U64, std::string> material_of_key;
    long positions = 0, same_material_different_key = 0, same_key_different_material = 0;

    for (int game = 0; game < 60; ++game) {
        std::istringstream ss("rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1");
        position p(ss);
        for (int ply = 0; ply < 120; ++ply) {
            Movegen mv(p);
            mv.generate<pseudo_legal, pieces>();
            std::vector<Move> legal;
            for (int i = 0; i < mv.size(); ++i)
                if (p.is_legal(mv[i]))
                    legal.push_back(mv[i]);
            if (legal.empty())
                break;
            p.do_move(legal[rng() % legal.size()]);

            std::string material;
            for (int c = 0; c < 2; ++c)
                for (int pc = pawn; pc <= king; ++pc)
                    material += static_cast<char>('0' + p.number_of(Color(c), Piece(pc)));
            const U64 k = p.material_key();
            ++positions;

            auto a = key_of_material.find(material);
            if (a == key_of_material.end())
                key_of_material[material] = k;
            else if (a->second != k)
                ++same_material_different_key;

            auto b = material_of_key.find(k);
            if (b == material_of_key.end())
                material_of_key[k] = material;
            else if (b->second != material)
                ++same_key_different_material;
        }
    }

    EXPECT_GT(positions, 3000) << "the random walk did not cover enough ground to mean anything";
    EXPECT_EQ(same_key_different_material, 0)
        << "two different materials share a key, so the material table will hand one "
           "position the cached score, phase and endgame type of the other";
    EXPECT_EQ(same_material_different_key, 0)
        << "identical material produced different keys, so the material table caches "
           "the same entry many times over and thrashes";
    EXPECT_EQ(key_of_material.size(), material_of_key.size())
        << "the material key must be a bijection with material";
}

// Every incrementally maintained key must equal the key a fresh position
// computes from scratch for the same board. The keys are updated by hand in
// do_quiet, add_piece, remove_piece and set, each guarded by its own
// condition, and a missed or mismatched guard is invisible until it silently
// corrupts whatever cache the key feeds -- exactly how the material key came
// to identify capture squares rather than material.
//
// Rebuilding from the FEN is the independent oracle: it exercises set() only,
// with no incremental path at all, so agreement across a random walk means the
// incremental updates compose to the same function as a from-scratch build.
// Promotions and en passant are the interesting cases, since both move a pawn
// off a square without a pawn arriving on it.
TEST_F(PositionTest, IncrementalKeysAgreeWithKeysBuiltFromScratch) {
    std::mt19937 rng(90210u);
    long positions = 0, promotions = 0, en_passants = 0;

    for (int game = 0; game < 40; ++game) {
        std::istringstream ss("rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1");
        position p(ss);
        for (int ply = 0; ply < 140; ++ply) {
            Movegen mv(p);
            mv.generate<pseudo_legal, pieces>();
            std::vector<Move> legal;
            for (int i = 0; i < mv.size(); ++i)
                if (p.is_legal(mv[i]))
                    legal.push_back(mv[i]);
            if (legal.empty())
                break;

            const Move m = legal[rng() % legal.size()];
            if (m.type <= capture_promotion_n)
                ++promotions;
            if (m.type == ep)
                ++en_passants;
            p.do_move(m);
            ++positions;

            const std::string fen = p.to_fen();
            std::istringstream rebuilt_ss(fen);
            position rebuilt(rebuilt_ss);

            ASSERT_EQ(p.key(), rebuilt.key()) << "position key drifted at " << fen;
            ASSERT_EQ(p.pawnkey(), rebuilt.pawnkey()) << "pawn key drifted at " << fen;
            ASSERT_EQ(p.material_key(), rebuilt.material_key())
                << "material key drifted at " << fen;
            ASSERT_EQ(p.repkey(), rebuilt.repkey()) << "repetition key drifted at " << fen;
        }
    }

    EXPECT_GT(positions, 2000);
    EXPECT_GT(promotions, 0) << "the walk never promoted, so it never tested the case "
                                "where a pawn leaves the board without one arriving";
    EXPECT_GT(en_passants, 0) << "the walk never played en passant, so it never tested "
                                 "a capture on a square the captured pawn is not on";
}

namespace {
/// Plays the move with the given origin and destination, and fails if no such
/// legal move exists.
bool play(position& p, Square from, Square to) {
    Movegen mv(p);
    mv.generate<pseudo_legal, pieces>();
    for (int i = 0; i < mv.size(); ++i)
        if (mv[i].f == from && mv[i].t == to && p.is_legal(mv[i])) {
            p.do_move(mv[i]);
            return true;
        }
    return false;
}
} // namespace

// A repetition is a repetition however many moves it took to come back.
//
// is_draw() walks the history two plies at a time, which is right, because only
// positions with the same side to move can repeat. But the side-to-move term
// was XORed into the key on every move without the outgoing side being XORed
// back out, so it accumulated rather than toggled and had a period of four
// plies instead of two. Every candidate the walk examined at a distance of two
// plies mod four therefore differed by a constant and could not match, no
// matter what was on the board.
//
// The engine consequently saw repetitions four and eight plies back and was
// structurally blind to those six, ten and fourteen plies back. Rooks
// triangulating home -- a1-b1-c1-a1 against a8-b8-c8-a8 -- repeat after six.
TEST_F(PositionTest, ARepetitionSixPliesBackIsStillARepetition) {
    std::istringstream ss("r3k2r/8/8/8/8/8/8/R3K2R w - - 0 1");
    position p(ss);
    const U64 start_key = p.key();

    const Square path[][2] = {{A1, B1}, {A8, B8}, {B1, C1}, {B8, C8}, {C1, A1}, {C8, A8}};
    for (auto& mv : path) {
        ASSERT_TRUE(play(p, mv[0], mv[1])) << "could not play the manoeuvre";
        if (&mv != &path[5])
            EXPECT_FALSE(p.is_draw()) << "claimed a draw before the position had repeated";
    }

    EXPECT_EQ(p.key(), start_key) << "the same board with the same side to move must hash "
                                     "the same however many plies ago it was last seen";
    EXPECT_TRUE(p.is_draw()) << "the position has occurred twice and was not detected";
}

// Castling rights die with the rook, whether it moves or is taken.
//
// do_move retired rights when the king or the rook moved, but nothing looked at
// the square a capture landed on, so taking a rook on its home square left the
// owner still nominally able to castle with it. The rights mask, the key and
// the FEN all went on describing a rook that was no longer on the board, and
// move generation kept emitting the castle for is_legal to throw away.
TEST_F(PositionTest, CapturingARookOnItsHomeSquareRetiresTheCastlingRight) {
    std::istringstream ss("4k2r/8/6N1/8/8/8/8/4K3 w k - 0 1");
    position p(ss);
    ASSERT_TRUE(p.can_castle_ks<black>());

    ASSERT_TRUE(play(p, G6, H8)) << "Nxh8 should be legal";

    EXPECT_FALSE(p.can_castle_ks<black>())
        << "black kept the kingside right after the h8 rook was captured";
    std::istringstream fen_fields(p.to_fen());
    std::string board, stm, castling;
    fen_fields >> board >> stm >> castling;
    EXPECT_EQ(castling, "-")
        << "the FEN still advertises a castling right for a rook that is gone: " << p.to_fen();

    Movegen mv(p);
    mv.generate<pseudo_legal, pieces>();
    for (int i = 0; i < mv.size(); ++i)
        EXPECT_NE(mv[i].type, castle_ks) << "still generating a castle with no rook to castle with";

    // And the key must agree with a position built from scratch, which is what
    // makes the stale right corrupt transposition lookups rather than merely
    // look untidy.
    std::istringstream rebuilt_ss(p.to_fen());
    position rebuilt(rebuilt_ss);
    EXPECT_EQ(p.key(), rebuilt.key());
}


// ─────────────────────────────────────────────────────────────────────────────
// Zobrist randoms must actually be random.
//
// The legacy table was 1835 hand-rolled constants that all fitted in 32 bits
// and had a Hamming weight of at most 4. They spanned a GF(2) space of only
// 32 dimensions, which saturated the table with short linear dependencies:
// 1715 triples XORed to zero and 400785 quadruples satisfied a ^ b == c ^ d.
//
// A four-term dependency is a key collision between two positions differing by
// two pieces, and three of the old ones used a single piece type, so they
// collided positions with identical material reachable in one game: two white
// knights on a2 and b2 hashed exactly like the same knights on h4 and d6.
// is_draw() compares repetition keys for equality, so that pair fabricates a
// repetition that never happened.
//
// This test guards the properties the old table lacked.
// ─────────────────────────────────────────────────────────────────────────────
TEST_F(PositionTest, ZobristRandomsAreStatisticallyIndependent) {
    std::vector<havoc::U64> rands;
    for (int sq = havoc::A1; sq <= havoc::H8; ++sq)
        for (int c = havoc::white; c <= havoc::black; ++c)
            for (int p = havoc::pawn; p <= havoc::king; ++p)
                rands.push_back(havoc::zobrist::piece(havoc::Square(sq), havoc::Color(c),
                                                      havoc::Piece(p)));
    for (int c = havoc::white; c <= havoc::black; ++c)
        for (havoc::U16 r = 0; r < 16; ++r)
            rands.push_back(havoc::zobrist::castle(havoc::Color(c), r));
    for (int f = 0; f < 8; ++f)
        rands.push_back(havoc::zobrist::ep(f));
    rands.push_back(havoc::zobrist::stm(havoc::white));
    rands.push_back(havoc::zobrist::stm(havoc::black));

    // Every value distinct.
    std::set<havoc::U64> distinct(rands.begin(), rands.end());
    EXPECT_EQ(distinct.size(), rands.size()) << "duplicate zobrist randoms";

    // Full 64-bit range, balanced bits. The old table averaged 3.84.
    long total_bits = 0;
    int min_pc = 64;
    for (auto v : rands) {
        int pc = std::popcount(v);
        total_bits += pc;
        min_pc = std::min(min_pc, pc);
    }
    const double mean_pc = double(total_bits) / double(rands.size());
    EXPECT_GT(mean_pc, 24.0) << "zobrist randoms are too sparse, mean popcount " << mean_pc;
    EXPECT_LT(mean_pc, 40.0) << "zobrist randoms are too dense, mean popcount " << mean_pc;
    EXPECT_GE(min_pc, 12) << "a zobrist random has only " << min_pc << " bits set";

    // They must span the full 64-dimensional GF(2) space, not a subspace.
    std::map<int, havoc::U64> basis;
    for (auto v : rands) {
        havoc::U64 x = v;
        for (auto it = basis.rbegin(); it != basis.rend(); ++it)
            x = std::min(x, x ^ it->second);
        if (x != 0ULL)
            basis[63 - std::countl_zero(x)] = x;
    }
    EXPECT_EQ(basis.size(), 64u) << "zobrist randoms span only " << basis.size() << " dimensions";

    // No three or four of them may XOR to zero: those are outright key
    // collisions between positions differing by two or three pieces.
    std::map<havoc::U64, std::vector<size_t>> pair_xor;
    for (size_t i = 0; i < rands.size(); ++i)
        for (size_t j = i + 1; j < rands.size(); ++j)
            pair_xor[rands[i] ^ rands[j]].push_back(i);

    int three_term = 0;
    for (auto v : rands)
        if (pair_xor.count(v) != 0)
            ++three_term;
    EXPECT_EQ(three_term, 0) << three_term << " triples of zobrist randoms XOR to zero";

    long four_term = 0;
    for (const auto& [x, who] : pair_xor)
        if (who.size() > 1)
            four_term += long(who.size()) * long(who.size() - 1) / 2;
    EXPECT_EQ(four_term, 0) << four_term << " quadruples of zobrist randoms satisfy a^b == c^d";
}

// ── FEN validation ──────────────────────────────────────────────────────────
//
// Every string below crashed the engine before setup() validated its input:
// missing kings indexed the 64-entry attack tables with no_square (65), a pawn
// on the first or last rank drove kpk::pawn_index() negative, std::stoi
// terminated the process on a non-numeric counter, and a placement field that
// ran off the board wrote outside the piece bitboards.

namespace {

bool parses(const std::string& fen) {
    std::istringstream ss(fen);
    position p;
    return p.setup(ss);
}

} // namespace

TEST_F(PositionTest, RejectsFenWithoutBothKings) {
    EXPECT_FALSE(parses("8/5ppp/8/8/8/8/5PPP/R5K1 w - - 0 1")) << "no black king";
    EXPECT_FALSE(parses("6k1/5ppp/8/8/8/8/5PPP/R7 w - - 0 1")) << "no white king";
    EXPECT_FALSE(parses("8/8/8/8/8/8/8/8 w - - 0 1")) << "empty board";
    EXPECT_FALSE(parses("4K3/8/8/8/8/8/8/4K3 w - - 0 1")) << "two white kings, no black";
}

TEST_F(PositionTest, RejectsPawnOnFirstOrLastRank) {
    // Drove kpk::pawn_index() to col * 6 - 1, indexing the bitbase negatively.
    EXPECT_FALSE(parses("4k3/8/8/8/8/8/8/4K2P w - - 0 1")) << "white pawn on rank 1";
    EXPECT_FALSE(parses("p7/8/8/8/8/8/8/4K2k w - - 0 1")) << "black pawn on rank 8";
    EXPECT_FALSE(parses("P7/8/8/8/8/8/8/4K2k w - - 0 1")) << "white pawn on rank 8";
    EXPECT_FALSE(parses("4k3/8/8/8/8/8/8/4K2p w - - 0 1")) << "black pawn on rank 1";
}

TEST_F(PositionTest, TreatsNonNumericCountersAsEpdOperations) {
    // Published EPD suites put operations exactly where the counters go, and
    // std::stoi used to terminate the process on them. The position is fully
    // determined without the counters, so it parses and they keep their
    // defaults.
    EXPECT_TRUE(parses("2rr3k/pp3pp1/1nnqbN1p/3pN3/2pP4/2P3Q1/PPB4P/R4RK1 w - - bm Qg6;"));
    EXPECT_TRUE(parses("rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - abc 1"));
    EXPECT_TRUE(parses("rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 xyz"));
    EXPECT_TRUE(parses("rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 99999999999999 1"));

    // A counter that is present and valid is still read.
    std::istringstream ss("4k3/8/8/8/8/8/8/4K3 w - - 17 42");
    position p;
    ASSERT_TRUE(p.setup(ss));
    EXPECT_NE(p.to_fen().find(" 17 42"), std::string::npos) << p.to_fen();
}

TEST_F(PositionTest, RejectsPlacementThatRunsOffTheBoard) {
    EXPECT_FALSE(parses("9999999/8/8/8/8/8/8/4K3 w - - 0 1")) << "digit past h1";
    EXPECT_FALSE(parses("/////// w - - 0 1")) << "rewinds past a1";
}

TEST_F(PositionTest, AcceptsLegalFens) {
    EXPECT_TRUE(parses("rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1"));
    EXPECT_TRUE(parses("r3k2r/p1ppqpb1/bn2pnp1/3PN3/1p2P3/2N2Q1p/PPPBBPPP/R3K2R w KQkq - 0 1"));
    EXPECT_TRUE(parses("8/2p5/3p4/KP5r/1R3p1k/8/4P1P1/8 w - - 0 1"));
    EXPECT_TRUE(parses("rnbqkbnr/pp1ppppp/8/2p5/4P3/8/PPPP1PPP/RNBQKBNR w KQkq c6 0 2"));
    EXPECT_TRUE(parses("4k3/8/8/8/8/8/4P3/4K3 w - - 0 1"));
    // Counters are optional in EPD-style strings.
    EXPECT_TRUE(parses("4k3/8/8/8/8/8/8/4K3 w - -"));
}

TEST_F(PositionTest, RejectedFenLeavesAnEmptyBoardNotAHalfParsedOne) {
    std::istringstream ss("8/5ppp/8/8/8/8/5PPP/R5K1 w - - 0 1");
    position p;
    ASSERT_FALSE(p.setup(ss));
    EXPECT_EQ(p.get_pieces<white>(), 0ULL);
    EXPECT_EQ(p.get_pieces<black>(), 0ULL);
}

} // namespace havoc
