#include "havoc/bitboard.hpp"
#include "havoc/kpk.hpp"
#include "havoc/book.hpp"
#include "havoc/eval/hce.hpp"
#include "havoc/magics.hpp"
#include "havoc/movegen.hpp"
#include "havoc/material_table.hpp"
#include "havoc/parameters.hpp"
#include "havoc/pawn_table.hpp"
#include "havoc/position.hpp"
#include "havoc/tablebase.hpp"
#include "havoc/tt.hpp"
#include "havoc/uci.hpp"
#include "havoc/zobrist.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <random>
#include <sstream>
#include <string>

#include "mirror.hpp"

#include <gtest/gtest.h>
#include <iostream>
#include <set>
#include <vector>

namespace {

/// Helper to build a position from a FEN string.
havoc::position make_pos(const std::string& fen) {
    std::istringstream iss(fen);
    return havoc::position(iss);
}

/// Mirror a FEN vertically and swap the colors of every piece.
///
/// The evaluation is defined from the point of view of the side to move, so a
/// position and its mirror describe exactly the same problem seen from the
/// other side, and a correct evaluator has to return an identical score for
/// both. Any difference at all is a color-indexing bug: a table indexed with
/// the wrong color, a shift that only works for white, a relative rank
/// computed from the wrong end of the board.
using havoc::testing::mirror_fen;

/// A spread of positions chosen to exercise every category in the evaluation:
/// openings, sharp middlegames, king attacks, pawn structures, passed pawns,
/// opposite-colored bishops and a range of endgames.
const std::vector<std::string>& mirror_test_positions() {
    static const std::vector<std::string> fens = {
        "rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1",
        "r3k2r/p1ppqpb1/bn2pnp1/3PN3/1p2P3/2N2Q1p/PPPBBPPP/R3K2R w KQkq - 0 1",
        "8/2p5/3p4/KP5r/1R3p1k/8/4P1P1/8 w - - 0 1",
        "r3k2r/Pppp1ppp/1b3nbN/nP6/BBP1P3/q4N2/Pp1P2PP/R2Q1RK1 w kq - 0 1",
        "rnbq1k1r/pp1Pbppp/2p5/8/2B5/8/PPP1NnPP/RNBQK2R w KQ - 1 8",
        "r4rk1/1pp1qppp/p1np1n2/2b1p1B1/2B1P1b1/3P1N1P/PPP1NPP1/R2Q1RK1 w - - 0 1",
        "r1bqkb1r/pppppppp/2n2n2/8/3PP3/8/PPP2PPP/RNBQKBNR w KQkq - 2 3",
        "r2q1rk1/ppp2ppp/2n1bn2/2b1p3/3pP3/3P1N1P/PPP1BPP1/RNBQR1K1 w - - 0 8",
        "2rr2k1/pp3ppp/2n1bn2/2q1p3/8/1NP2N1P/PP3PP1/R1BQR1K1 w - - 5 14",
        // Opposite-colored bishops, pieces still on
        "r2q1rk1/1p2bppp/2n2n2/3p4/3P4/2N2N2/PP2BPPP/R2Q1RK1 w - - 0 1",
        // Pure opposite-colored bishop ending
        "8/pp3p2/2b1k3/8/1P6/2B1K3/P4P2/8 w - - 0 1",
        // Passed pawns and a rook ending
        "8/3k4/8/2P5/8/4K3/6p1/8 w - - 0 1",
        "8/1R6/5pk1/8/6P1/5K2/1r6/8 w - - 0 1",
        // Wrecked structure: doubled, isolated and backward pawns
        "r1b2rk1/pp3ppp/2p5/2p5/8/2P2P2/PP4PP/R1B2RK1 w - - 0 1",
        // Exposed king with heavy pieces bearing down on it
        "6k1/5ppp/8/8/8/8/5PPP/2R2RK1 w - - 0 1",
        "r1bq1rk1/pp3ppp/2n1pn2/2pp4/1b1P4/2NBPN2/PPQ2PPP/R1B2RK1 w - - 0 1",
        // En passant available, to exercise the ep mirroring too
        "rnbqkbnr/ppp1p1pp/8/3pPp2/8/8/PPPP1PPP/RNBQKBNR w KQkq f6 0 3",
        // Knight outposts and a closed centre
        "r1bq1rk1/pp2ppbp/2np1np1/2p5/2P1P3/2NP1NP1/PP3PBP/R1BQ1RK1 w - - 0 1",
    };
    return fens;
}

/// Fixture that initializes tables once for all tests.
class EvalTest : public ::testing::Test {
  protected:
    static void SetUpTestSuite() {
        havoc::bitboards::init();
        havoc::magics::init();
        havoc::zobrist::init();
        havoc::kpk::init();
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

// ─── Evaluation must be a function of the position ─────────────────────────

// has_castled is set only by do_move when a castling move is played, and is
// never derived from a FEN. It is not part of the zobrist key either. So the
// same position, with the same key, evaluates differently depending on how it
// was reached -- and the transposition table, which is keyed on that key,
// hands one path's score to the other.
//
// This walks a king and rook from e1/h1 to g1/f1 the long way round, which
// reaches exactly the position kingside castling reaches, and compares it
// against the castled path.
TEST_F(EvalTest, EvaluationDoesNotDependOnHowThePositionWasReached) {
    havoc::parameters params;
    havoc::pawn_table pt(params);
    havoc::material_table mt(params);
    havoc::HCEEvaluator eval(pt, mt, params);

    auto play = [](havoc::position& pos, const std::string& uci) {
        havoc::Movegen mvs(pos);
        mvs.generate<havoc::pseudo_legal, havoc::pieces>();
        for (int i = 0; i < mvs.size(); ++i) {
            if (!pos.is_legal(mvs[i]))
                continue;
            if (havoc::uci::move_to_string(mvs[i]) == uci) {
                pos.do_move(mvs[i]);
                return true;
            }
        }
        return false;
    };

    // Enough material on both sides that is_endgame() stays false, otherwise
    // the endgame specialisation can return before the castling terms run.
    // f2, g2 and d7 are empty so both kings have a route off their back rank.
    const std::string start = "r3k1n1/ppp1pppp/8/8/8/8/PPPPP2P/4K2R w K - 0 1";

    // Path A: castle kingside. King e1 -> g1, rook h1 -> f1.
    auto a = make_pos(start);
    ASSERT_TRUE(play(a, "e1g1"));

    // Path B: the same placement, walked there. The rook has to go first,
    // because once the king stands on g1 the rook can no longer cross it, and
    // the king then has to detour over f2 and g2 for the same reason. Black
    // shuffles on a three-move cycle so that it is Black to move at the end of
    // both paths with its king back on e8.
    auto b = make_pos(start);
    for (const char* mv : {"h1f1", "e8d8", "e1f2", "d8d7", "f2g2", "d7e8", "g2g1"})
        ASSERT_TRUE(play(b, mv)) << "could not play " << mv;

    ASSERT_EQ(a.key(), b.key())
        << "the two paths must reach the same position for this test to mean anything";
    ASSERT_EQ(a.to_move(), b.to_move());

    EXPECT_EQ(eval.evaluate(a), eval.evaluate(b))
        << "the same position evaluates differently depending on the move order "
           "that reached it. Both share a zobrist key, so the transposition "
           "table will serve one of these scores for the other.";
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

// The old symmetry test checked a single position with a 30cp tolerance, which
// is wide enough to hide almost any color bug. Require exact agreement across a
// spread of positions instead.
TEST_F(EvalTest, EvaluationIsExactlyMirrorSymmetric) {
    havoc::parameters params;
    havoc::pawn_table pt(params);
    havoc::material_table mt(params);
    havoc::HCEEvaluator eval(pt, mt, params);

    for (const auto& fen : mirror_test_positions()) {
        auto original = make_pos(fen);
        const std::string mirrored_fen = mirror_fen(fen);
        auto mirrored = make_pos(mirrored_fen);

        // Guard the helper itself: a mirror that dropped or mangled material
        // would make the test vacuous.
        ASSERT_EQ(mirror_fen(mirrored_fen), fen) << "mirror_fen is not an involution on " << fen;

        const int a = eval.evaluate(original);
        const int b = eval.evaluate(mirrored);
        EXPECT_EQ(a, b) << "asymmetric evaluation\n  " << fen << " -> " << a << "\n  "
                        << mirrored_fen << " -> " << b << "\n  difference " << (a - b);
    }
}

// The fixed position list above is a spot check. This walks a few hundred
// positions reached by random legal play and requires the same exact symmetry
// of every one of them, which is what makes it likely to catch the next
// color-indexing mistake rather than the two it already caught.
TEST_F(EvalTest, EvaluationIsMirrorSymmetricOverRandomPlay) {
    havoc::parameters params;
    havoc::pawn_table pt(params);
    havoc::material_table mt(params);
    havoc::HCEEvaluator eval(pt, mt, params);

    std::mt19937 rng(20260812u); // fixed seed: a failure must be reproducible
    int checked = 0;
    int asymmetric = 0;

    for (int game = 0; game < 150 && asymmetric < 5; ++game) {
        auto pos = make_pos("rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1");

        for (int ply = 0; ply < 60; ++ply) {
            std::vector<havoc::Move> legal;
            havoc::Movegen mvs(pos);
            mvs.generate<havoc::pseudo_legal, havoc::pieces>();
            for (int i = 0; i < mvs.size(); ++i)
                if (pos.is_legal(mvs[i]))
                    legal.push_back(mvs[i]);
            if (legal.empty())
                break;

            pos.do_move(legal[rng() % legal.size()]);

            // Skip the opening moves, where every position is still nearly
            // symmetric anyway and a bug would not show.
            if (ply < 6)
                continue;

            // Compare the position round-tripped through FEN, not the live one.
            // has_castled is play history, not board state, and does not
            // survive FEN, so evaluating the played position against a parsed
            // mirror would compare a castled king with an uncastled one.
            const std::string fen = pos.to_fen();
            auto original = make_pos(fen);
            auto mirrored = make_pos(mirror_fen(fen));
            const int a = eval.evaluate(original);
            const int b = eval.evaluate(mirrored);
            ++checked;
            if (a != b) {
                ++asymmetric;
                ADD_FAILURE() << "asymmetric evaluation\n  " << fen << " -> " << a << "\n  "
                              << mirror_fen(fen) << " -> " << b << "\n  difference " << (a - b);
            }
        }
    }

    EXPECT_GT(checked, 5000) << "the walk did not cover enough positions to mean anything";
    std::cout << "[          ] checked " << checked << " random positions" << std::endl;
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

// Opposite-colored bishops are only drawish once the bishops are the last
// pieces. The scale factor used to be keyed on bishop colors alone, so any
// middlegame in which each side happened to hold one bishop on opposite colors
// had its whole evaluation multiplied by 24/128 = 0.19 -- a clean extra pawn
// read as +0.26 instead of +1.42.
//
// Assert it by differencing against a run with the scale neutralized: with
// other pieces on the board the knob must make no difference at all.
TEST_F(EvalTest, KpkBitbaseAgreesWithMovegenOnEveryLegalPosition) {
    // The bitbase is built by value iteration over its own successor function,
    // so checking it against itself proves nothing. This walks a large sample
    // of legal KPK positions with the real move generator instead and asks
    // whether each verdict follows from the verdicts of its actual children,
    // including the two moves that leave the table: capturing the pawn (bare
    // kings, drawn) and promoting (KQ vs K, won unless the queen falls at once
    // or the defender is stalemated).
    auto legal_moves = [](havoc::position& p) {
        havoc::Movegen mv(p);
        mv.generate<havoc::pseudo_legal, havoc::pieces>();
        std::vector<havoc::Move> out;
        for (int i = 0; i < mv.size(); ++i)
            if (p.is_legal(mv[i]))
                out.push_back(mv[i]);
        return out;
    };
    auto promoted_material = [](const havoc::position& p) {
        return p.get_pieces<havoc::white, havoc::queen>() |
               p.get_pieces<havoc::white, havoc::rook>() |
               p.get_pieces<havoc::white, havoc::bishop>() |
               p.get_pieces<havoc::white, havoc::knight>();
    };

    long checked = 0, mismatches = 0;
    for (int pf = 0; pf < 4; ++pf)
        for (int pr = 1; pr <= 6; ++pr) {
            const int psq = pr * 8 + pf;
            for (int wk = 0; wk < 64; ++wk)
                for (int bk = 0; bk < 64; ++bk) {
                    if (wk == psq || bk == psq || wk == bk)
                        continue;
                    if (std::max(std::abs((wk >> 3) - (bk >> 3)), std::abs((wk & 7) - (bk & 7))) <= 1)
                        continue;
                    // Keep the runtime sane: every ninth position, which still
                    // covers every pawn square and both sides to move.
                    if ((wk * 64 + bk) % 9 != 0)
                        continue;
                    for (int s = 0; s < 2; ++s) {
                        const bool wtm = (s == 0);
                        havoc::U64 patt = 0ULL;
                        if ((psq & 7) > 0)
                            patt |= 1ULL << (psq + 7);
                        if ((psq & 7) < 7)
                            patt |= 1ULL << (psq + 9);
                        if (wtm && (patt & (1ULL << bk)))
                            continue;  // black already in check on white's turn

                        char b[64] = {0};
                        b[wk] = 'K';
                        b[psq] = 'P';
                        b[bk] = 'k';
                        std::string fen;
                        for (int r = 7; r >= 0; --r) {
                            int run = 0;
                            for (int c = 0; c < 8; ++c) {
                                const char pc = b[r * 8 + c];
                                if (!pc)
                                    ++run;
                                else {
                                    if (run) {
                                        fen += static_cast<char>('0' + run);
                                        run = 0;
                                    }
                                    fen += pc;
                                }
                            }
                            if (run)
                                fen += static_cast<char>('0' + run);
                            if (r)
                                fen += '/';
                        }
                        fen += wtm ? " w - - 0 1" : " b - - 0 1";

                        auto pos = make_pos(fen);
                        const bool want =
                            havoc::kpk::probe(wk, psq, bk, wtm ? havoc::white : havoc::black);

                        const auto moves = legal_moves(pos);
                        bool got = false;
                        if (!moves.empty()) {
                            got = !wtm;  // white needs one winning move, black needs them all
                            for (const auto& m : moves) {
                                havoc::position child = pos;
                                child.do_move(m);
                                const havoc::U64 cp = child.get_pieces<havoc::white, havoc::pawn>();
                                const havoc::U64 cq = promoted_material(child);
                                bool child_won;
                                if (cp == 0ULL && cq == 0ULL) {
                                    child_won = false;  // bare kings
                                } else if (cq != 0ULL) {
                                    const auto replies = legal_moves(child);
                                    child_won = !replies.empty();
                                    for (const auto& r2 : replies) {
                                        havoc::position g = child;
                                        g.do_move(r2);
                                        if (promoted_material(g) == 0ULL) {
                                            child_won = false;
                                            break;
                                        }
                                    }
                                } else {
                                    child_won = havoc::kpk::probe(
                                        child.king_square(havoc::white), havoc::bits::lsb(cp),
                                        child.king_square(havoc::black),
                                        child.to_move() == havoc::white ? havoc::white
                                                                        : havoc::black);
                                }
                                if (wtm ? child_won : !child_won) {
                                    got = wtm;
                                    break;
                                }
                            }
                        }
                        ++checked;
                        if (want != got && mismatches++ < 5)
                            ADD_FAILURE() << "bitbase says " << want << " but successors say " << got
                                          << " for " << fen;
                    }
                }
        }
    EXPECT_EQ(mismatches, 0);
    EXPECT_GT(checked, 15000) << "sampling stride skipped too much of the table";
}

TEST_F(EvalTest, DrawnKingAndPawnEndingsScoreZero) {
    havoc::parameters params;
    havoc::pawn_table pt(params);
    havoc::material_table mt(params);
    havoc::HCEEvaluator eval(pt, mt, params);

    // Textbook draws that haVoc used to score as a clean extra pawn.
    const char* drawn[] = {
        "7k/8/7K/7P/8/8/8/8 w - - 0 1",     // rook pawn, defender holds the corner
        "8/8/8/3k4/8/3K4/3P4/8 w - - 0 1",  // defender has the opposition
        "8/8/8/8/8/1k6/1p6/1K6 w - - 0 1",  // black pawn, white king stalemated
        "8/8/8/8/8/k7/p7/K7 w - - 0 1",     // black rook pawn, white king blockading
    };
    for (const char* fen : drawn) {
        auto pos = make_pos(fen);
        EXPECT_EQ(eval.evaluate(pos), 0) << "drawn KPK should score zero: " << fen;
    }

    // Won versions, so the rule cannot pass by flattening every KPK ending.
    const char* won[] = {
        "8/8/8/2k5/8/3K4/3P4/8 w - - 0 1",  // attacker has the opposition
        "8/6k1/8/8/8/8/6PK/8 w - - 0 1",    // king escorting the pawn
        "4k3/8/8/8/8/8/4P3/4K3 w - - 0 1",  // king leads the pawn up the board
    };
    for (const char* fen : won) {
        auto pos = make_pos(fen);
        EXPECT_GT(eval.evaluate(pos), 50) << "won KPK should still score as a win: " << fen;
    }
}

TEST_F(EvalTest, WrongRookPawnAndWrongBishopIsDrawn) {
    havoc::parameters params;
    havoc::pawn_table pt(params);
    havoc::material_table mt(params);
    havoc::HCEEvaluator eval(pt, mt, params);

    // Every pawn on a rook file, every bishop on the complex the promotion
    // square is not on, and the defending king on the promotion square. The
    // attacker cannot make progress no matter how many pawns are involved.
    struct Case {
        const char* fen;
        const char* why;
    };
    const Case drawn[] = {
        {"k7/8/8/8/8/8/PB6/6K1 w - - 0 1", "a-pawn, dark bishop, black king on a8"},
        {"7k/8/8/8/8/8/7P/5BK1 w - - 0 1", "h-pawn, bishop on f1 cannot see h8"},
        {"k7/8/8/8/8/P7/PB6/6K1 w - - 0 1", "two a-pawns, still drawn"},
        // Same theorem with the colors reversed, which the mirror tests do not
        // reach because they never generate this material.
        {"1k6/1b6/p7/8/8/8/8/K7 b - - 0 1", "black a-pawn, bishop on b7 cannot see a1"},
    };
    for (const auto& c : drawn) {
        auto pos = make_pos(c.fen);
        EXPECT_EQ(eval.evaluate(pos), 0) << "should be a dead draw: " << c.why << " (" << c.fen << ")";
    }

    // Negative controls. Each breaks exactly one clause of the theorem, and
    // each must still be scored as a win, so the rule cannot pass by simply
    // zeroing every rook-pawn ending.
    const Case winning[] = {
        {"k7/8/8/8/8/8/P1B5/6K1 w - - 0 1", "bishop on c2 does cover a8"},
        {"6k1/8/8/8/8/8/PB6/6K1 w - - 0 1", "defending king nowhere near the corner"},
        {"k7/8/8/8/8/8/PBN5/6K1 w - - 0 1", "a knight covers the corner the bishop cannot"},
        {"k7/8/8/8/8/8/PPB5/6K1 w - - 0 1", "the b-pawn does not promote in the corner"},
    };
    for (const auto& c : winning) {
        auto pos = make_pos(c.fen);
        EXPECT_GT(eval.evaluate(pos), 200) << "should still be winning: " << c.why << " (" << c.fen << ")";
    }
}

TEST_F(EvalTest, OppositeColorBishops_NotScaledWithPiecesOnBoard) {
    havoc::parameters params;
    havoc::parameters neutral;
    neutral.opposite_bishop_scale = 128;

    havoc::pawn_table pt(params);
    havoc::material_table mt(params);
    havoc::HCEEvaluator eval(pt, mt, params);

    havoc::pawn_table npt(neutral);
    havoc::material_table nmt(neutral);
    havoc::HCEEvaluator neval(npt, nmt, neutral);

    // Both queens, all four rooks and all four knights still on the board.
    // White has Be2 (light), black Be7 (dark), and white is a clean pawn up.
    auto middlegame = make_pos("r2q1rk1/1p2bppp/2n2n2/3p4/3P4/2N2N2/PP2BPPP/R2Q1RK1 w - - 0 1");
    EXPECT_EQ(eval.evaluate(middlegame), neval.evaluate(middlegame))
        << "opposite_bishop_scale must not touch a position that still has "
           "queens, rooks and knights on it";

    // Positive control: in a genuine opposite-colored bishop ending the knob
    // must still bite, otherwise the test above could pass by the feature
    // having been removed outright.
    auto ending = make_pos("8/pp3p2/2b1k3/8/1P6/2B1K3/P4P2/8 w - - 0 1");
    EXPECT_NE(eval.evaluate(ending), neval.evaluate(ending))
        << "opposite_bishop_scale must still apply to a pure OCB ending";
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

// The evaluation used to be flat across every move once the defender was down
// to a bare king: same material, no pawns, nothing positional to say. The
// search saw a plateau, had nothing to climb, and shuffled. Over a four-
// position suite the engine failed to mate with two bishops in two of them and
// needed a mean of 69 plies in the others; with a gradient it mates all four in
// a mean of 32.5. These assertions pin the gradient itself.
TEST_F(EvalTest, DrivesTheBareKingTowardsMate) {
    havoc::parameters params;
    havoc::pawn_table pt(params);
    havoc::material_table mt(params);
    havoc::HCEEvaluator eval(pt, mt, params);
    auto score = [&](const char* fen) {
        auto pos = make_pos(fen);
        return eval.evaluate(pos);
    };

    // Identical material either way, so the only thing separating these is
    // where the defending king stands.
    EXPECT_GT(score("k7/8/8/8/8/8/8/3RK3 w - - 0 1"), score("8/8/8/3k4/8/8/8/3RK3 w - - 0 1"))
        << "driving the bare king to the edge has to score better than leaving "
           "it in the centre";

    // Mate needs the attacking king in opposition, so closing the gap counts.
    EXPECT_GT(score("k7/8/1K6/8/8/8/8/3R4 w - - 0 1"), score("k7/8/8/8/8/8/8/3RK3 w - - 0 1"))
        << "walking the attacking king in has to score better than leaving it home";

    // Bishop and knight only mate in the two corners the bishop can attack.
    // A dark-squared bishop wants h8, not a8, and both corners are otherwise
    // identical -- same edge distance, same distance to the white king.
    EXPECT_GT(score("7k/8/8/8/8/8/8/2BNK3 w - - 0 1"), score("k7/8/8/8/8/8/8/2BNK3 w - - 0 1"))
        << "bishop and knight must aim at the corner the bishop covers";
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
// The evaluation is exact for every caller. There used to be a lazy cutoff that
// returned a partial score whenever the position was lopsided, without ever
// consulting the caller's window; it fired in 62.6% of positions from random
// play and was a median of 71 cp -- worst 568 -- away from the truth. This
// pins the property that replaced it: the margin argument is vestigial and
// changing it cannot change what comes back.
TEST_F(EvalTest, EvaluationIsExactForEveryMargin) {
    havoc::parameters params;
    params.tempo = 25;
    havoc::pawn_table pt(params);
    havoc::material_table mt(params);
    havoc::HCEEvaluator eval(pt, mt, params);

    std::mt19937 rng(20260814u);
    int checked = 0;
    for (int game = 0; game < 12; ++game) {
        auto pos = make_pos("rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1");
        for (int ply = 0; ply < 60; ++ply) {
            std::vector<havoc::Move> legal;
            havoc::Movegen mvs(pos);
            mvs.generate<havoc::pseudo_legal, havoc::pieces>();
            for (int i = 0; i < mvs.size(); ++i)
                if (pos.is_legal(mvs[i]))
                    legal.push_back(mvs[i]);
            if (legal.empty())
                break;
            pos.do_move(legal[rng() % legal.size()]);
            if (ply < 6)
                continue;
            auto p2 = make_pos(pos.to_fen());
            const int exact = eval.evaluate(p2, -1);
            for (int margin : {1, 50, 225, 1000}) {
                ASSERT_EQ(eval.evaluate(p2, margin), exact)
                    << "margin " << margin << " changed the evaluation of " << pos.to_fen();
            }
            ++checked;
        }
    }
    EXPECT_GT(checked, 400);
}

// A whole extra queen: the sign must not be in doubt, and the two sides must
// agree once the score is taken from the point of view of whoever is to move.
TEST_F(EvalTest, AnExtraQueenIsWorthTheSameToEitherSide) {
    havoc::parameters params;
    params.tempo = 25;
    havoc::pawn_table pt(params);
    havoc::material_table mt(params);
    havoc::HCEEvaluator eval(pt, mt, params);

    auto white_up = make_pos("rnb1kbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1");
    auto black_up = make_pos("rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNB1KBNR b KQkq - 0 1");
    EXPECT_GT(eval.evaluate(white_up), 0) << "white is up a queen and to move";
    EXPECT_GT(eval.evaluate(black_up), 0) << "black is up a queen and to move";
    EXPECT_EQ(eval.evaluate(white_up), eval.evaluate(black_up));
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
        // The same wide-open boards, but with a defending minor so the side to
        // move is not facing a bare king. Pawnless king-versus-lone-king
        // positions take the mating shortcut in evaluate() and never reach the
        // mobility tables, so without these the top rook and bishop buckets
        // lose their only coverage.
        "4k2n/8/8/8/8/8/8/R3K3 w Q - 0 1",
        "7n/8/4k3/8/3QB3/8/4K3/8 w - - 0 1",
        "k6n/8/8/8/3R4/8/8/4K3 w - - 0 1",
        // Cramped, to reach the bottom of the mobility tables
        "rnbqkbnr/pppppppp/8/8/8/PPPPPPPP/RNBQKBNR/8 w kq - 0 1",
        "1nb1kb2/1ppppp2/8/8/8/8/PPPPPP2/1NB1KB2 w - - 0 1",
        // A knight safe check: black Kg8 is checked from f6, the white
        // knight on e4 attacks f6, and nothing black owns defends it -- the
        // h7 pawn covers g6, not f6. Without this the knight safe-check
        // weight is never exercised by any position in this list.
        "r5k1/7p/8/8/4N3/8/5PPP/4K2R w K - 0 1",
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
        "6k1/5ppp/8/8/3b4/8/2B2PPP/6K1 w - - 0 1",        "6k1/8/8/3b4/8/8/2B5/6K1 w - - 0 1",
        "6k1/8/8/8/8/8/2B5/6K1 w - - 0 1",
        "6k1/8/8/8/8/8/2N5/6K1 w - - 0 1",
        "6k1/8/8/8/8/8/2R5/6K1 w - - 0 1",
        "6k1/5ppp/8/8/8/8/5PPP/2R3K1 w - - 0 1",
        // A wrong rook pawn: white's a-pawn promotes on a8, white's bishop is
        // on the dark complex and can never touch it, and the black king is
        // already sitting on the promotion square. Nothing else in this list
        // reaches wrong_rook_pawn_scale.
        "k7/8/8/8/8/8/PB6/6K1 w - - 0 1",
        // King and pawn endings
        "8/3k4/8/8/3P4/3K4/8/8 w - - 0 1",
        "8/2p5/3p4/KP5r/1R3p1k/8/4P1P1/8 w - - 0 1",
        "6k1/5ppp/8/8/8/8/1P6/1K6 w - - 0 1",
        // Doubled pawns, asymmetric so the two sides cannot cancel: white has
        // a doubled c-file, black does not. Without this the corpus never
        // reaches the doubled-pawn penalty at all.
        "6k1/pp3ppp/8/8/8/2P5/PPP2PPP/6K1 w - - 0 1",
        // Doubled and isolated together, which takes the other branch of the
        // doubled-pawn test, plus a tripled file.
        "6k1/1p3pp1/8/8/2P5/2P5/2P2PP1/6K1 w - - 0 1",
        // The same weakness with a full board, so the middlegame endpoint of
        // the doubled-pawn penalty is exercised and not just the endgame one.
        "r1bqkbnr/pp1ppppp/2n5/8/8/2P5/PPP2PPP/RNBQKBNR w KQkq - 0 1",
        // A passer two ranks from promotion, which is the one rung of the
        // passed_pawn_rank_bonus ladder the rest of the set never reaches.
        "6k1/pp6/4P3/8/8/8/5PPP/6K1 w - - 0 1",
        // A rook behind its own passer with a clear path to it. This is the
        // only shape that reaches passed_pawn_rook_behind and, because a1-a5
        // is empty, passed_pawn_rook_support as well. Enough material is left
        // on that is_endgame() stays false, otherwise the endgame
        // specialisation returns before eval_passed_pawns runs at all.
        "r3k2r/4bppp/8/P7/8/8/1PP2PPP/R3K2R w KQkq - 0 1",
        // The same passer with no rook behind it and a black rook bearing on
        // the square in front, so crudeControl goes negative and the blocked
        // penalty fires. Three positions, one per rung of the ladder: the
        // pawn is three, two and one rank from promotion.
        "r3k2r/4bppp/8/P7/8/8/1PP2PPP/4K2R w Kkq - 0 1",
        "r3k2r/4bppp/P7/8/8/8/1PP2PPP/4K2R w Kkq - 0 1",
        "1r2k2r/P3bppp/8/8/8/8/1PP2PPP/4K2R w Kk - 0 1",
        // Everything above is quiet or an ending. That left the entire king
        // attack weighting -- the per-piece king harassment tables, the
        // attack_combos matrix -- and the threat tables unreached, because
        // none of these positions has an enemy piece bearing on a king zone
        // or an undefended piece to threaten. The tuner could move those
        // weights and the error would not change.
        //
        // Pieces swarming a castled king: knight, bishop, rook and queen all
        // attack squares in black's king ring at once, which is what the
        // combination matrix is indexed by.
        "r1b2rk1/pp3ppp/2n5/q2p2N1/3P4/2B5/PP2QPPP/R4RK1 w - - 0 1",
        "r4rk1/pp3ppp/8/2b1N3/2B5/1Q6/PP3PPP/5RK1 w - - 0 1",
        "2r3k1/5ppp/8/6N1/3Q4/8/PP3PPP/6K1 w - - 0 1",
        // The mirror of the same idea, so a white-only attack cannot be the
        // only thing the corpus sees.
        "6k1/pp3ppp/8/3q4/6n1/8/PP3PPP/2R3K1 b - - 0 1",
        // Loose pieces of every type standing en prise to every type, which
        // is what the knight/bishop/rook/queen threat tables are indexed by.
        "4k3/1n1r4/8/2B1Q3/8/2N5/8/4K3 w - - 0 1",
        "4k3/3q4/8/2n1r3/8/2B2R2/8/4K3 b - - 0 1",
        // An asymmetric bishop pair: white keeps both bishops, black has one
        // bishop and two knights, so doubled_bishop_bonus cannot cancel.
        "r1bqk2r/pppp1ppp/2n2n2/4p3/2B1P3/2NP1N2/PPP2PPP/R1BQK2R w KQkq - 0 1",
        // A rook on the seventh and, separately, connected rooks on the back
        // rank. Black's rooks are split by a knight on b8 so the bonus does
        // not appear on both sides and cancel.
        "r2q2k1/2R2ppp/8/8/8/8/PP3PPP/3Q2K1 w - - 0 1",
        "rn3rk1/5ppp/8/8/8/8/PP3PPP/R3R1K1 w - - 0 1",
        // A rook on a fully open file, with no pawn of either colour on it.
        "3r2k1/pp3ppp/8/8/8/8/PP3PPP/4R1K1 w - - 0 1",
        // One undefended victim on d5, attacked at once by a knight, a
        // bishop, a rook and a queen, with the victim's type varied across
        // the four positions. eval_threats scores `ei.pieces[them] ^ pawns`
        // minus everything the enemy defends, so the victim has to be both
        // genuinely loose and genuinely attacked -- black's king and pawns are
        // parked on the far side of the board so they defend nothing.
        "7k/3Q2pp/8/3n4/1N6/8/B5PP/3RK3 w - - 0 1",
        "7k/3Q2pp/8/3b4/1N6/8/B5PP/3RK3 w - - 0 1",
        "7k/3Q2pp/8/3r4/1N6/8/B5PP/3RK3 w - - 0 1",
        "7k/3Q2pp/8/3q4/1N6/8/B5PP/3RK3 w - - 0 1",
        // A pawn joining the attack on the king ring. attack_combos is
        // indexed by a pair of attacking piece types and the pawn row is only
        // reachable when a pawn attacks the ring alongside a piece: here f6
        // covers g7 while the knight, rook and queen cover it too.
        "r5k1/pp2R3/4NP2/7Q/8/8/PP6/6K1 w - - 0 1",
        // The same with a second pawn, so the pawn arm of the king harassment
        // table reaches its top entry rather than stopping at one square.
        "r5k1/pp4P1/5P2/7Q/8/8/PP6/R5K1 w - - 0 1",
        // Strictly one-sided threats. In the four d5 positions above both
        // sides end up with a loose piece of the same type, so the threat
        // weight moves identically on both sides of the subtraction and
        // cancels. Here white's attacker is defended by a pawn or its king
        // and black's is not, so only one side scores.
        "k7/pp6/8/3n4/1N6/P7/6PP/4K3 w - - 0 1",
        "k7/pp6/8/3r4/8/8/6PP/3RK3 w - - 0 1",
        // Every piece type attacking one square of the king ring at once.
        // attack_combos is indexed by a *pair* of types that attack the same
        // square, so a swarm that covers different squares reaches none of
        // it. Pawn h6, knight f5, bishop f8, rook e7 and queen b2 all bear on
        // g7, which reaches all ten live entries of the matrix from one
        // position. Nothing attacks g8, so black is not in check.
        "r4Bk1/pp2R3/7P/5N2/8/8/PQP5/6K1 w - - 0 1",
        // A centralised enemy king, which is the only way a single rook or
        // queen reaches the upper entries of its harassment table: a king on
        // the edge simply has fewer ring squares to attack.
        "r7/pp6/8/3k4/2R5/8/PP6/6K1 w - - 0 1",
        "r7/pp6/8/3k4/1Q6/8/PP6/6K1 w - - 0 1",
        // A rook attacking a loose enemy rook. eval_threats builds its
        // "defended" set from pawn and piece attacks only -- a king does not
        // count as a defender -- so in the naive version of this position both
        // rooks read as hanging and the weight cancels. White's rook is
        // defended by the c3 pawn instead.
        "k7/pp6/8/3r4/3R4/2P5/6PP/4K3 w - - 0 1",
        // A queen bearing on exactly four squares of a centralised king's
        // ring, along the e-file and the e2-c4 diagonal.
        "r7/pp6/8/3k4/8/8/PP2Q3/6K1 w - - 0 1",
        // Outposts, indexed by the file of the outpost square. A hole is the
        // square directly in front of an enemy backward pawn, and a pawn whose
        // neighbouring files are both empty counts as backward, so pawns on
        // alternating files are all backward at once and one position covers
        // four files. Two positions per piece therefore reach all eight
        // entries. Black has no minor piece to place on a hole of White's, so
        // the term cannot cancel between the two sides.
        "7k/8/8/p1p1p1p1/N1N1N1N1/8/8/7K w - - 0 1",
        "k7/8/8/1p1p1p1p/1N1N1N1N/8/8/K7 w - - 0 1",
        "7k/8/8/p1p1p1p1/B1B1B1B1/8/8/7K w - - 0 1",
        "k7/8/8/1p1p1p1p/1B1B1B1B/8/8/K7 w - - 0 1",
        // Three enemy pawns storming the king's flank, the top entry of
        // king_storm_penalty. White's king is on the kingside with Black's f,
        // g and h pawns advancing on it, while Black's king sits on the
        // opposite flank where White has nothing storming, so the two sides do
        // not cancel. The queens and rooks are there to keep the middlegame
        // weight high enough for the term, which tapers to nothing in the
        // endgame, to survive the phase interpolation.
        "k6r/7q/8/8/5ppp/8/PP3PPP/R2Q2K1 w - - 0 1",
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

    // Search constants are tunable but deliberately invisible to the
    // evaluation: they decide which nodes get visited, not what a position is
    // worth. Their coverage is asserted by
    // SearchTest.EverySearchParameterReachesTheSearch instead.
    std::set<std::string> search_params;
    for (auto& [name, slot] : params.all_params(havoc::TuneStage::search))
        search_params.insert(name);

    for (auto& [name, slot] : slots) {
        if (name.rfind("pst_", 0) == 0)
            continue;
        if (search_params.count(name))
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
        // loop starts at knight. uncastled_penalty is a feature that was never
        // implemented -- eval_king has a hard-coded castling bonus with no
        // matching penalty.
        "attacker_weight_0", "uncastled_penalty",
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
// The generalisation of the test below. That one names three position pairs by
// hand, which only catches a cache-key hole if someone already guessed where it
// is. This walks random games and checks every position twice: once with the
// pawn and material tables emptied first, and once with them full of whatever
// the rest of the walk left behind.
//
// A pure evaluation cannot tell the difference. If it can, some input to a
// cached term is not part of that cache's key, and the entry a position finds
// depends on which positions came before it. That is exactly the defect found
// in the material key, in the pawn hash king mask, and in has_castled, three
// separate times, each time only after it had already cost playing strength.
// This test does not need to know which term is wrong to fail.
TEST_F(EvalTest, EvaluationIsIndependentOfCacheContentsOverRandomPlay) {
    havoc::parameters params;
    havoc::pawn_table pt(params);
    havoc::material_table mt(params);
    havoc::HCEEvaluator eval(pt, mt, params);

    std::mt19937 rng(20260813u); // fixed seed: a failure must be reproducible

    std::vector<std::string> fens;
    for (int game = 0; game < 30; ++game) {
        auto pos = make_pos("rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1");
        for (int ply = 0; ply < 70; ++ply) {
            std::vector<havoc::Move> legal;
            havoc::Movegen mvs(pos);
            mvs.generate<havoc::pseudo_legal, havoc::pieces>();
            for (int i = 0; i < mvs.size(); ++i)
                if (pos.is_legal(mvs[i]))
                    legal.push_back(mvs[i]);
            if (legal.empty())
                break;
            pos.do_move(legal[rng() % legal.size()]);
            if (ply >= 6)
                fens.push_back(pos.to_fen());
        }
    }
    ASSERT_GT(fens.size(), 1000u) << "the walk did not cover enough positions to mean anything";

    // Cold: every position sees empty tables, so nothing can leak into it.
    std::vector<float> cold;
    cold.reserve(fens.size());
    for (const auto& fen : fens) {
        pt.clear();
        mt.clear();
        auto pos = make_pos(fen);
        cold.push_back(eval.evaluate(pos, -1.0f));
    }

    // Warm, forwards: the tables now carry every earlier position in the walk.
    pt.clear();
    mt.clear();
    int mismatches = 0;
    for (size_t i = 0; i < fens.size() && mismatches < 5; ++i) {
        auto pos = make_pos(fens[i]);
        const float warm = eval.evaluate(pos, -1.0f);
        if (warm != cold[i]) {
            ++mismatches;
            ADD_FAILURE() << "evaluation depends on cache contents (forward walk)\n  " << fens[i]
                          << "\n  cold " << cold[i] << "  warm " << warm;
        }
    }

    // Warm, backwards: a different set of entries is resident when each
    // position is reached, so a hole that the forward order happened to miss
    // still has a second chance to show.
    pt.clear();
    mt.clear();
    for (size_t n = fens.size(); n > 0 && mismatches < 5; --n) {
        const size_t i = n - 1;
        auto pos = make_pos(fens[i]);
        const float warm = eval.evaluate(pos, -1.0f);
        if (warm != cold[i]) {
            ++mismatches;
            ADD_FAILURE() << "evaluation depends on cache contents (reverse walk)\n  " << fens[i]
                          << "\n  cold " << cold[i] << "  warm " << warm;
        }
    }

    std::cout << "[          ] checked " << fens.size() << " positions cold and warm"
              << std::endl;
}

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
