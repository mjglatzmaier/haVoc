#pragma once

/// The quantised network: file format, weights, and a scalar reference
/// implementation of the forward pass.
///
/// Why integers at all
/// -------------------
/// The forward pass runs at every node of an alpha-beta search, millions of
/// times a second, so it is done in fixed point. That is not a performance
/// nicety: float inference is roughly an order of magnitude too slow to be
/// worth having, and an evaluator that costs more than it informs is a
/// regression however good its predictions are.
///
/// Why a scalar reference exists, and stays
/// ---------------------------------------
/// This implementation is deliberately unvectorised. It is the definition of
/// what the network computes, and a SIMD kernel is checked against it rather
/// than replacing it. Without a reference, a vectorised kernel is only
/// checkable against itself, and the failure mode of a wrong kernel is not a
/// crash -- it is an engine that plays slightly badly for reasons no test
/// reports.
///
/// The quantisation scheme
/// -----------------------
/// Every activation is clipped ReLU into [0, 1], which is what makes the
/// mapping onto a fixed-point range exact rather than approximate.
///
///   accumulator      integer, scale QA           value in [0, QA]
///   dense weights    int8,    scale s_L          per layer, from the file
///   dense products   int32,   scale QA * s_L     rescaled by / s_L
///   output           int32,   scale QA * s_out   -> centipawns
///
/// `scripts/nnue/quantise.py` performs exactly these roundings, and the
/// exactness test requires the two paths to agree on every position, not
/// approximately but identically. Anything less and the two implementations
/// have merely been observed to be similar.

#include <cstdint>
#include <cstring>
#include <istream>
#include <string>
#include <vector>

#if defined(__AVX2__)
#include <immintrin.h>
#endif

#include "havoc/nnue/features.hpp"

namespace havoc::nnue {

inline constexpr uint32_t kNetworkFormatVersion = 1;

/// Accumulator scale. Chosen so a clipped activation fills the int16 range
/// without reaching it, leaving headroom for the sum of ~30 feature rows.
inline constexpr int32_t kQA = 255;

/// Dense weights are int8, and each layer carries its **own** scale, chosen
/// when the network is quantised as `floor(127 / max|w|)` for that layer.
///
/// A single shared scale was tried first and was a 46 cp error. The scale had
/// been fixed at 64 to match the clamp applied during training (127/64), but
/// the trained network's largest weight was only 1.25, so two thirds of the
/// int8 range went unused and a third of `fc1`'s weights -- whose rms is
/// 0.062 -- were smaller than a single quantisation step. Fitting the scale
/// to each layer's actual range cut the error to 9.2 cp for free.
///
/// The scales live in the file rather than here, because they are a property
/// of the trained network and not of the engine.
inline constexpr int32_t kMaxDenseWeight = 127;

/// Centipawns per unit of network output. Must equal `CP_SCALE` in
/// `scripts/nnue/train.py`; it is stored in the file and checked on load.
inline constexpr int32_t kDefaultCpScale = 400;

/// Bounds the vector kernel's stack buffers. A network wider than this still
/// loads and still runs -- on the reference path. The loader's own limits are
/// looser on purpose: refusing to load a wide network would make this an
/// architecture constraint rather than a kernel one.
inline constexpr int kMaxSimdL1 = 2048;
inline constexpr int kMaxSimdL2 = 512;

#pragma pack(push, 1)
struct NetworkHeader {
    char magic[4];  ///< "HVNW"
    uint32_t format_version;
    uint32_t feature_set_version;
    uint32_t input_dim;
    uint32_t l1;
    uint32_t l2;
    uint32_t l3;
    int32_t cp_scale;
    int32_t qa;      ///< Accumulator/activation scale.
    int32_t s_fc1;   ///< Per-layer int8 weight scales; see kMaxDenseWeight.
    int32_t s_fc2;
    int32_t s_out;
    char reserved[16];
};
#pragma pack(pop)
static_assert(sizeof(NetworkHeader) == 64, "header layout is part of the file format");

[[nodiscard]] inline int32_t clipped(int32_t x, int32_t hi) {
    return x < 0 ? 0 : (x > hi ? hi : x);
}

/// Immutable weights, shared by every thread. Accumulators are not here; they
/// are per-thread and live with the evaluator.
class Network {
  public:
    /// @return An error string, empty on success. Loading never throws and
    ///         never leaves the object half-populated.
    [[nodiscard]] std::string load(std::istream& in);

    [[nodiscard]] bool loaded() const { return loaded_; }
    [[nodiscard]] int l1() const { return l1_; }
    [[nodiscard]] int l2() const { return l2_; }
    [[nodiscard]] int l3() const { return l3_; }
    [[nodiscard]] int32_t cp_scale() const { return cp_scale_; }

