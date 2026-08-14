/// @file texel_tune.cpp
#include "havoc/kpk.hpp"
/// @brief Texel tuner with pre-parsed positions, multi-threaded eval, checkpointing.

#include "havoc/bitboard.hpp"
#include "havoc/eval/hce.hpp"
#include "havoc/magics.hpp"
#include "havoc/material_table.hpp"
#include "havoc/movegen.hpp"
#include "havoc/parameters.hpp"
#include "havoc/pawn_table.hpp"
#include "havoc/position.hpp"
#include "havoc/zobrist.hpp"

#include <chrono>
#include <cmath>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

using namespace havoc;

/// Is this position quiet enough for a *static* evaluation to be meaningful?
///
/// Texel tuning compares a static evaluation against a game result. That only
/// makes sense where the static score is not about to be overturned by a
/// tactic. pgn2epd's filter is just "not in check", which keeps every position
/// with a hanging piece on it -- the static score there can be a whole piece
/// away from the truth, and the optimiser has no way to fix it by moving
/// weights, so it is pure label noise.
///
/// Requiring that no capture wins material by static exchange is the cheap
/// approximation of "the quiescence search would not move the score".
static bool is_quiet_position(position& p) {
    if (p.in_check())
        return false;
    Movegen mvs(p);
    mvs.generate<capture, pieces>();
    for (int i = 0; i < mvs.size(); ++i) {
        if (!p.is_legal(mvs[i]))
            continue;
        if (p.see(mvs[i]) > 0)
            return false;
    }
    return true;
}

struct TuningEntry { position pos; double result; };

static inline double sigmoid(double eval, double K) {
    return 1.0 / (1.0 + std::pow(10.0, -eval / K));
}

/// Convert an evaluation to white's point of view.
///
/// HCEEvaluator::evaluate() returns a *side-to-move* relative score (positive
/// means "good for whoever is to move"), but the training labels are the game
/// result from *white's* point of view (1.0 = white won), constant for every
/// position taken from that game. Comparing the two directly inverts the sign
/// of every black-to-move position, which is roughly half the data set. The
/// optimiser cannot represent such a target, so it drives the weights toward
/// the degenerate "always predict 0.5" solution instead of fitting anything.
static inline double to_white_pov(double stm_relative_eval, const position& p) {
    return p.to_move() == white ? stm_relative_eval : -stm_relative_eval;
}

class TexelTuner {
public:
    std::vector<TuningEntry> entries;
    parameters params;
    double cached_K = 0.0;
    int num_threads = 1;
    bool quiet_filter = true;
    double max_step_override = 0.0;

    bool load_data(const std::string& filename) {
        // is_quiet_position() filters the training set with static exchange
        // evaluation, so SEE has to be holding the same piece values the
        // evaluation is. parameters::load() syncs them, but the tuner is also
        // allowed to run from defaults with no parameter file at all.
        params.sync_see_values();
        auto t0 = std::chrono::steady_clock::now();
        std::ifstream in(filename);
        if (!in.is_open()) return false;
        std::string line;
        uint64_t loaded = 0, skipped = 0, noisy = 0;
        while (std::getline(in, line)) {
            auto c9 = line.find(" c9 ");
            if (c9 == std::string::npos) { ++skipped; continue; }
            auto q1 = line.find('"', c9);
            auto q2 = line.find('"', q1 + 1);
            if (q1 == std::string::npos || q2 == std::string::npos) { ++skipped; continue; }
            double result = std::stod(line.substr(q1 + 1, q2 - q1 - 1));
            std::istringstream fs(line.substr(0, c9));
            TuningEntry e; e.pos.setup(fs); e.result = result;
            if (quiet_filter && !is_quiet_position(e.pos)) { ++noisy; continue; }
            entries.push_back(std::move(e));
            if (++loaded % 100000 == 0)
                std::cout << "  Loaded " << loaded << " positions...\r" << std::flush;
        }
        auto s = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
        std::cout << "Loaded " << loaded << " positions in " << (int)s
                  << "s (" << skipped << " skipped";
        if (quiet_filter) std::cout << ", " << noisy << " not quiet";
        std::cout << ")" << std::endl;
        return !entries.empty();
    }

