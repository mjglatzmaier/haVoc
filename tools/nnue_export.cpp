/// @file nnue_export.cpp
/// @brief Turn an EPD corpus into the fixed-stride training file the NNUE
///        trainer reads.
///
/// This exists so that the feature encoding happens exactly once, in C++, in
/// `include/havoc/nnue/features.hpp`, and is then *transported* to the
/// trainer rather than reimplemented there. The Python side never sees a FEN.
///
/// Two labelling modes, and the difference between them is the whole point of
/// the staged plan:
///
///   --label hce      Discard the EPD's score and relabel every position with
///                    haVoc's own static evaluation. The resulting file is a
///                    *known-answer test*: the target is a deterministic
///                    function we already possess, so if a network trained on
///                    it does not converge, the fault is in the pipeline --
///                    features, scaling, loss, quantisation, inference -- and
///                    not in the data or in the idea. Debugging that costs
///                    hours. Discovering it later, on real labels, costs days.
///
///   --label search   Keep the EPD's `ce` score, which is what a real network
///                    is trained on.

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include "havoc/bitboard.hpp"
#include "havoc/eval/hce.hpp"
#include "havoc/kpk.hpp"
#include "havoc/magics.hpp"
#include "havoc/material_table.hpp"
#include "havoc/movegen.hpp"
#include "havoc/nnue/dataset.hpp"
#include "havoc/nnue/features.hpp"
#include "havoc/parameters.hpp"
#include "havoc/pawn_table.hpp"
#include "havoc/position.hpp"
#include "havoc/zobrist.hpp"

using namespace havoc;

namespace {

struct Options {
    std::string input;
    std::string output = "nnue_train.hbin";
    nnue::LabelKind label = nnue::LabelKind::hce_static;
    int threads = static_cast<int>(std::thread::hardware_concurrency());
    int max_abs_score = 10000;
};

/// Parse one EPD line into a record. Feature extraction and, in HCE mode,
/// evaluation both happen here.
///
/// @return false if the line is malformed or the position is unusable, in
///         which case `out` is untouched.
bool encode_line(const std::string& line, nnue::LabelKind label, HCEEvaluator& ev, int max_abs,
                 nnue::Record& out) {
    const auto c9 = line.find(" c9 ");
    if (c9 == std::string::npos)
        return false;

    position pos;
    std::istringstream fs(line.substr(0, c9));
    pos.setup(fs);

    int score = 0;
    if (label == nnue::LabelKind::hce_static) {
        score = ev.evaluate(pos);
    } else {
        const auto ce = line.find(" ce ");
        if (ce == std::string::npos)
            return false;
        try {
            score = std::stoi(line.substr(ce + 4));
        } catch (...) {
            return false;
        }
    }
    if (std::abs(score) > max_abs)
        return false;

    // The EPD result is white's, written by datagen as 1 / 0.5 / 0.
    uint8_t result = 255;
    const auto q1 = line.find('"', c9);
    const auto q2 = q1 == std::string::npos ? std::string::npos : line.find('"', q1 + 1);
    if (q1 != std::string::npos && q2 != std::string::npos) {
        try {
            const double r = std::stod(line.substr(q1 + 1, q2 - q1 - 1));
            result = r > 0.75 ? 2 : (r < 0.25 ? 0 : 1);
        } catch (...) {
        }
    }

    std::memset(&out, 0, sizeof(out));
    out.score = static_cast<int16_t>(score);
    out.stm = pos.to_move() == white ? 0 : 1;
    out.result = result;
    std::fill(std::begin(out.feat_white), std::end(out.feat_white), nnue::kNoFeature);
    std::fill(std::begin(out.feat_black), std::end(out.feat_black), nnue::kNoFeature);

    int nw = 0, nb = 0;
    nnue::for_each_active(pos, white, [&](int idx) { out.feat_white[nw++] = static_cast<uint16_t>(idx); });
    nnue::for_each_active(pos, black, [&](int idx) { out.feat_black[nb++] = static_cast<uint16_t>(idx); });
    if (nw != nb)
        return false;  // Both perspectives encode the same men; they cannot differ.
    out.n = static_cast<uint8_t>(nw);
    return true;
}

void usage(const char* prog) {
    std::cerr << "Usage: " << prog << " --input FILE.epd [options]\n"
              << "  --output FILE   Output .hbin (default: nnue_train.hbin)\n"
              << "  --label KIND    hce | search (default: hce)\n"
              << "  --threads N     Encoding threads (default: CPU count)\n"
              << "  --max-score N   Drop |score| above this (default: 10000)\n";
}

}  // namespace

