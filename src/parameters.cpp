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
    // save() writes category + fine; load() must accept exactly the same set.
    // Using the default argument here silently restricted matching to the
    // shape stage, so every category scale and every material value in the
    // file was dropped without complaint -- including when a tuned file was
    // loaded back into the engine via the UCI 'load_params' path.
    auto params = all_params(TuneStage::category);
    auto fine = all_params(TuneStage::fine);
    params.insert(params.end(), fine.begin(), fine.end());

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

    // Save all params from all stages
    auto cat = const_cast<parameters*>(this)->all_params(TuneStage::category);
    auto fine = const_cast<parameters*>(this)->all_params(TuneStage::fine);
    // fine already includes shape params, so cat + fine covers everything
    for (const auto& [name, ptr] : cat)
        out << name << " = " << *ptr << "\n";
    for (const auto& [name, ptr] : fine)
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

        result.emplace_back("uncastled_penalty", &uncastled_penalty);
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

} // namespace havoc
