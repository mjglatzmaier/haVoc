/// @file test_nnue_network.cpp
/// @brief Pin the quantised network's file format and integer arithmetic.
///
/// This test writes **already-quantised integer weights** directly, rather
/// than converting floats the way `scripts/nnue/quantise.py` does. That is
/// deliberate. A C++ copy of the quantiser would be a second implementation
/// of the very thing whose duplication this project is trying to avoid, and
/// it would agree with itself no matter what either side did wrong. What is
/// under test here is narrower and checkable: that the engine performs the
/// documented fixed-point arithmetic, and that it refuses files it cannot
/// interpret rather than loading them into plausible-looking nonsense.
///
/// The other half -- that the trained network survives export -- is a
/// property of the *quantiser*, and is measured on real positions by
/// `quantise.py --verify-data`, which reports the int8-versus-float error
/// directly. Splitting it this way keeps each side checked by something that
/// is not itself.

#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <functional>
#include <sstream>
#include <string>
#include <vector>

#include "havoc/nnue/network.hpp"

#include <gtest/gtest.h>

using namespace havoc;

namespace {

/// A deliberately tiny architecture, so the expected value can be computed by
/// hand rather than by a second implementation of the forward pass.
constexpr int kL1 = 2, kL2 = 2, kL3 = 2;

struct RawWeights {
    std::vector<int16_t> ft_w = std::vector<int16_t>(
        static_cast<size_t>(nnue::kInputDim) * kL1, 0);
    std::vector<int16_t> ft_b = {0, 0};
    std::vector<int8_t> fc1_w = std::vector<int8_t>(kL2 * 2 * kL1, 0);
    std::vector<int32_t> fc1_b = {0, 0};
    std::vector<int8_t> fc2_w = std::vector<int8_t>(kL3 * kL2, 0);
    std::vector<int32_t> fc2_b = {0, 0};
    std::vector<int8_t> out_w = {0, 0};
    int32_t out_b = 0;

    int32_t s_fc1 = 64, s_fc2 = 64, s_out = 64;
    int32_t cp_scale = nnue::kDefaultCpScale;
    uint32_t format_version = nnue::kNetworkFormatVersion;
    uint32_t feature_set_version = nnue::kFeatureSetVersion;
    int32_t qa = nnue::kQA;
    const char* magic = "HVNW";

    [[nodiscard]] std::string serialise() const {
        nnue::NetworkHeader h{};
        std::memcpy(h.magic, magic, 4);
        h.format_version = format_version;
        h.feature_set_version = feature_set_version;
        h.input_dim = nnue::kInputDim;
        h.l1 = kL1;
        h.l2 = kL2;
        h.l3 = kL3;
        h.cp_scale = cp_scale;
        h.qa = qa;
        h.s_fc1 = s_fc1;
        h.s_fc2 = s_fc2;
        h.s_out = s_out;

        std::string s(reinterpret_cast<const char*>(&h), sizeof(h));
        auto append = [&s](const auto& v) {
            s.append(reinterpret_cast<const char*>(v.data()),
                     v.size() * sizeof(typename std::decay_t<decltype(v)>::value_type));
        };
        append(ft_w);
        append(ft_b);
        append(fc1_w);
        append(fc1_b);
        append(fc2_w);
        append(fc2_b);
        append(out_w);
        s.append(reinterpret_cast<const char*>(&out_b), sizeof(out_b));
        return s;
    }
};

[[nodiscard]] std::string load_from(const std::string& bytes, nnue::Network& net) {
    std::istringstream in(bytes, std::ios::binary);
    return net.load(in);
}

}  // namespace

TEST(NnueNetwork, HeaderIsSixtyFourBytes) {
    // The Python writer packs this layout by hand, so its size is a contract
    // rather than an implementation detail. A mismatch here shifts every
    // weight in the file by a few bytes and the network still "loads".
    EXPECT_EQ(sizeof(nnue::NetworkHeader), 64u);
}

TEST(NnueNetwork, LoadsAWellFormedFile) {
    RawWeights w;
    nnue::Network net;
    EXPECT_EQ(load_from(w.serialise(), net), "");
    EXPECT_TRUE(net.loaded());
    EXPECT_EQ(net.l1(), kL1);
    EXPECT_EQ(net.cp_scale(), nnue::kDefaultCpScale);
}

