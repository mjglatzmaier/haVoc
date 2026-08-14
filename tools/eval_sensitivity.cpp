/// @file eval_sensitivity.cpp
/// @brief Measure how many centipawns each evaluation parameter is actually
///        worth, over a corpus of real positions.
///
/// Why
/// ---
/// A hand-crafted evaluation accumulates terms faster than it accumulates
/// evidence about them. Terms get added, scaled by a category factor, scaled
/// again by a per-piece factor, and tuned by a fitter that only ever sees the
/// sum. Nothing in that process reports what any individual term ends up being
/// worth, so it is entirely possible -- and the suspicion that prompted this
/// tool -- for real chess knowledge to be present in the source, registered
/// with the tuner, reachable by a unit test, and still contribute a fraction
/// of a centipawn to any decision the engine ever makes.
///
/// Two numbers per parameter, both averaged over the corpus:
///
///   worth   mean |eval(param := 0) - eval(param)|, the centipawns that
///           vanish if the term is deleted outright. This is the honest answer
///           to "how much is this knowledge worth to the engine as shipped".
///           It is meaningless for a parameter whose default is already 0.
///
///   grad    mean |eval(param + 1) - eval(param)|, the centipawns one unit of
///           the parameter buys. This is what a tuner sees. A parameter with a
///           tiny grad cannot be fitted at all: coordinate descent needs a
///           strict improvement, and integer division will swallow the step.
///
///   reach   fraction of positions where the parameter changes the evaluation
///           at all. A term can be perfectly well implemented and still be
///           dead weight because the shape it looks for is rare.
///
/// Output is grouped by family, since a single family (a mobility table, a
/// piece-square table) is the unit a person actually reasons about.

#include "havoc/bitboard.hpp"
#include "havoc/eval/hce.hpp"
#include "havoc/kpk.hpp"
#include "havoc/magics.hpp"
#include "havoc/material_table.hpp"
#include "havoc/parameters.hpp"
#include "havoc/pawn_table.hpp"
#include "havoc/position.hpp"
#include "havoc/zobrist.hpp"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <map>
#include <sstream>
#include <string>
#include <vector>

using namespace havoc;

namespace {

struct Sens {
    std::string name;
    int def = 0;
    double worth = 0.0;
    double grad = 0.0;
    double reach = 0.0;
};

/// A family is the parameter name with any trailing _<digits> stripped, so the
/// 64 entries of a piece-square table or the 28 rungs of a mobility ladder are
/// reported as one thing.
std::string family_of(const std::string& name) {
    std::size_t end = name.size();
    while (end > 0 && std::isdigit(static_cast<unsigned char>(name[end - 1])))
        --end;
    if (end > 0 && end < name.size() && name[end - 1] == '_')
        return name.substr(0, end - 1);
    return name;
}

/// Four parameters are used as integer divisors in the evaluation
/// (pinned_scaling for bishop, rook and queen, and king_danger_divisor).
/// Setting one to 0 to see what it is worth divides by zero and takes the
/// process down, so the deletion probe is skipped for them and only the
/// gradient is reported.
///
/// That they are divisors at all is worth noticing: an integer divisor can
/// only ever be 1, 2, 3..., so the smallest change a tuner can make to
/// pinned_scaling is to halve the term. There is no fine adjustment available.
bool is_divisor(const std::string& name) {
    return name == "king_danger_divisor" || name.rfind("pinned_scaling", 0) == 0;
}

std::vector<position> load(const std::string& file, std::size_t limit) {
    std::vector<position> out;
    std::ifstream in(file);
    if (!in.is_open()) {
        std::cerr << "cannot open " << file << "\n";
        return out;
    }
    std::string line;
    while (out.size() < limit && std::getline(in, line)) {
        auto c9 = line.find(" c9 ");
        std::istringstream fs(c9 == std::string::npos ? line : line.substr(0, c9));
        position p;
        p.setup(fs);
        out.push_back(std::move(p));
    }
    return out;
}

} // namespace

