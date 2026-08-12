#include "havoc/parameters.hpp"

#include <fstream>
#include <iostream>
#include <sstream>
#include <string>

namespace havoc {

bool parameters::load(const std::string& filename) {
    std::ifstream in(filename);
    if (!in.is_open())
        return false;

    std::string line;
    // save() and load() share one definition of "every tunable". When they
    // disagreed, whole groups were written to disk and silently ignored on the
    // way back in -- including when a tuned file was loaded into the engine
    // over the UCI 'ParamFile' path.
    auto params = every_param();

    std::size_t applied = 0, unknown = 0;
    while (std::getline(in, line)) {
        if (line.empty() || line[0] == '#')
            continue;
        auto eq = line.find('=');
        if (eq == std::string::npos)
            continue;

        std::string key = line.substr(0, eq);
        std::string val = line.substr(eq + 1);

        // Trim whitespace
        auto trim = [](std::string& s) {
            while (!s.empty() && s.front() == ' ')
                s.erase(s.begin());
            while (!s.empty() && s.back() == ' ')
                s.pop_back();
        };
        trim(key);
        trim(val);

        bool matched = false;
        for (auto& [name, ptr] : params) {
            if (name == key) {
                try {
                    *ptr = std::stoi(val);
                    matched = true;
                } catch (const std::exception&) {
                    // Leave the parameter at its current value rather than
                    // aborting: a single malformed line should not discard an
                    // otherwise usable file.
                }
                break;
            }
        }
        matched ? ++applied : ++unknown;
    }

    if (unknown > 0)
        std::cerr << "info string load_params: " << applied << " applied, " << unknown
                  << " unrecognised key(s) ignored" << std::endl;
    return applied > 0;
}

bool parameters::save(const std::string& filename) const {
    std::ofstream out(filename);
    if (!out.is_open())
        return false;

    for (const auto& [name, ptr] : const_cast<parameters*>(this)->every_param())
        out << name << " = " << *ptr << "\n";
    return true;
}

std::vector<std::pair<std::string, int*>> parameters::all_params(TuneStage stage) {
    std::vector<std::pair<std::string, int*>> result;

    if (stage == TuneStage::category) {
        // Stage 1: Category-level scale factors only. Tempo is FROZEN here
        // to prevent the bias term from absorbing scale changes.
        result.emplace_back("sq_score_category_scale", &sq_score_category_scale);
        result.emplace_back("mobility_category_scale", &mobility_category_scale);
        result.emplace_back("king_safety_category_scale", &king_safety_category_scale);
        result.emplace_back("threat_category_scale", &threat_category_scale);
        result.emplace_back("passed_pawn_category_scale", &passed_pawn_category_scale);
        result.emplace_back("pawn_structure_category_scale", &pawn_structure_category_scale);
        result.emplace_back("space_category_scale", &space_category_scale);
        result.emplace_back("king_danger_divisor", &king_danger_divisor);
        return result;
    }

    if (stage == TuneStage::shape) {
        // Stage 2: Individual weights and curve shapes (~50-100 params)
        // NOTE: tempo is excluded — it's a bias term that interferes with
        // weight tuning. Set it manually to a small value (5-15cp).

        // Per-piece mobility scale factors
        result.emplace_back("knight_mobility_scale", &knight_mobility_scale);
        result.emplace_back("bishop_mobility_scale", &bishop_mobility_scale);
        result.emplace_back("rook_mobility_scale", &rook_mobility_scale);

        // Mobility curve entries
        for (size_t i = 0; i < knight_mobility_table.size(); ++i)
            result.emplace_back("knight_mobility_" + std::to_string(i),
                                &knight_mobility_table[i]);
        for (size_t i = 0; i < bishop_mobility_table.size(); ++i)
            result.emplace_back("bishop_mobility_" + std::to_string(i),
                                &bishop_mobility_table[i]);
        for (size_t i = 0; i < rook_mobility_table.size(); ++i)
            result.emplace_back("rook_mobility_" + std::to_string(i),
                                &rook_mobility_table[i]);

        // Endgame scaling
        result.emplace_back("opposite_bishop_scale", &opposite_bishop_scale);
        result.emplace_back("no_pawn_scale", &no_pawn_scale);
        result.emplace_back("minor_advantage_no_pawn_scale", &minor_advantage_no_pawn_scale);

        // Passed pawn rank bonuses
        for (size_t i = 0; i < passed_pawn_rank_bonus.size(); ++i)
            result.emplace_back("passed_pawn_rank_bonus_" + std::to_string(i),
                                &passed_pawn_rank_bonus[i]);

        // Attacker weights
        for (size_t i = 0; i < attacker_weight.size(); ++i)
            result.emplace_back("attacker_weight_" + std::to_string(i), &attacker_weight[i]);

        // King shelter
        for (size_t i = 0; i < king_shelter.size(); ++i)
            result.emplace_back("king_shelter_" + std::to_string(i), &king_shelter[i]);

        // King safe squares
        for (size_t i = 0; i < king_safe_sqs.size(); ++i)
            result.emplace_back("king_safe_sqs_" + std::to_string(i), &king_safe_sqs[i]);

        result.emplace_back("bishop_own_pawn_penalty_mg", &bishop_own_pawn_penalty_mg);
        result.emplace_back("bishop_own_pawn_penalty_eg", &bishop_own_pawn_penalty_eg);

        result.emplace_back("uncastled_penalty", &uncastled_penalty);
        return result;
    }

    if (stage == TuneStage::pst) {
        // Stage 4: the piece-square tables themselves -- 2 phases x 6 pieces
        // x 64 squares. By far the largest group of weights in the evaluation
        // and, until they were moved out of squares.hpp, the only major group
        // the tuner could not see.
        static const char* piece_names[6] = {"pawn", "knight", "bishop",
                                             "rook", "queen",  "king"};
        for (size_t pc = 0; pc < 6; ++pc)
            for (size_t sq = 0; sq < 64; ++sq)
                result.emplace_back(std::string("pst_mg_") + piece_names[pc] + "_" +
                                        std::to_string(sq),
                                    &pst_mg[pc][sq]);
        for (size_t pc = 0; pc < 6; ++pc)
            for (size_t sq = 0; sq < 64; ++sq)
                result.emplace_back(std::string("pst_eg_") + piece_names[pc] + "_" +
                                        std::to_string(sq),
                                    &pst_eg[pc][sq]);
        return result;
    }

    if (stage == TuneStage::search) {
        // Stage 5: search constants. Deliberately NOT part of any eval stage
        // -- a Texel gradient over these is identically zero, because they
        // change which nodes the search visits rather than what a position is
        // worth. They are tuned by SPSA against game results.
        result.emplace_back("rfp_max_depth", &rfp_max_depth);
        result.emplace_back("rfp_margin", &rfp_margin);
        result.emplace_back("nmp_min_depth", &nmp_min_depth);
        result.emplace_back("nmp_base_r", &nmp_base_r);
        result.emplace_back("nmp_depth_div", &nmp_depth_div);
        result.emplace_back("nmp_eval_div", &nmp_eval_div);
        result.emplace_back("nmp_eval_max", &nmp_eval_max);
        result.emplace_back("futility_base", &futility_base);
        result.emplace_back("history_prune_depth", &history_prune_depth);
        result.emplace_back("history_prune_margin", &history_prune_margin);
        result.emplace_back("see_prune_depth", &see_prune_depth);
        result.emplace_back("singular_min_depth", &singular_min_depth);
        result.emplace_back("singular_margin", &singular_margin);
        result.emplace_back("lmr_min_depth", &lmr_min_depth);
        result.emplace_back("lmr_hist_bad", &lmr_hist_bad);
        result.emplace_back("lmr_hist_good", &lmr_hist_good);
        result.emplace_back("best_move_bonus", &best_move_bonus);
        result.emplace_back("history_bonus_scale", &history_bonus_scale);
        result.emplace_back("history_malus_pct", &history_malus_pct);
        result.emplace_back("lazy_margin", &lazy_margin);
        return result;
    }

    // Stage 3 (fine): material values + everything from stage 2
    // Material values
    for (size_t i = 0; i < material_value.size(); ++i)
        result.emplace_back("material_value_" + std::to_string(i), &material_value[i]);

    // Include all stage 2 params as well
    auto stage2 = all_params(TuneStage::shape);
    result.insert(result.end(), stage2.begin(), stage2.end());

    return result;
}

std::vector<std::pair<std::string, int*>> parameters::every_param() {
    // Single source of truth for "every tunable". save() and load() must agree
    // on this set; when they did not, whole groups were written to disk and
    // then silently ignored on the way back in.
    auto result = all_params(TuneStage::category);
    for (auto stage : {TuneStage::fine, TuneStage::pst, TuneStage::search}) {
        auto s = all_params(stage);
        result.insert(result.end(), s.begin(), s.end());
    }
    return result;
}

} // namespace havoc