TEST(NnueNetwork, ComputesTheDocumentedFixedPointArithmetic) {
    RawWeights w;
    // One accumulator unit on for the side to move, everything else silent.
    // acc_us = {QA, 0}, acc_them = {0, 0}, so x = {127, 0, 0, 0}.
    //
    // fc1 row 0 = {1,0,0,0} -> sum = 127, h1[0] = clip(127/64) = 1
    // fc1 row 1 = {0,...}   -> h1[1] = 0
    // fc2 row 0 = {2,0}     -> sum = 2,   h2[0] = clip(2/64)   = 0
    // fc2 row 1 = {0,0}, but bias 64*8 -> sum = 512, h2[1] = 8
    // out = {0, 2} -> o = 2*8 = 16
    // eval = 16 * 400 / (127 * 64) = 6400 / 8128 = 0 (truncated)
    //
    // A truncating divide is the point of the last line: it is what the
    // engine does, and the Python side matches it deliberately.
    w.fc1_w[0] = 1;
    w.fc2_w[0] = 2;
    w.fc2_b[1] = 64 * 8;
    w.out_w[1] = 2;

    nnue::Network net;
    ASSERT_EQ(load_from(w.serialise(), net), "");

    const int32_t acc_us[kL1] = {nnue::kQA, 0};
    const int32_t acc_them[kL1] = {0, 0};
    EXPECT_EQ(net.forward(acc_us, acc_them), 0);

    // Scale the output weight up so the result clears the truncation and the
    // arithmetic is pinned at a non-zero value too.
    w.out_w[1] = 127;
    w.fc2_b[1] = 64 * nnue::kQA;  // h2[1] saturates at QA
    nnue::Network net2;
    ASSERT_EQ(load_from(w.serialise(), net2), "");
    // o = 127 * 127 = 16129; eval = 16129 * 400 / 8128 = 793
    EXPECT_EQ(net2.forward(acc_us, acc_them), 16129 * 400 / (127 * 64));
    EXPECT_EQ(net2.forward(acc_us, acc_them), 793);
}

TEST(NnueNetwork, ClipsAccumulatorsIntoTheActivationRange) {
    // Clipped ReLU is what makes the fixed-point mapping exact, so an
    // accumulator far outside the range must saturate rather than wrap.
    RawWeights w;
    w.fc1_w[0] = 1;
    nnue::Network net;
    ASSERT_EQ(load_from(w.serialise(), net), "");

    const int32_t huge[kL1] = {1 << 20, 0};
    const int32_t at_max[kL1] = {nnue::kQA, 0};
    const int32_t zero[kL1] = {0, 0};
    const int32_t negative[kL1] = {-(1 << 20), 0};
    EXPECT_EQ(net.forward(huge, zero), net.forward(at_max, zero));
    EXPECT_EQ(net.forward(negative, zero), net.forward(zero, zero));
}

TEST(NnueNetwork, TheSideToMoveHalfIsNotInterchangeableWithTheOther) {
    // If the two accumulators were concatenated in the wrong order, or the
    // ordering carried no information, the network could not express whose
    // move it is -- and would train to a plausible loss while evaluating the
    // wrong side.
    RawWeights w;
    w.fc1_w[0] = 100;  // reads acc_us[0]
    w.fc2_w[0] = 127;
    w.out_w[0] = 127;
    w.s_fc1 = 1;
    w.s_fc2 = 1;
    w.s_out = 1;

    nnue::Network net;
    ASSERT_EQ(load_from(w.serialise(), net), "");
    const int32_t on[kL1] = {nnue::kQA, 0};
    const int32_t off[kL1] = {0, 0};
    EXPECT_NE(net.forward(on, off), net.forward(off, on));
}

TEST(NnueNetwork, RefusesFilesItCannotInterpret) {
    struct Case {
        const char* name;
        std::function<void(RawWeights&)> corrupt;
    };
    const std::vector<Case> cases = {
        {"bad magic", [](RawWeights& w) { w.magic = "XXXX"; }},
        {"future format", [](RawWeights& w) { w.format_version += 1; }},
        {"stale feature set", [](RawWeights& w) { w.feature_set_version += 1; }},
        {"foreign accumulator scale", [](RawWeights& w) { w.qa = nnue::kQA * 2; }},
        {"zero dense scale", [](RawWeights& w) { w.s_fc1 = 0; }},
    };
    for (const auto& c : cases) {
        RawWeights w;
        c.corrupt(w);
        nnue::Network net;
        const std::string err = load_from(w.serialise(), net);
        EXPECT_FALSE(err.empty()) << "accepted a file with " << c.name;
        EXPECT_FALSE(net.loaded()) << c.name;
    }
}