    /// The feature transformer row for one active input, `l1()` values wide.
    [[nodiscard]] const int16_t* ft_row(int feature) const {
        return ft_weights_.data() + static_cast<size_t>(feature) * static_cast<size_t>(l1_);
    }
    [[nodiscard]] const int16_t* ft_bias() const { return ft_bias_.data(); }

    /// Evaluate from two finished accumulators, side-to-move first.
    ///
    /// @param acc_us   Raw accumulator for the side to move, `l1()` wide.
    /// @param acc_them Raw accumulator for the other side.
    /// @return Centipawns, side-to-move point of view.
    ///
    /// Dispatches to a vectorised kernel where one is available and the layer
    /// widths suit it, and to `forward_reference` otherwise. The two must
    /// agree exactly, not nearly -- integer addition is associative, so a
    /// different summation order is not an excuse for a different answer.
    [[nodiscard]] int forward(const int32_t* acc_us, const int32_t* acc_them) const {
#if defined(__AVX2__)
        if (simd_ok_)
            return forward_avx2(acc_us, acc_them);
#endif
        return forward_reference(acc_us, acc_them);
    }

    /// The definition of what the network computes. Never dispatched around;
    /// a vectorised kernel is checked against this, not trusted instead of it.
    [[nodiscard]] int forward_reference(const int32_t* acc_us, const int32_t* acc_them) const;

  private:
#if defined(__AVX2__)
    [[nodiscard]] int forward_avx2(const int32_t* acc_us, const int32_t* acc_them) const;
#endif

    bool loaded_ = false;
    /// Whether the layer widths are multiples of the vector width. Checked
    /// once at load rather than per call, and false falls back rather than
    /// failing: a network with an odd layer size should still run.
    bool simd_ok_ = false;
    int l1_ = 0, l2_ = 0, l3_ = 0;
    int32_t cp_scale_ = kDefaultCpScale;
    int32_t s_fc1_ = 1, s_fc2_ = 1, s_out_ = 1;

    std::vector<int16_t> ft_weights_;  ///< [input_dim * l1]
    std::vector<int16_t> ft_bias_;     ///< [l1]
    std::vector<int8_t> fc1_w_;        ///< [l2 * 2*l1], row-major by output
    std::vector<int32_t> fc1_b_;       ///< [l2]
    std::vector<int8_t> fc2_w_;        ///< [l3 * l2]
    std::vector<int32_t> fc2_b_;       ///< [l3]
    std::vector<int8_t> out_w_;        ///< [l3]
    int32_t out_b_ = 0;