    double compute_error(double K) {
        const size_t N = entries.size();
        const int T = num_threads;
        std::vector<double> errs(T, 0.0);
        std::vector<std::thread> threads;
        auto work = [&](int tid) {
            pawn_table pt(params); material_table mt(params);
            HCEEvaluator ev(pt, mt, params);
            size_t a = (N * tid) / T, b = (N * (tid + 1)) / T;
            double e = 0.0;
            for (size_t i = a; i < b; ++i) {
                double raw = (double)ev.evaluate(entries[i].pos, -1);
                double p = sigmoid(to_white_pov(raw, entries[i].pos), K);
                double d = entries[i].result - p;
                e += d * d;
            }
            errs[tid] = e;
        };
        for (int t = 0; t < T; ++t) threads.emplace_back(work, t);
        for (auto& t : threads) t.join();
        double tot = 0; for (auto x : errs) tot += x;
        return tot / (double)N;
    }

    double find_optimal_K(bool force = false) {
        if (cached_K > 0 && !force) {
            std::cout << "Using cached K = " << cached_K << std::endl;
            return cached_K;
        }
        std::cout << "Finding optimal K..." << std::flush;
        auto t0 = std::chrono::steady_clock::now();
        // Range deliberately wide: a K pinned against the top of the search
        // interval silently flattens the sigmoid and shrinks every gradient,
        // which looks like "tuning converged" rather than "K was clamped".
        double lo = 10, hi = 4000;
        for (int i = 0; i < 30; ++i) {
            double m1 = lo + (hi - lo) / 3, m2 = hi - (hi - lo) / 3;
            if (compute_error(m1) < compute_error(m2)) hi = m2; else lo = m1;
        }
        cached_K = (lo + hi) / 2;
        auto s = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
        std::cout << " K = " << cached_K << " (err=" << compute_error(cached_K)
                  << ", " << (int)s << "s)" << std::endl;
        return cached_K;
    }

    /// Report the error under both sign conventions.
    ///
    /// This exists to make the perspective bug visible rather than theoretical.
    /// If the labels and the evaluation disagree about point of view, the
    /// "white POV" and "raw stm" errors will straddle 0.25 -- the error you get
    /// from predicting 0.5 for every position, i.e. from having learned
    /// nothing.
    void diagnose(double K) {
        const size_t N = entries.size();
        size_t black_to_move = 0;
        double err_white_pov = 0.0, err_raw = 0.0, mean_label = 0.0;

        pawn_table pt(params); material_table mt(params);
        HCEEvaluator ev(pt, mt, params);
        for (size_t i = 0; i < N; ++i) {
            if (entries[i].pos.to_move() != white) ++black_to_move;
            mean_label += entries[i].result;
            double raw = (double)ev.evaluate(entries[i].pos, -1);
            double dw = entries[i].result - sigmoid(to_white_pov(raw, entries[i].pos), K);
            double dr = entries[i].result - sigmoid(raw, K);
            err_white_pov += dw * dw;
            err_raw += dr * dr;
        }
        std::cout << "\n--- data diagnostics (K=" << K << ") ---\n"
                  << "  positions          : " << N << "\n"
                  << "  black to move      : " << black_to_move << " ("
                  << (100.0 * (double)black_to_move / (double)N) << "%)\n"
                  << "  mean label         : " << (mean_label / (double)N) << "\n"
                  << "  error (white POV)  : " << (err_white_pov / (double)N) << "\n"
                  << "  error (raw stm)    : " << (err_raw / (double)N) << "\n"
                  << "  error (predict 0.5): 0.25\n"
                  << "-------------------------------" << std::endl;
    }