TEST(NnueNetwork, RefusesATruncatedOrOverlongFile) {
    RawWeights w;
    const std::string good = w.serialise();

    nnue::Network shortnet;
    EXPECT_FALSE(load_from(good.substr(0, good.size() - 4), shortnet).empty());
    EXPECT_FALSE(shortnet.loaded());

    // A longer file means the writer and reader disagree about the layout.
    // The weights would load and mean something else, which is worse than a
    // failure because nothing reports it.
    nnue::Network longnet;
    EXPECT_FALSE(load_from(good + std::string(8, '\0'), longnet).empty());
    EXPECT_FALSE(longnet.loaded());

    nnue::Network headerless;
    EXPECT_FALSE(load_from(std::string(16, '\0'), headerless).empty());
}

// ── Cross-language exactness ────────────────────────────────────────────────
//
// Everything above pins the arithmetic against hand-computed values, which
// proves the engine does what this file says. It does not prove the engine
// does what `scripts/nnue/quantise.py` does, and that is the mismatch that
// would matter: the quantiser decides the weights, and if the two integer
// paths diverge anywhere -- a rounding direction, a division that floors
// instead of truncating -- the engine plays a network subtly unlike the one
// that was trained, and nothing reports it.
//
// `quantise.py --cases` writes the positions it verified together with the
// integer evaluation it computed for each. This replays them.
//
// The files are training artefacts, not repository content, so the test is
// opt-in via the environment and skips without them. Skipping is the honest
// outcome: the alternative is either a 21 MB binary in git or a test that
// silently passes when its input is missing.
//
//   HAVOC_NNUE_NET=/path/net.nnue HAVOC_NNUE_CASES=/path/cases.bin ./havoc_tests
TEST(NnueNetwork, MatchesThePythonQuantiserExactlyOnRecordedCases) {
    const char* net_path = std::getenv("HAVOC_NNUE_NET");
    const char* cases_path = std::getenv("HAVOC_NNUE_CASES");
    if (!net_path || !cases_path)
        GTEST_SKIP() << "set HAVOC_NNUE_NET and HAVOC_NNUE_CASES to replay recorded cases";

    std::ifstream nf(net_path, std::ios::binary);
    ASSERT_TRUE(nf.good()) << "cannot open " << net_path;
    nnue::Network net;
    ASSERT_EQ(net.load(nf), "");

    std::ifstream cf(cases_path, std::ios::binary);
    ASSERT_TRUE(cf.good()) << "cannot open " << cases_path;
    uint32_t count = 0;
    cf.read(reinterpret_cast<char*>(&count), sizeof(count));
    ASSERT_TRUE(cf.good());
    ASSERT_GT(count, 0u);

    const int l1 = net.l1();
    std::vector<int32_t> acc_us(static_cast<size_t>(l1)), acc_them(static_cast<size_t>(l1));
    auto build = [&](const uint16_t* feats, std::vector<int32_t>& acc) {
        const int16_t* bias = net.ft_bias();
        for (int i = 0; i < l1; ++i)
            acc[static_cast<size_t>(i)] = bias[i];
        for (int k = 0; k < nnue::kMaxActiveFeatures; ++k) {
            if (feats[k] >= nnue::kInputDim)  // padding
                continue;
            const int16_t* row = net.ft_row(feats[k]);
            for (int i = 0; i < l1; ++i)
                acc[static_cast<size_t>(i)] += row[i];
        }
    };

    uint32_t mismatches = 0;
    std::string first;
    for (uint32_t n = 0; n < count; ++n) {
        uint16_t fu[nnue::kMaxActiveFeatures], ft[nnue::kMaxActiveFeatures];
        int32_t expected = 0;
        cf.read(reinterpret_cast<char*>(fu), sizeof(fu));
        cf.read(reinterpret_cast<char*>(ft), sizeof(ft));
        cf.read(reinterpret_cast<char*>(&expected), sizeof(expected));
        ASSERT_TRUE(cf.good()) << "case file truncated at record " << n;

        build(fu, acc_us);
        build(ft, acc_them);
        const int got = net.forward(acc_us.data(), acc_them.data());
        if (got != expected) {
            ++mismatches;
            if (first.empty())
                first = "record " + std::to_string(n) + ": C++ " + std::to_string(got) +
                        " vs Python " + std::to_string(expected);
        }
    }

    // Exactly, on every case. "Close" would mean the two integer pipelines
    // differ somewhere and the difference merely happens to be small here.
    EXPECT_EQ(mismatches, 0u) << mismatches << " of " << count << " disagree; first " << first;
}