    /// The dense weights again, widened to int16 at load.
    ///
    /// Pure redundancy, kept because it is measurably worth it: sign-extending
    /// int8 to int16 inside the loop puts four shuffle uops per iteration on a
    /// single port and that port becomes the limit. Widening once at load
    /// costs 32 KB and 13% of the layer's time (196 ns against 226 ns for the
    /// 512x32 layer, measured). The file stays int8; this is a decode, not a
    /// second copy of the format.
    std::vector<int16_t> fc1_w16_, fc2_w16_;
};

inline int Network::forward_reference(const int32_t* acc_us, const int32_t* acc_them) const {
    // Scratch is thread_local rather than automatic because this runs at every
    // node: three fresh allocations per evaluation cost more than the network.
    // It is per-thread, so a shared const Network stays safe to call from every
    // search thread at once.
    thread_local std::vector<int32_t> x, h1, h2;
    x.resize(static_cast<size_t>(2 * l1_));
    h1.resize(static_cast<size_t>(l2_));
    h2.resize(static_cast<size_t>(l3_));

    // Layer 1 input: both accumulators clipped, side to move first. The
    // ordering is load-bearing -- it is the only place the network learns
    // whose move it is.
    for (int i = 0; i < l1_; ++i) {
        x[static_cast<size_t>(i)] = clipped(acc_us[i], kQA);
        x[static_cast<size_t>(l1_ + i)] = clipped(acc_them[i], kQA);
    }

    for (int j = 0; j < l2_; ++j) {
        int32_t sum = fc1_b_[static_cast<size_t>(j)];
        const int8_t* w = fc1_w_.data() + static_cast<size_t>(j) * static_cast<size_t>(2 * l1_);
        for (int i = 0; i < 2 * l1_; ++i)
            sum += static_cast<int32_t>(w[i]) * x[static_cast<size_t>(i)];
        h1[static_cast<size_t>(j)] = clipped(sum / s_fc1_, kQA);
    }

    for (int j = 0; j < l3_; ++j) {
        int32_t sum = fc2_b_[static_cast<size_t>(j)];
        const int8_t* w = fc2_w_.data() + static_cast<size_t>(j) * static_cast<size_t>(l2_);
        for (int i = 0; i < l2_; ++i)
            sum += static_cast<int32_t>(w[i]) * h1[static_cast<size_t>(i)];
        h2[static_cast<size_t>(j)] = clipped(sum / s_fc2_, kQA);
    }

    int64_t out = out_b_;
    for (int i = 0; i < l3_; ++i)
        out += static_cast<int64_t>(out_w_[static_cast<size_t>(i)]) * h2[static_cast<size_t>(i)];

    // `out` carries scale QA * s_out; convert to centipawns. Integer division
    // truncates toward zero, and `scripts/nnue/quantise.py` does the same, so
    // the two paths agree exactly rather than nearly.
    return static_cast<int>(out * cp_scale_ / (static_cast<int64_t>(kQA) * s_out_));
}

#if defined(__AVX2__)

/// The same arithmetic as `forward_reference`, sixteen products at a time.
///
/// Exactness rests on two facts, and both are checked rather than assumed.
/// Activations are clipped into [0, QA] with QA = 255, and weights are int8,
/// so every product fits in int16 and every *adjacent pair* of products fits
/// in int32 -- which is precisely what `_mm256_madd_epi16` computes. Nothing
/// saturates, so the vector path is not an approximation of the scalar one;
/// it is the same sum in a different order, and integer addition does not
/// care about the order.
///
/// The alternative, `_mm256_maddubs_epi16`, would do twice as many products
/// per instruction and is what most engines use -- but it accumulates into
/// int16 and *saturates*, so it is only correct while the weights stay small
/// enough, a condition no test here could state. That trade is available
/// later, behind a measurement; it is not taken silently.
inline int Network::forward_avx2(const int32_t* acc_us, const int32_t* acc_them) const {
    // Plain stack arrays, not thread_local vectors: a thread_local with a
    // non-trivial constructor costs a guard check and an indirection on every
    // access, which at this call rate was measurably larger than the layer it
    // was holding. Sizes are bounded by the loader, which is why `simd_ok_`
    // and the bound below have to agree.
    int16_t x[2 * kMaxSimdL1];
    int16_t h1[kMaxSimdL2];
    int16_t h2[kMaxSimdL2];

    // A plain loop: an explicit packs/permute/clamp version of this measured
    // identical, so the compiler is already vectorising it and the intrinsics
    // would only be harder to read.
    for (int i = 0; i < l1_; ++i) {
        x[i] = static_cast<int16_t>(clipped(acc_us[i], kQA));
        x[l1_ + i] = static_cast<int16_t>(clipped(acc_them[i], kQA));
    }

    // One dense layer: `out[j] = clip((bias[j] + w[j] . in) / scale)`.
    //
    // Four output rows at a time. The inputs are read once for the four
    // instead of once each, which matters because this loop is limited by
    // load slots rather than by multiplies, and the four horizontal sums fold
    // into one shuffle chain instead of four.
    auto dense = [](const int16_t* w, const int32_t* bias, const int16_t* in, int in_dim,
                    int out_dim, int32_t scale, int16_t* out) {
        for (int j = 0; j < out_dim; j += 4) {
            __m256i a0 = _mm256_setzero_si256(), a1 = a0, a2 = a0, a3 = a0;
            const int16_t* w0 = w + static_cast<size_t>(j) * static_cast<size_t>(in_dim);
            const int16_t* w1 = w0 + in_dim;
            const int16_t* w2 = w1 + in_dim;
            const int16_t* w3 = w2 + in_dim;
            for (int i = 0; i < in_dim; i += 16) {
                const __m256i xv = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(in + i));
                a0 = _mm256_add_epi32(
                    a0, _mm256_madd_epi16(
                            xv, _mm256_loadu_si256(reinterpret_cast<const __m256i*>(w0 + i))));
                a1 = _mm256_add_epi32(
                    a1, _mm256_madd_epi16(
                            xv, _mm256_loadu_si256(reinterpret_cast<const __m256i*>(w1 + i))));
                a2 = _mm256_add_epi32(
                    a2, _mm256_madd_epi16(
                            xv, _mm256_loadu_si256(reinterpret_cast<const __m256i*>(w2 + i))));
                a3 = _mm256_add_epi32(
                    a3, _mm256_madd_epi16(
                            xv, _mm256_loadu_si256(reinterpret_cast<const __m256i*>(w3 + i))));
            }
            // hadd twice leaves [s0 s1 s2 s3 | s0 s1 s2 s3] split across the
            // two lanes; adding the halves completes the four sums.
            const __m256i s01 = _mm256_hadd_epi32(a0, a1);
            const __m256i s23 = _mm256_hadd_epi32(a2, a3);
            const __m256i s = _mm256_hadd_epi32(s01, s23);
            const __m128i sums = _mm_add_epi32(_mm256_castsi256_si128(s),
                                               _mm256_extracti128_si256(s, 1));
            alignas(16) int32_t part[4];
            _mm_store_si128(reinterpret_cast<__m128i*>(part), sums);
            for (int k = 0; k < 4; ++k)
                out[j + k] = static_cast<int16_t>(clipped((bias[j + k] + part[k]) / scale, kQA));
        }
    };

