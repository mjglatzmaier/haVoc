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
#include <cstring>
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
    // acc_us = {QA, 0}, acc_them = {0, 0}, so x = {255, 0, 0, 0}.
    //
    // fc1 row 0 = {1,0,0,0} -> sum = 255, h1[0] = clip(255/64) = 3
    // fc1 row 1 = {0,...}   -> h1[1] = 0
    // fc2 row 0 = {2,0}     -> sum = 6,   h2[0] = clip(6/64)   = 0
    // fc2 row 1 = {0,0}, but bias 64*8 -> sum = 512, h2[1] = 8
    // out = {0, 3} -> o = 3*8 = 24
    // eval = 24 * 400 / (255 * 64) = 9600 / 16320 = 0 (truncated)
    //
    // A truncating divide is the point of the last line: it is what the
    // engine does, and the Python side matches it deliberately.
    w.fc1_w[0] = 1;
    w.fc2_w[0] = 2;
    w.fc2_b[1] = 64 * 8;
    w.out_w[1] = 3;

    nnue::Network net;
    ASSERT_EQ(load_from(w.serialise(), net), "");

    const int32_t acc_us[kL1] = {nnue::kQA, 0};
    const int32_t acc_them[kL1] = {0, 0};
    EXPECT_EQ(net.forward(acc_us, acc_them), 0);

    // Scale the output weight up so the result clears the truncation and the
    // arithmetic is pinned at a non-zero value too.
    w.out_w[1] = 127;
    w.fc2_b[1] = 64 * 255;  // h2[1] saturates at QA
    nnue::Network net2;
    ASSERT_EQ(load_from(w.serialise(), net2), "");
    // o = 127 * 255 = 32385; eval = 32385 * 400 / 16320 = 793
    EXPECT_EQ(net2.forward(acc_us, acc_them), 32385 * 400 / (255 * 64));
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
        {"foreign accumulator scale", [](RawWeights& w) { w.qa = 127; }},
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