    void optimize(int iters, TuneStage stage, const std::string& ckpt) {
        double K = find_optimal_K();
        diagnose(K);
        auto tunable = params.all_params(stage);
        const size_t NP = tunable.size();
        // Each parameter starts with the same step and adapts from there, so
        // the only thing a stage needs to declare is the first step to try.
        double max_step = max_step_override > 0 ? max_step_override : 16.0;
        struct Bounds { int lo, hi; };
        auto bounds = [](const std::string& n) -> Bounds {
            // These exist to keep a scale from going negative (which would
            // invert the sign of a whole category) or running away, not to
            // encode an opinion about the answer. The previous [10, 200] cap
            // did encode one, and it was binding: fits on two different
            // datasets sat on it, with pawn_structure_category_scale pinned
            // to the 10 floor on both, threat_category_scale on the 200
            // ceiling, and king_safety_category_scale at 199. Four of eleven
            // stage-1 parameters were resting against a cap, so the reported
            // fit was a property of the box rather than of the data. Zero is
            // allowed deliberately: "this category is worthless as shaped" is
            // a legitimate thing for the data to say.
            if (n.find("category_scale") != std::string::npos) return {0, 800};
            if (n.find("mobility_scale") != std::string::npos) return {0, 800};
            if (n == "king_danger_divisor") return {64, 1024};
            if (n.find("material_value") != std::string::npos) return {10, 30000};
            if (n.find("_scale") != std::string::npos) return {0, 800};
            return {-500, 500};
        };
        // The parameters are ints, but a step smaller than one has to be able
        // to accumulate across sweeps, so the real state is kept in doubles
        // and rounded only when written back into the evaluation.
        std::vector<double> shadow(NP);
        for (size_t i = 0; i < NP; ++i) shadow[i] = *tunable[i].second;

        double cur_err = compute_error(K);
        std::cout << "\nStage " << (int)stage+1 << " | Error: " << cur_err
                  << " | Params: " << NP << " | Max step: " << max_step
                  << " | Threads: " << num_threads << std::endl;

        // Coordinate descent with a per-parameter step size.
        //
        // Two gradient schemes were tried here first and both failed on the
        // same property of this problem. The finite-difference gradient is
        // extremely heterogeneous -- max/median |grad| measured 41 on one
        // iteration and 188 on the next -- and the parameters are integers, so
        // normalising the direction by its largest coordinate moves the median
        // coordinate by step/188, which rounds to zero. A "successful"
        // iteration then updated one parameter out of 273 after paying 546
        // error evaluations. Stepping by the sign of each gradient removes the
        // scale problem but exposes a second one: with median |grad| at 1.3e-6
        // against a maximum of 5.2e-5, a large share of the signs are noise,
        // and moving all 259 of them together is worse than not moving at all,
        // even at a step of one centipawn.
        //
        // Coordinate descent avoids both. Each parameter is changed on its own
        // and kept only if the error over the whole dataset actually falls, so
        // a noisy coordinate costs one wasted evaluation instead of corrupting
        // the step. A sweep costs the same 2*NP evaluations the gradient did,
        // but it can improve many parameters instead of one. Each parameter
        // carries its own step, halved whenever neither direction helps, which
        // lets a weight that needs to move 40 centipawns get there in a few
        // sweeps while a weight that is already right settles quickly.
        std::vector<double> pstep(NP, max_step);
        int total_improved = 0;
        for (int it = 0; it < iters; ++it) {
            auto t0 = std::chrono::steady_clock::now();
            std::cout << "\n=== Sweep " << it + 1 << " ===" << std::endl;
            int improved = 0;
            for (size_t i = 0; i < NP; ++i) {
                if (pstep[i] < 0.5) continue;
                auto b = bounds(tunable[i].first);
                const int base = *tunable[i].second;
                bool ok = false;
                for (double dir : {1.0, -1.0}) {
                    double target = shadow[i] + dir * pstep[i];
                    long nv = std::lround(target);
                    nv = std::max((long)b.lo, std::min((long)b.hi, nv));
                    if ((int)nv == base) continue;
                    *tunable[i].second = (int)nv;
                    double e = compute_error(K);
                    if (e < cur_err) {
                        cur_err = e;
                        shadow[i] = std::max((double)b.lo, std::min((double)b.hi, target));
                        ++improved;
                        ok = true;
                        break;
                    }
                    *tunable[i].second = base;
                }
                if (!ok) pstep[i] *= 0.5;
                if (NP > 20 && (i + 1) % 10 == 0)
                    std::cout << "  sweep: " << i + 1 << "/" << NP << "\r" << std::flush;
            }
            if (NP > 20) std::cout << "  sweep: " << NP << "/" << NP << "    " << std::endl;

            int live = 0;
            for (size_t i = 0; i < NP; ++i)
                if (pstep[i] >= 0.5) ++live;
            total_improved += improved;
            auto s = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
            std::cout << "Error: " << cur_err << "  (" << improved << "/" << NP
                      << " improved, " << live << " still live, " << (int)s << "s)" << std::endl;
            if (!ckpt.empty()) {
                params.save(ckpt);
                std::cout << "  Checkpoint: " << ckpt << std::endl;
            }
            if (live == 0) {
                std::cout << "Every step size is exhausted; no further integer move helps."
                          << std::endl;
                break;
            }
        }
        std::cout << "\nTotal accepted parameter changes: " << total_improved << std::endl;
    }
};