int main(int argc, char** argv) {
    std::string file = "/tmp/val200.epd";
    std::size_t limit = 3000;
    bool by_param = false;
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        if (a == "--positions" && i + 1 < argc)
            limit = std::strtoul(argv[++i], nullptr, 10);
        else if (a == "--by-param")
            by_param = true;
        else
            file = a;
    }

    bitboards::init();
    magics::init();
    zobrist::init();
    kpk::init();

    auto corpus = load(file, limit);
    if (corpus.empty()) {
        std::cerr << "no positions loaded\n";
        return 1;
    }

    parameters params;
    params.sync_see_values();
    pawn_table pt(params);
    material_table mt(params);
    HCEEvaluator ev(pt, mt, params);

    // The pawn and material tables cache by position hash, not by parameter
    // value, so every perturbation has to invalidate them or the measurement
    // silently reads scores computed under the previous parameters.
    auto eval_all = [&](std::vector<int>& out) {
        pt.clear();
        mt.clear();
        out.clear();
        out.reserve(corpus.size());
        for (auto& p : corpus)
            out.push_back(ev.evaluate(p));
    };

    std::vector<int> base, probe;
    eval_all(base);

    auto slots = params.every_param();
    std::vector<std::string> search_names;
    for (auto& [name, slot] : params.all_params(TuneStage::search))
        search_names.push_back(name);

    std::vector<Sens> rows;
    rows.reserve(slots.size());

    std::size_t done = 0;
    for (auto& [name, slot] : slots) {
        if (std::find(search_names.begin(), search_names.end(), name) != search_names.end())
            continue;

        const int original = *slot;
        Sens r;
        r.name = name;
        r.def = original;

        // What the term is worth: delete it and see what moves.
        if (original != 0 && !is_divisor(name)) {
            *slot = 0;
            eval_all(probe);
            double sum = 0.0;
            std::size_t hits = 0;
            for (std::size_t i = 0; i < base.size(); ++i) {
                const int d = std::abs(probe[i] - base[i]);
                sum += d;
                hits += (d != 0);
            }
            r.worth = sum / static_cast<double>(base.size());
            r.reach = static_cast<double>(hits) / static_cast<double>(base.size());
        }

        // What a tuner sees: one unit.
        *slot = original + 1;
        eval_all(probe);
        double gsum = 0.0;
        std::size_t ghits = 0;
        for (std::size_t i = 0; i < base.size(); ++i) {
            const int d = std::abs(probe[i] - base[i]);
            gsum += d;
            ghits += (d != 0);
        }
        r.grad = gsum / static_cast<double>(base.size());
        if (original == 0)
            r.reach = static_cast<double>(ghits) / static_cast<double>(base.size());

        *slot = original;
        rows.push_back(r);

        if (++done % 50 == 0)
            std::cerr << "  " << done << " parameters\r" << std::flush;
    }
    std::cerr << "                         \r";

    std::cout << "positions: " << corpus.size() << "  parameters: " << rows.size() << "\n\n";

    if (by_param) {
        std::sort(rows.begin(), rows.end(),
                  [](const Sens& a, const Sens& b) { return a.worth > b.worth; });
        std::cout << std::left << std::setw(36) << "parameter" << std::right << std::setw(8)
                  << "default" << std::setw(10) << "worth" << std::setw(10) << "grad"
                  << std::setw(9) << "reach" << "\n";
        for (const auto& r : rows)
            std::cout << std::left << std::setw(36) << r.name << std::right << std::setw(8) << r.def
                      << std::setw(10) << std::fixed << std::setprecision(2) << r.worth
                      << std::setw(10) << std::setprecision(3) << r.grad << std::setw(8)
                      << std::setprecision(1) << (r.reach * 100.0) << "%\n";
        return 0;
    }

    // Family view. worth is summed, because deleting a whole family removes
    // every member; grad and reach are reported at their maximum, since the
    // question there is whether *any* member of the family is tunable.
    struct Fam {
        double worth = 0.0;
        double grad = 0.0;
        double reach = 0.0;
        int members = 0;
        int frozen = 0;
    };
    std::map<std::string, Fam> fams;
    for (const auto& r : rows) {
        auto& f = fams[family_of(r.name)];
        f.worth += r.worth;
        f.grad = std::max(f.grad, r.grad);
        f.reach = std::max(f.reach, r.reach);
        ++f.members;
        f.frozen += (r.grad == 0.0);
    }

    std::vector<std::pair<std::string, Fam>> ordered(fams.begin(), fams.end());
    std::sort(ordered.begin(), ordered.end(),
              [](const auto& a, const auto& b) { return a.second.worth > b.second.worth; });

    std::cout << std::left << std::setw(34) << "family" << std::right << std::setw(6) << "n"
              << std::setw(8) << "frozen" << std::setw(11) << "worth" << std::setw(10) << "grad"
              << std::setw(9) << "reach" << "\n";
    std::cout << std::string(78, '-') << "\n";
    for (const auto& [name, f] : ordered)
        std::cout << std::left << std::setw(34) << name << std::right << std::setw(6) << f.members
                  << std::setw(8) << f.frozen << std::setw(11) << std::fixed
                  << std::setprecision(2) << f.worth << std::setw(10) << std::setprecision(3)
                  << f.grad << std::setw(8) << std::setprecision(1) << (f.reach * 100.0) << "%\n";

    return 0;
}