    dense(fc1_w16_.data(), fc1_b_.data(), x, 2 * l1_, l2_, s_fc1_, h1);
    dense(fc2_w16_.data(), fc2_b_.data(), h1, l2_, l3_, s_fc2_, h2);

    int64_t out = out_b_;
    for (int i = 0; i < l3_; ++i)
        out += static_cast<int64_t>(out_w_[static_cast<size_t>(i)]) * h2[i];
    return static_cast<int>(out * cp_scale_ / (static_cast<int64_t>(kQA) * s_out_));
}

#endif  // __AVX2__

inline std::string Network::load(std::istream& in) {
    loaded_ = false;

    NetworkHeader h{};
    in.read(reinterpret_cast<char*>(&h), sizeof(h));
    if (!in)
        return "network: file is shorter than its header";
    if (std::memcmp(h.magic, "HVNW", 4) != 0)
        return "network: bad magic, not a haVoc network";
    if (h.format_version != kNetworkFormatVersion)
        return "network: format version " + std::to_string(h.format_version) + ", engine speaks " +
               std::to_string(kNetworkFormatVersion);
    if (h.feature_set_version != kFeatureSetVersion)
        return "network: trained on feature set v" + std::to_string(h.feature_set_version) +
               ", engine encodes v" + std::to_string(kFeatureSetVersion) +
               ". Retrain; the indices no longer mean the same squares.";
    if (h.input_dim != kInputDim)
        return "network: input dimension mismatch";
    if (h.qa != kQA)
        return "network: accumulator scale differs from the engine's";
    if (h.s_fc1 <= 0 || h.s_fc2 <= 0 || h.s_out <= 0)
        return "network: non-positive dense weight scale";
    if (h.l1 == 0 || h.l2 == 0 || h.l3 == 0 || h.l1 > 4096 || h.l2 > 1024 || h.l3 > 1024)
        return "network: implausible layer sizes";

    s_fc1_ = h.s_fc1;
    s_fc2_ = h.s_fc2;
    s_out_ = h.s_out;
    l1_ = static_cast<int>(h.l1);
    l2_ = static_cast<int>(h.l2);
    l3_ = static_cast<int>(h.l3);
    cp_scale_ = h.cp_scale;

    auto read = [&](auto& vec, size_t n) {
        vec.resize(n);
        in.read(reinterpret_cast<char*>(vec.data()),
                static_cast<std::streamsize>(n * sizeof(vec[0])));
        return static_cast<bool>(in);
    };

    const size_t l1 = static_cast<size_t>(l1_), l2 = static_cast<size_t>(l2_),
                 l3 = static_cast<size_t>(l3_);
    if (!read(ft_weights_, static_cast<size_t>(kInputDim) * l1) || !read(ft_bias_, l1) ||
        !read(fc1_w_, l2 * 2 * l1) || !read(fc1_b_, l2) || !read(fc2_w_, l3 * l2) ||
        !read(fc2_b_, l3) || !read(out_w_, l3))
        return "network: truncated weights";

    in.read(reinterpret_cast<char*>(&out_b_), sizeof(out_b_));
    if (!in)
        return "network: truncated output bias";

    // A trailing byte means the writer and reader disagree about the layout,
    // which is worth failing on: the weights would load but mean something
    // else. Checked by trying to read one more byte and requiring EOF.
    char extra = 0;
    in.read(&extra, 1);
    if (in)
        return "network: file is longer than the declared architecture";

    // The vector kernel walks sixteen values at a time and does not carry a
    // tail loop, so it is used only when every inner dimension divides evenly.
    // A network that does not is not rejected; it simply runs the reference.
    fc1_w16_.assign(fc1_w_.begin(), fc1_w_.end());
    fc2_w16_.assign(fc2_w_.begin(), fc2_w_.end());

    simd_ok_ = l1_ % 16 == 0 && l2_ % 16 == 0 && l3_ % 4 == 0 && l1_ <= kMaxSimdL1 &&
               l2_ <= kMaxSimdL2 && l3_ <= kMaxSimdL2;

    loaded_ = true;
    return {};
}

}  // namespace havoc::nnue
