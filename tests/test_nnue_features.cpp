/// @file test_nnue_features.cpp
/// @brief Pin the HalfKP feature encoding, which is the one part of the NNUE
///        pipeline that nothing downstream can check for us.
///
/// A wrong feature index does not crash and does not fail a sync test: it
/// trains perfectly well against itself and produces a network that is merely
/// weak, which is indistinguishable from "the idea did not work". By the time
/// that is visible, days of GPU time have been spent. So the encoding is
/// pinned here, before anything is trained on it.
///
/// The load-bearing test is mirror invariance. HalfKP orients squares to the
/// perspective being encoded, so a position and its colour-and-rank reflection
/// must produce *swapped* perspective feature sets. That single property
/// catches a missing `^ 56`, an orientation applied to the piece square but
/// not the king square, and an own/enemy polarity that is the wrong way round
/// -- the three ways this encoding is normally got wrong.

#include <algorithm>
#include <set>
#include <tuple>
#include <sstream>
#include <string>
#include <vector>

#include "havoc/bitboard.hpp"
#include "havoc/kpk.hpp"
#include "havoc/magics.hpp"
#include "havoc/nnue/dataset.hpp"
#include "havoc/nnue/features.hpp"
#include "havoc/position.hpp"
#include "havoc/zobrist.hpp"
#include "mirror.hpp"

#include <gtest/gtest.h>

using namespace havoc;

namespace {

class NnueFeatures : public ::testing::Test {
  protected:
    static void SetUpTestSuite() {
        bitboards::init();
        magics::init();
        zobrist::init();
        kpk::init();
    }
};

position from_fen(const std::string& fen) {
    position p;
    std::istringstream is(fen);
    p.setup(is);
    return p;
}

std::vector<int> features(const position& p, Color perspective) {
    std::vector<int> v;
    nnue::for_each_active(p, perspective, [&](int i) { v.push_back(i); });
    std::sort(v.begin(), v.end());
    return v;
}

int men_excluding_kings(const std::string& fen) {
    int n = 0;
    for (char c : fen.substr(0, fen.find(' '))) {
        if (std::isalpha(static_cast<unsigned char>(c)) && c != 'k' && c != 'K')
            ++n;
    }
    return n;
}

const std::vector<std::string>& corpus() {
    static const std::vector<std::string> fens = {
        "rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1",
        "r1bqkb1r/pppp1ppp/2n2n2/4p3/2B1P3/5N2/PPPP1PPP/RNBQK2R w KQkq - 4 4",
        "8/2p5/3p4/KP5r/1R3p1k/8/4P1P1/8 w - - 0 1",
        "r3k2r/p1ppqpb1/bn2pnp1/3PN3/1p2P3/2N2Q1p/PPPBBPPP/R3K2R w KQkq - 0 1",
        "4k3/8/8/8/8/8/8/4K3 w - - 0 1",
        "8/8/8/4k3/8/8/4P3/4K3 w - - 0 1",
        "2kr3r/pp1n1ppp/2pbpn2/q7/2PP4/2N1PN2/PPQ1BPPP/2KR3R b - - 0 1",
    };
    return fens;
}

}  // namespace

TEST_F(NnueFeatures, EveryIndexIsInRangeAndDistinct) {
    for (const auto& fen : corpus()) {
        const position p = from_fen(fen);
        for (Color c : {white, black}) {
            const auto v = features(p, c);
            for (int idx : v) {
                ASSERT_GE(idx, 0) << fen;
                ASSERT_LT(idx, nnue::kInputDim) << fen;
            }
            const std::set<int> uniq(v.begin(), v.end());
            EXPECT_EQ(uniq.size(), v.size())
                << "two men mapped to one input in " << fen << " (perspective " << c << ")";
        }
    }
}

TEST_F(NnueFeatures, CountsEveryManExceptTheKings) {
    for (const auto& fen : corpus()) {
        const position p = from_fen(fen);
        const int expected = men_excluding_kings(fen);
        EXPECT_EQ(static_cast<int>(features(p, white).size()), expected) << fen;
        EXPECT_EQ(static_cast<int>(features(p, black).size()), expected) << fen;
    }
}