int main(int argc, char* argv[]) {
    std::string data = "training_data.epd", pfile, out = "tuned_params.txt";
    int iters = 5, stg = 2, thr = (int)std::thread::hardware_concurrency();
    bool qfilter = true; double lr_ov = 0.0;
    double fK = 0;
    for (int i = 1; i < argc; ++i) {
        std::string k = argv[i];
        if ((k=="--data"||k=="-d") && i+1<argc) data = argv[++i];
        else if ((k=="--params"||k=="-p") && i+1<argc) pfile = argv[++i];
        else if ((k=="--output"||k=="-o") && i+1<argc) out = argv[++i];
        else if ((k=="--iterations"||k=="-i") && i+1<argc) iters = std::stoi(argv[++i]);
        else if ((k=="--stage"||k=="-s") && i+1<argc) stg = std::stoi(argv[++i]);
        else if (k=="--K" && i+1<argc) fK = std::stod(argv[++i]);
        else if ((k=="--threads"||k=="-t") && i+1<argc) thr = std::stoi(argv[++i]);
        else if (k=="--no-quiet-filter") qfilter = false;
        else if ((k=="--max-step"||k=="--lr") && i+1<argc) lr_ov = std::stod(argv[++i]);
        else if (k=="--help"||k=="-h") {
            std::cerr << "Usage: " << argv[0] << " --data FILE [--params FILE] [--output FILE] "
                      << "[--iterations N] [--stage 1|2|3|4] [--K val] [--threads N] "
                      << "[--no-quiet-filter] [--max-step F]\n"; return 0;
        }
    }
    auto stage = (stg==1 ? TuneStage::category : stg==3 ? TuneStage::fine
                : stg==4 ? TuneStage::pst : TuneStage::shape);
    bitboards::init(); magics::init(); zobrist::init(); kpk::init();
    TexelTuner tuner; tuner.num_threads = std::max(1, thr); tuner.quiet_filter = qfilter;
    tuner.max_step_override = lr_ov;
    if (!pfile.empty() && tuner.params.load(pfile))
        std::cout << "Loaded params from " << pfile << std::endl;
    if (!tuner.load_data(data)) { std::cerr << "Failed to load " << data << std::endl; return 1; }
    if (fK > 0) { tuner.cached_K = fK; std::cout << "Fixed K = " << fK << std::endl; }
    tuner.optimize(iters, stage, out);
    tuner.params.save(out);
    std::cout << "\nSaved to " << out << std::endl;
    return 0;
}
