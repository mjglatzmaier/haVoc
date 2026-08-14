/// @file eval_bench.cpp
/// @brief Score the evaluation on the property that actually converts to Elo:
///        telling two equal-material positions apart, for the right reason.
///
/// Why this exists
/// ---------------
/// The three instruments already pointed at this evaluation each miss the
/// thing that decides moves.
///
///   Texel error measures global calibration and is dominated by material. A
///   refit cut held-out error 7.6% relative and measured +1.4 +/- 19.9 Elo
///   over 781 games -- an exact 0.500 score. Worse, its gradient landscape is
///   independent of the current values, so refitting from better defaults
///   converges right back to the same place. It cannot see positional terms
///   because they live below its resolution and because its quiet filter
///   discards the 22% of positions where tactical terms fire.
///
///   An SPRT measures the truth but costs thousands of games and never says
///   which term was wrong.
///
///   eval_sensitivity measures what each term is *worth* in the aggregate, but
///   a term can be worth a lot and still be worth it in the wrong places.
///
/// Between them sits the property that chooses moves: two positions one ply
/// apart almost always have identical material, so the engine's entire ability
/// to choose between them rests on positional terms. This tool measures four
/// things about that ability, on the shared corpus in eval_pairs.hpp.
///
///   ordering      does the better position score higher? The bare
///                 pass/fail the regression test already asserts.
///
///   margin        by how much. A pair decided by 1cp is not really decided:
///                 it will be swamped by the first piece-square accident, and
///                 it sits below the granularity at which search margins and
///                 move ordering can act on it. Ordering without margin is a
///                 coin flip that happened to land right.
///
///   attribution   *which* parameter produced the difference, measured by
///                 deleting each parameter and seeing how much of the margin
///                 disappears with it. This is the question the ordering
///                 assertion cannot ask. A pair built to probe rook activity
///                 that is actually decided by a piece-square table is not
///                 evidence about rooks -- it is a coincidence that will
///                 reverse the moment something unrelated is retuned.
///
///   balance       how concentrated the attribution is. A margin carried
///                 entirely by one term is a term doing its job. A margin
///                 assembled from twenty terms of a quarter-centipawn each is
///                 an evaluation with no opinion, and it is fragile: any of
///                 the twenty can flip it.
///
/// None of these needs a single game to compute, so the whole scorecard runs
/// in under a second and can gate every change to the evaluation.
///
/// Reading the output: `share` is the fraction of the margin a term accounts
/// for. Shares can exceed 100% or go negative, because terms are not additive
/// once category scales and phase interpolation are involved -- a negative
/// share is a term actively arguing *against* the correct answer, which is
/// usually the most interesting line in the report.

#include "havoc/bitboard.hpp"
#include "havoc/eval/eval_pairs.hpp"
#include "havoc/eval/hce.hpp"
#include "havoc/movegen.hpp"
#include "havoc/kpk.hpp"
#include "havoc/magics.hpp"
#include "havoc/material_table.hpp"
#include "havoc/parameters.hpp"
#include "havoc/pawn_table.hpp"
#include "havoc/position.hpp"
#include "havoc/zobrist.hpp"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <fstream>
#include <map>
#include <iomanip>
#include <iostream>
#include <limits>
#include <numeric>
#include <sstream>
#include <string>
#include <vector>

using namespace havoc;