// The vectorised kernel sums in a different order and narrows the
// accumulators through a different path. Integer addition is associative, so
// "different order" is not licence for a different answer -- and a kernel that
// is only nearly right is the failure mode that shows up as an engine playing
// slightly badly with nothing to point at.
// Widths the vector path accepts; RawWeights' 2x2x2 is deliberately not one
// of them, so this needs its own network. The dense kernel walks 32 values at
// a time and finishes with at most one 16-wide remainder, so the widths below
// are chosen to reach every combination: fc2 with in_dim 16 is remainder only,
// 32 is main loop only, and 48 is both.
void expect_vector_kernel_matches_reference(int kVL1, int kVL2, int kVL3) {
    SCOPED_TRACE("l1=" + std::to_string(kVL1) + " l2=" + std::to_string(kVL2) + " l3=" +
                 std::to_string(kVL3));
    nnue::NetworkHeader h{};
    std::memcpy(h.magic, "HVNW", 4);
    h.format_version = nnue::kNetworkFormatVersion;
    h.feature_set_version = nnue::kFeatureSetVersion;
    h.input_dim = nnue::kInputDim;
    h.l1 = kVL1;
    h.l2 = kVL2;
    h.l3 = kVL3;
    h.cp_scale = nnue::kDefaultCpScale;
    h.qa = nnue::kQA;
    h.s_fc1 = 3;  // small scales keep the layer outputs off the clip bounds,
    h.s_fc2 = 3;  // so a difference has somewhere to show
    h.s_out = 3;

    uint32_t st = 0x9e3779b9u;
    auto next = [&st]() {
        st ^= st << 13;
        st ^= st >> 17;
        st ^= st << 5;
        return st;
    };

    std::string s(reinterpret_cast<const char*>(&h), sizeof(h));
    auto append = [&s](const auto& v) {
        s.append(reinterpret_cast<const char*>(v.data()),
                 v.size() * sizeof(typename std::decay_t<decltype(v)>::value_type));
    };
    std::vector<int16_t> ft_w(static_cast<size_t>(nnue::kInputDim) * kVL1, 0);
    std::vector<int16_t> ft_b(kVL1, 0);
    std::vector<int8_t> fc1_w(kVL2 * 2 * kVL1);
    for (auto& w : fc1_w)
        w = static_cast<int8_t>(static_cast<int32_t>(next() % 255u) - 127);
    std::vector<int32_t> fc1_b(kVL2);
    for (auto& b : fc1_b)
        b = static_cast<int32_t>(next() % 2001u) - 1000;
    std::vector<int8_t> fc2_w(kVL3 * kVL2);
    for (auto& w : fc2_w)
        w = static_cast<int8_t>(static_cast<int32_t>(next() % 255u) - 127);
    std::vector<int32_t> fc2_b(kVL3);
    for (auto& b : fc2_b)
        b = static_cast<int32_t>(next() % 2001u) - 1000;
    std::vector<int8_t> out_w(kVL3);
    for (auto& w : out_w)
        w = static_cast<int8_t>(static_cast<int32_t>(next() % 255u) - 127);
    const int32_t out_b = 137;

    append(ft_w);
    append(ft_b);
    append(fc1_w);
    append(fc1_b);
    append(fc2_w);
    append(fc2_b);
    append(out_w);
    s.append(reinterpret_cast<const char*>(&out_b), sizeof(out_b));

    nnue::Network net;
    ASSERT_EQ(load_from(s, net), "");

    std::vector<int32_t> us(static_cast<size_t>(kVL1)), them(static_cast<size_t>(kVL1));
    int disagreements = 0, nonzero = 0;
    for (int trial = 0; trial < 20000; ++trial) {
        for (int i = 0; i < kVL1; ++i) {
            // Include values outside [0, QA] on purpose: the clip and the
            // narrowing are exactly where the two paths could diverge.
            us[i] = static_cast<int32_t>(next() % 100000u) - 40000;
            them[i] = static_cast<int32_t>(next() % 100000u) - 40000;
        }
        const int a = net.forward(us.data(), them.data());
        const int b = net.forward_reference(us.data(), them.data());
        disagreements += (a != b) ? 1 : 0;
        nonzero += (b != 0) ? 1 : 0;
    }
    EXPECT_EQ(disagreements, 0);
    EXPECT_GT(nonzero, 1000) << "the trial network evaluates to zero almost everywhere, so "
                                "agreement between the two paths proves nothing";
}

TEST(NnueNetwork, TheVectorKernelAgreesWithTheReferenceExactly) {
    expect_vector_kernel_matches_reference(16, 16, 4);   // fc2: remainder only
    expect_vector_kernel_matches_reference(32, 32, 4);   // fc2: main loop only
    expect_vector_kernel_matches_reference(16, 48, 4);   // fc2: main loop + remainder
}