int main(int argc, char** argv) {
    Options opt;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        auto next = [&]() -> std::string { return i + 1 < argc ? argv[++i] : ""; };
        if (a == "--input")
            opt.input = next();
        else if (a == "--output")
            opt.output = next();
        else if (a == "--label") {
            const std::string k = next();
            if (k == "hce")
                opt.label = nnue::LabelKind::hce_static;
            else if (k == "search")
                opt.label = nnue::LabelKind::search;
            else {
                std::cerr << "unknown --label " << k << std::endl;
                return 1;
            }
        } else if (a == "--threads")
            opt.threads = std::stoi(next());
        else if (a == "--max-score")
            opt.max_abs_score = std::stoi(next());
        else {
            usage(argv[0]);
            return 1;
        }
    }
    if (opt.input.empty()) {
        usage(argv[0]);
        return 1;
    }
    opt.threads = std::max(1, opt.threads);

    bitboards::init();
    magics::init();
    zobrist::init();
    kpk::init();

    std::ifstream in(opt.input);
    if (!in.is_open()) {
        std::cerr << "cannot open " << opt.input << std::endl;
        return 1;
    }
    std::ofstream out(opt.output, std::ios::binary);
    if (!out.is_open()) {
        std::cerr << "cannot write " << opt.output << std::endl;
        return 1;
    }

    // Reserve the header; count is only known at the end.
    nnue::FileHeader hdr{};
    std::memcpy(hdr.magic, "HVNN", 4);
    hdr.format_version = nnue::kDatasetFormatVersion;
    hdr.feature_set_version = nnue::kFeatureSetVersion;
    hdr.input_dim = nnue::kInputDim;
    hdr.max_active = nnue::kMaxActiveFeatures;
    hdr.record_bytes = sizeof(nnue::Record);
    hdr.label_kind = static_cast<uint32_t>(opt.label);
    out.write(reinterpret_cast<const char*>(&hdr), sizeof(hdr));

    std::cout << "haVoc nnue_export: " << opt.input << " -> " << opt.output << ", label="
              << (opt.label == nnue::LabelKind::hce_static ? "hce" : "search") << ", "
              << opt.threads << " threads" << std::endl;

    const auto t0 = std::chrono::steady_clock::now();
    const size_t kBatch = 65536;
    uint64_t written = 0, skipped = 0;

    std::vector<std::string> lines;
    lines.reserve(kBatch);
    std::vector<nnue::Record> recs(kBatch);
    std::vector<char> ok(kBatch);

    // Batched fan-out: reading is serial, encoding is not. Each worker owns
    // its own evaluator because the pawn and material caches are not shared.
    auto flush = [&]() {
        const size_t n = lines.size();
        if (n == 0)
            return;
        std::vector<std::thread> pool;
        const size_t chunk = (n + static_cast<size_t>(opt.threads) - 1) / static_cast<size_t>(opt.threads);
        for (int t = 0; t < opt.threads; ++t) {
            const size_t lo = std::min(n, static_cast<size_t>(t) * chunk), hi = std::min(n, lo + chunk);
            if (lo >= hi)
                break;
            pool.emplace_back([&, lo, hi]() {
                parameters params;
                params.sync_see_values();
                pawn_table pt(params);
                material_table mt(params);
                HCEEvaluator ev(pt, mt, params);
                for (size_t i = lo; i < hi; ++i)
                    ok[i] = encode_line(lines[i], opt.label, ev, opt.max_abs_score, recs[i]) ? 1 : 0;
            });
        }
        for (auto& th : pool)
            th.join();
        for (size_t i = 0; i < n; ++i) {
            if (ok[i]) {
                out.write(reinterpret_cast<const char*>(&recs[i]), sizeof(nnue::Record));
                ++written;
            } else
                ++skipped;
        }
        lines.clear();
    };

    std::string line;
    while (std::getline(in, line)) {
        if (line.empty())
            continue;
        lines.push_back(line);
        if (lines.size() == kBatch) {
            flush();
            std::cout << "  " << written << " encoded\r" << std::flush;
        }
    }
    flush();

    if (!out) {
        std::cerr << "\nnnue_export: write failed; " << opt.output << " is incomplete"
                  << std::endl;
        return 1;
    }
    hdr.count = written;
    out.seekp(0);
    out.write(reinterpret_cast<const char*>(&hdr), sizeof(hdr));
    out.close();

    const double secs =
        std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    std::cout << "\nDone: " << written << " records, " << skipped << " skipped, "
              << static_cast<int>(secs) << "s ("
              << static_cast<int>(static_cast<double>(written) / std::max(1.0, secs)) << "/sec)" << std::endl;
    return 0;
}