namespace {

/// Four parameters are integer divisors. Setting one to zero divides by zero;
/// the sensitivity tool learned this by crashing. To ask "does this term carry
/// the pair" we weaken it instead, by multiplying the divisor, which shrinks
/// the term it governs without ever reaching an illegal value.
bool is_divisor(const std::string& name) {
    return name.find("divisor") != std::string::npos;
}


/// Load an EPD/FEN corpus, one position per line.
std::vector<position> load_corpus(const std::string& file, std::size_t limit) {
    std::vector<position> out;
    std::ifstream in(file);
    if (!in.is_open()) {
        std::cerr << "cannot open " << file << "\n";
        return out;
    }
    std::string line;
    while (out.size() < limit && std::getline(in, line)) {
        if (line.empty())
            continue;
        auto c9 = line.find(" c9 ");
        std::istringstream fs(c9 == std::string::npos ? line : line.substr(0, c9));
        position p;
        p.setup(fs);
        out.push_back(std::move(p));
    }
    return out;
}

std::string family_of(const std::string& name) {
    std::size_t i = name.size();
    while (i > 0 && std::isdigit(static_cast<unsigned char>(name[i - 1])))
        --i;
    if (i > 0 && i < name.size() && name[i - 1] == '_')
        --i;
    return name.substr(0, i);
}

position make_pos(const std::string& fen) {
    std::istringstream iss(fen);
    return position(iss);
}

struct Contribution {
    std::string name;
    double delta = 0.0; // centipawns of margin that vanish with this term
};

struct Result {
    std::string feature;
    std::string expects;
    int margin = 0;
    bool ordered = false;
    bool decisive = false;
    bool attributed = false;
    double top_share = 0.0;
    std::vector<Contribution> top;
};


/// Does this parameter change what the engine would actually *play*?
///
/// Every other measurement in this file, and in eval_sensitivity, reports
/// centipawns. Centipawns are not what wins games. A parameter that shifts
/// every position in a family by the same amount moves the score a great deal
/// and changes no decision at all, because the engine picks the argmax over
/// sibling positions and a common offset cancels in that comparison.
///
/// That is the difference between a level and a preference, and it is the
/// most likely explanation for a result that keeps recurring here: sweeping
/// parameter changes that measure large in centipawns and exactly neutral in
/// games. A category scale multiplies a whole family, and sibling positions
/// one ply apart usually share most of that family, so most of the change
/// divides out of the comparison that actually selects the move.
///
/// So measure the thing directly. For each position, generate the legal moves,
/// score the resulting positions, and record which move the evaluation
/// prefers. Then perturb one parameter and see how often that preference
/// changes. `flip` is the fraction of positions where the engine would now
/// play a different move -- an upper bound on how much Elo the parameter can
/// possibly be worth, and a hard zero is proof that it cannot be worth any.
int run_decisions(const std::string& file, std::size_t limit, parameters& params,
                  pawn_table& pt, material_table& mt, HCEEvaluator& ev, int top_n) {
    auto roots = load_corpus(file, limit);
    if (roots.empty())
        return 1;

    // Generate once; replaying do_move/undo_move per probe is far cheaper than
    // regenerating, and it uses the engine's own idiom rather than relying on
    // position copy semantics.
    std::vector<std::vector<Move>> moves(roots.size());
    std::size_t total = 0;
    for (std::size_t i = 0; i < roots.size(); ++i) {
        Movegen mvs(roots[i]);
        mvs.generate<pseudo_legal, pieces>();
        for (int j = 0; j < mvs.size(); ++j)
            if (roots[i].is_legal(mvs[j]))
                moves[i].push_back(mvs[j]);
        total += moves[i].size();
    }

    // The evaluation returns a score from the side to move's point of view, so
    // a child position is scored for the opponent and has to be negated to
    // rank it for the side choosing the move.
    auto preferences = [&](std::vector<int>& out) {
        pt.clear();
        mt.clear();
        out.assign(roots.size(), -1);
        for (std::size_t i = 0; i < roots.size(); ++i) {
            int best = std::numeric_limits<int>::min();
            for (std::size_t j = 0; j < moves[i].size(); ++j) {
                roots[i].do_move(moves[i][j]);
                const int s = -ev.evaluate(roots[i]);
                roots[i].undo_move(moves[i][j]);
                if (s > best) {
                    best = s;
                    out[i] = static_cast<int>(j);
                }
            }
        }
    };

    std::vector<int> base, probe;
    preferences(base);

    std::vector<std::string> search_names;
    for (auto& [name, slot] : params.all_params(TuneStage::search))
        search_names.push_back(name);

    std::map<std::string, std::pair<double, int>> fam; // family -> (flips, params)
    std::vector<std::pair<std::string, double>> rows;

    for (auto& [name, slot] : params.every_param()) {
        if (std::find(search_names.begin(), search_names.end(), name) != search_names.end())
            continue;
        const int original = *slot;
        if (original == 0 && !is_divisor(name))
            continue;

        if (is_divisor(name))
            *slot = original * 16;
        else
            *slot = 0;
        preferences(probe);
        *slot = original;

        std::size_t flips = 0;
        for (std::size_t i = 0; i < base.size(); ++i)
            flips += (base[i] != probe[i]);
        const double pct = 100.0 * double(flips) / double(base.size());

        rows.push_back({name, pct});
        auto& f = fam[family_of(name)];
        f.first += pct;
        f.second += 1;
    }

    std::cout << "\ndecision sensitivity\n";
    std::cout << "  positions " << roots.size() << "   legal moves " << total
              << "   mean branching " << (total / roots.size()) << "\n";
    std::cout << "  flip = % of positions where deleting the term changes the preferred move\n\n";

    std::vector<std::pair<std::string, double>> fam_rows;
    for (auto& [k, v] : fam)
        fam_rows.push_back({k, v.first});
    std::sort(fam_rows.begin(), fam_rows.end(),
              [](auto& a, auto& b) { return a.second > b.second; });

    std::cout << "family                                   flip%\n";
    std::cout << "----------------------------------------------\n";
    for (int i = 0; i < top_n && i < int(fam_rows.size()); ++i)
        std::cout << std::left << std::setw(40) << fam_rows[i].first.substr(0, 39) << std::right
                  << std::setw(7) << std::fixed << std::setprecision(1) << fam_rows[i].second
                  << "\n";

    std::size_t dead = 0;
    for (auto& r : rows)
        dead += (r.second == 0.0);
    std::cout << "\n  parameters probed        " << rows.size() << "\n";
    std::cout << "  changing no decision    " << dead << "  ("
              << (100 * dead / (rows.empty() ? 1 : rows.size())) << "%)\n\n";
    return 0;
}

} // namespace