TEST_F(NnueFeatures, NeverExceedsTheExportRecordCapacity) {
    // The on-disk record is fixed-stride; overflowing it silently truncates
    // the position rather than failing, so the bound is asserted, not assumed.
    for (const auto& fen : corpus()) {
        const position p = from_fen(fen);
        EXPECT_LE(features(p, white).size(), static_cast<size_t>(nnue::kMaxActiveFeatures)) << fen;
    }
    // 30 men besides the kings is the true maximum and must still fit.
    const position full = from_fen("rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1");
    EXPECT_EQ(features(full, white).size(), static_cast<size_t>(nnue::kMaxActiveFeatures));
}

TEST_F(NnueFeatures, MirroringAPositionSwapsThePerspectives) {
    for (const auto& fen : corpus()) {
        const position p = from_fen(fen);
        const position m = from_fen(havoc::testing::mirror_fen(fen));
        EXPECT_EQ(features(p, white), features(m, black)) << "white/black mismatch for " << fen;
        EXPECT_EQ(features(p, black), features(m, white)) << "black/white mismatch for " << fen;
    }
}

TEST_F(NnueFeatures, PerspectivesDisagreeOnAnAsymmetricPosition) {
    // Guards against the degenerate encoder that ignores its perspective
    // argument, which would pass mirror invariance trivially.
    const position p = from_fen("8/8/8/4k3/8/8/4P3/4K3 w - - 0 1");
    EXPECT_NE(features(p, white), features(p, black));
}

TEST_F(NnueFeatures, IndexMatchesTheDocumentedLayout) {
    // One index computed by hand, so a wholesale re-derivation of the layout
    // cannot quietly keep every relative property while moving everything.
    // White pawn on e2 (sq 12), white king on e1 (sq 4), white's perspective:
    // kind = pawn(0) * 2 + own(0) = 0, so index = 4 * 640 + 0 * 64 + 12.
    EXPECT_EQ(nnue::index(white, E1, white, pawn, E2), 4 * 640 + 12);

    // The same pawn from black's perspective, whose king is on e5 (sq 36):
    // both squares reflect (e5 -> e4 = 28, e2 -> e7 = 52) and the pawn is now
    // the enemy, so kind = 1.
    EXPECT_EQ(nnue::index(black, E5, white, pawn, E2), 28 * 640 + 1 * 64 + 52);
}

TEST_F(NnueFeatures, TheEncodingReconstructsTheBoardExactly) {
    // The decisive property, and the analogue of the FeatureDelta replay test:
    // invert every index and require the original men back. A network can only
    // learn what the encoding preserved, so anything lost here is lost for
    // good -- and losing it is silent, because a lossy encoding still trains.
    //
    // HalfKP deliberately does not carry side to move, castling rights or the
    // en passant square. Those are the evaluator's problem, not the feature
    // set's, so the reconstruction is of piece placement only.
    for (const auto& fen : corpus()) {
        const position p = from_fen(fen);
        for (Color perspective : {white, black}) {
            const int king_bucket =
                static_cast<int>(nnue::orient(perspective, p.king_square(perspective)));

            std::vector<std::tuple<Color, Piece, int>> decoded;
            nnue::for_each_active(p, perspective, [&](int idx) {
                ASSERT_EQ(idx / nnue::kFeaturesPerKing, king_bucket)
                    << "index does not agree with the king square it was bucketed by";
                const int rest = idx % nnue::kFeaturesPerKing;
                const int kind = rest / 64;
                const int sq = rest % 64;
                const Piece pt = static_cast<Piece>(kind / 2);
                const Color pc = (kind % 2 == 0)
                                     ? perspective
                                     : static_cast<Color>(static_cast<int>(perspective) ^ 1);
                decoded.emplace_back(pc, pt, sq);
            });

            std::vector<std::tuple<Color, Piece, int>> expected;
            for (int s = 0; s < 64; ++s) {
                const Piece pt = p.piece_on(static_cast<Square>(s));
                if (pt == no_piece || pt == king)
                    continue;
                const Color pc =
                    (p.get_pieces<white>() & bitboards::squares[s]) ? white : black;
                expected.emplace_back(pc, pt,
                                      static_cast<int>(nnue::orient(perspective,
                                                                    static_cast<Square>(s))));
            }

            std::sort(decoded.begin(), decoded.end());
            std::sort(expected.begin(), expected.end());
            EXPECT_EQ(decoded, expected) << "encoding lost or invented a man in " << fen
                                         << " (perspective " << perspective << ")";
        }
    }
}