int main(int argc, char** argv) {
    std::string decisions_file;
    std::size_t decisions_limit = 300;
    int min_margin = 8;
    int show = 3;
    bool verbose = false;
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        if (a == "--decisions" && i + 1 < argc)
            decisions_file = argv[++i];
        else if (a == "--positions" && i + 1 < argc)
            decisions_limit = std::strtoul(argv[++i], nullptr, 10);
        else if (a == "--min-margin" && i + 1 < argc)
            min_margin = std::atoi(argv[++i]);
        else if (a == "--show" && i + 1 < argc)
            show = std::atoi(argv[++i]);
        else if (a == "--verbose" || a == "-v")
            verbose = true;
    }

    bitboards::init();
    magics::init();
    zobrist::init();
    kpk::init();

    parameters params;
    params.sync_see_values();
    pawn_table pt(params);
    material_table mt(params);
    HCEEvaluator ev(pt, mt, params);

    // The pawn and material tables cache by position hash, not by parameter
    // value, so every perturbation has to invalidate them or the measurement
    // silently reads scores computed under the previous parameters.
    auto margin_of = [&](position& better, position& worse) {
        pt.clear();
        mt.clear();
        const int b = ev.evaluate(better);
        const int w = ev.evaluate(worse);
        return b - w;
    };

    if (!decisions_file.empty())
        return run_decisions(decisions_file, decisions_limit, params, pt, mt, ev, show > 3 ? show : 20);

    auto slots = params.every_param();
    std::vector<std::string> search_names;
    for (auto& [name, slot] : params.all_params(TuneStage::search))
        search_names.push_back(name);
    auto is_search = [&](const std::string& n) {
        return std::find(search_names.begin(), search_names.end(), n) != search_names.end();
    };

    std::vector<Result> results;

    for (const auto& c : eval_pairs::all()) {
        auto better = make_pos(c.better);
        auto worse = make_pos(c.worse);

        Result r;
        r.feature = c.feature;
        r.expects = c.expects;
        r.margin = margin_of(better, worse);
        r.ordered = r.margin > 0;
        r.decisive = r.margin >= min_margin;

        std::vector<Contribution> contribs;
        for (auto& [name, slot] : slots) {
            if (is_search(name))
                continue;
            const int original = *slot;
            if (original == 0 && !is_divisor(name))
                continue; // already off: it cannot be carrying anything

            if (is_divisor(name))
                *slot = original * 16; // weaken, never divide by zero
            else
                *slot = 0;

            const int probe = margin_of(better, worse);
            *slot = original;

            const double d = static_cast<double>(r.margin - probe);
            if (d != 0.0)
                contribs.push_back({name, d});
        }

        // Aggregate by family before ranking. A table like king_shelter is one
        // idea spread over several entries, and a pair can move two of those
        // entries in opposite directions -- +7 on the near slot, -4 on the far
        // one -- so no single element beats an unrelated scalar even when the
        // family as a whole is what decided the pair. Ranking parameters alone
        // reports that as "king safety did not carry this", which is false.
        std::map<std::string, double> by_family;
        for (const auto& c2 : contribs)
            by_family[family_of(c2.name)] += c2.delta;
        std::vector<Contribution> fams;
        for (auto& [k, v] : by_family)
            fams.push_back({k, v});
        std::sort(fams.begin(), fams.end(),
                  [](const Contribution& a, const Contribution& b) {
                      return std::abs(a.delta) > std::abs(b.delta);
                  });
        contribs = fams;

        const double denom = r.margin != 0 ? std::abs(static_cast<double>(r.margin)) : 1.0;
        if (!contribs.empty())
            r.top_share = contribs.front().delta / denom;

        for (int i = 0; i < show && i < static_cast<int>(contribs.size()); ++i)
            r.top.push_back(contribs[i]);

        if (!r.expects.empty())
            for (const auto& c2 : r.top)
                if (c2.name.find(r.expects) != std::string::npos)
                    r.attributed = true;

        results.push_back(std::move(r));
    }

    // ---- report -----------------------------------------------------------

    std::cout << "\nevaluation discrimination scorecard\n";
    std::cout << "  pairs " << results.size() << "   decisive threshold " << min_margin
              << "cp\n\n";

    for (const auto& r : results) {
        std::cout << (r.ordered ? "  ok  " : "  XX  ") << std::left << std::setw(44)
                  << r.feature.substr(0, 43) << " margin " << std::right << std::setw(5)
                  << r.margin << "cp";
        if (!r.decisive && r.ordered)
            std::cout << "  (thin)";
        std::cout << "\n";

        for (const auto& c : r.top) {
            const double share = r.margin != 0 ? 100.0 * c.delta / std::abs(double(r.margin)) : 0.0;
            std::cout << "        " << std::left << std::setw(38) << c.name.substr(0, 37)
                      << std::right << std::setw(7) << std::fixed << std::setprecision(1)
                      << c.delta << "cp " << std::setw(6) << share << "%\n";
        }
        if (!r.expects.empty() && !r.attributed)
            std::cout << "        !! expected " << r.expects
                      << " to carry this pair; it is not in the top " << show << "\n";
        if (r.top.empty())
            std::cout << "        !! no parameter accounts for this margin\n";
        std::cout << "\n";
    }

    int ordered = 0, decisive = 0, attributed = 0, with_expect = 0;
    std::vector<int> margins;
    double share_sum = 0.0;
    for (const auto& r : results) {
        ordered += r.ordered;
        decisive += r.decisive;
        if (!r.expects.empty()) {
            ++with_expect;
            attributed += r.attributed;
        }
        margins.push_back(std::abs(r.margin));
        share_sum += r.top_share;
    }
    std::sort(margins.begin(), margins.end());
    const int median = margins.empty() ? 0 : margins[margins.size() / 2];

    std::cout << "summary\n";
    std::cout << "  ordered correctly   " << ordered << "/" << results.size() << "\n";
    std::cout << "  decisive (>=" << min_margin << "cp)   " << decisive << "/" << results.size()
              << "\n";
    std::cout << "  correctly attributed " << attributed << "/" << with_expect << "\n";
    std::cout << "  median margin       " << median << "cp\n";
    std::cout << "  mean top-term share " << std::fixed << std::setprecision(0)
              << 100.0 * share_sum / double(results.size()) << "%\n\n";

    if (verbose)
        std::cout << "a negative share means the term argues against the correct answer\n\n";

    return ordered == static_cast<int>(results.size()) ? 0 : 1;
}
