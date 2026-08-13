#include "havoc/parameters.hpp"

#include "havoc/position.hpp"

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

    sync_see_values();
    return applied > 0;
}

// Static exchange evaluation orders and prunes captures, so it is asserting
// things about material that the evaluation also asserts. Keep the two saying
// the same thing.
void parameters::sync_see_values() const {
    position::set_see_values({material_value[0], material_value[1], material_value[2],
                              material_value[3], material_value[4]});
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
        result.emplace_back("mobility_endgame_scale", &mobility_endgame_scale);
        result.emplace_back("king_safety_category_scale", &king_safety_category_scale);
        result.emplace_back("threat_category_scale", &threat_category_scale);
        result.emplace_back("passed_pawn_category_scale", &passed_pawn_category_scale);
        result.emplace_back("passed_pawn_endgame_scale", &passed_pawn_endgame_scale);
        result.emplace_back("pawn_structure_category_scale", &pawn_structure_category_scale);
        result.emplace_back("doubled_pawn_penalty_mg", &doubled_pawn_penalty_mg);
        result.emplace_back("doubled_pawn_penalty_eg", &doubled_pawn_penalty_eg);
        result.emplace_back("backward_pawn_penalty_mg", &backward_pawn_penalty_mg);
        result.emplace_back("backward_pawn_penalty_eg", &backward_pawn_penalty_eg);
        result.emplace_back("isolated_pawn_penalty_mg", &isolated_pawn_penalty_mg);
        result.emplace_back("isolated_pawn_penalty_eg", &isolated_pawn_penalty_eg);
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
        result.emplace_back("wrong_rook_pawn_scale", &wrong_rook_pawn_scale);

        // Passed pawn rank bonuses
        for (size_t i = 0; i < passed_pawn_rank_bonus.size(); ++i)
            result.emplace_back("passed_pawn_rank_bonus_" + std::to_string(i),
                                &passed_pawn_rank_bonus[i]);

        result.emplace_back("passed_pawn_unblocked", &passed_pawn_unblocked);
        result.emplace_back("passed_pawn_control", &passed_pawn_control);
        result.emplace_back("passed_pawn_rook_behind", &passed_pawn_rook_behind);
        result.emplace_back("passed_pawn_rook_support", &passed_pawn_rook_support);
        result.emplace_back("passed_pawn_connected", &passed_pawn_connected);
        for (size_t i = 0; i < passed_pawn_blocked_penalty.size(); ++i)
            result.emplace_back("passed_pawn_blocked_penalty_" + std::to_string(i),
                                &passed_pawn_blocked_penalty[i]);

        // Attacker weights
        for (size_t i = 0; i < attacker_weight.size(); ++i)
            result.emplace_back("attacker_weight_" + std::to_string(i), &attacker_weight[i]);

        // King shelter
        for (size_t i = 0; i < king_shelter.size(); ++i)
            result.emplace_back("king_shelter_" + std::to_string(i), &king_shelter[i]);

        // King safe squares
        for (size_t i = 0; i < king_safe_sqs.size(); ++i)
            result.emplace_back("king_safe_sqs_" + std::to_string(i), &king_safe_sqs[i]);
        for (size_t i = 1; i < safe_check_weight.size(); ++i)
            result.emplace_back("safe_check_weight_" + std::to_string(i), &safe_check_weight[i]);

        result.emplace_back("bishop_own_pawn_penalty_mg", &bishop_own_pawn_penalty_mg);
        result.emplace_back("bishop_own_pawn_penalty_eg", &bishop_own_pawn_penalty_eg);

        result.emplace_back("uncastled_penalty", &uncastled_penalty);

        // King harassment tables, the threat tables and the piece-pair combo
        // matrix. All of these were static constexpr until now, which put the
        // bulk of the king attack weighting out of the tuner's reach.
        //
        // Every one of these loops starts at 1, not 0. eval_king reads these
        // tables as table[min(N, king_attk_count)] inside `if
        // (king_attk_count)`, so entry 0 is the "attacks no square of the king
        // ring" case and is unreachable by construction. Registering it would
        // hand the tuner a weight with an identically zero gradient.
        for (size_t i = 1; i < pawn_king.size(); ++i)
            result.emplace_back("pawn_king_" + std::to_string(i), &pawn_king[i]);
        for (size_t i = 1; i < knight_king.size(); ++i)
            result.emplace_back("knight_king_" + std::to_string(i), &knight_king[i]);
        for (size_t i = 1; i < bishop_king.size(); ++i)
            result.emplace_back("bishop_king_" + std::to_string(i), &bishop_king[i]);
        for (size_t i = 1; i < rook_king.size(); ++i)
            result.emplace_back("rook_king_" + std::to_string(i), &rook_king[i]);
        // queen_king stops one short of the end. The table is indexed per
        // queen by min(6, count), so entry 6 needs a single queen attacking
        // six squares of the eight-square king ring, and the king blocks
        // every ray that would pass through its own square. Exhausting all
        // 64x63 king/queen placements gives a maximum of five, so entry 6
        // cannot be read by any position, legal or otherwise.
        for (size_t i = 1; i + 1 < queen_king.size(); ++i)
            result.emplace_back("queen_king_" + std::to_string(i), &queen_king[i]);

        // Only the strictly lower triangle. eval_king walks p1 from knight to
        // queen and p2 from pawn to p1 - 1, so a combination is always stored
        // with the stronger piece first and the diagonal never appears -- a
        // piece type cannot combine with itself here. That is 10 live entries
        // out of 25; the other 15 are shape, not weights.
        for (size_t i = knight; i <= queen; ++i)
            for (size_t j = 0; j < i; ++j)
                result.emplace_back("attack_combos_" + std::to_string(i) + "_" +
                                        std::to_string(j),
                                    &attack_combos[i][j]);

        // Threat tables are indexed by the victim's piece type. eval_threats
        // builds its victim set as `ei.pieces[them] ^ enemyPawns`, so a pawn
        // is never a victim here and entry 0 cannot be read. Entry 5, the
        // king, is only reachable when the side to move is in check, and a
        // bonus for "attacking the king" is not a weight worth fitting, so it
        // is left live but untuned.
        for (size_t i = knight; i <= queen; ++i) {
            result.emplace_back("knight_attks_" + std::to_string(i), &knight_attks[i]);
            result.emplace_back("bishop_attks_" + std::to_string(i), &bishop_attks[i]);
            result.emplace_back("rook_attks_" + std::to_string(i), &rook_attks[i]);
            result.emplace_back("queen_attks_" + std::to_string(i), &queen_attks[i]);
        }

        result.emplace_back("connected_rook_bonus", &connected_rook_bonus);
        result.emplace_back("doubled_bishop_bonus", &doubled_bishop_bonus);
        result.emplace_back("open_file_bonus", &open_file_bonus);
        result.emplace_back("bishop_open_center_bonus", &bishop_open_center_bonus);
        result.emplace_back("rook_7th_bonus", &rook_7th_bonus);

        // Outpost bonuses are indexed by the file of the outpost square, so
        // all eight entries are reachable: any file can contain a hole in the
        // enemy pawn structure.
        for (size_t i = 0; i < knight_outpost_bonus.size(); ++i)
            result.emplace_back("knight_outpost_" + std::to_string(i), &knight_outpost_bonus[i]);
        for (size_t i = 0; i < bishop_outpost_bonus.size(); ++i)
            result.emplace_back("bishop_outpost_" + std::to_string(i), &bishop_outpost_bonus[i]);

        // Center influence is read once per piece type in eval_knights,
        // eval_bishops, eval_rooks and eval_queens. Pawns and kings never
        // score it, so entries 0 and 5 are shape, not weights.
        for (size_t i = knight; i <= queen; ++i)
            result.emplace_back("center_influence_" + std::to_string(i),
                                &center_influence_bonus[i]);

        // The bonus for attacking the enemy queen is only read by the knight,
        // bishop and rook evaluations -- a queen attacking a queen is a plain
        // exchange, handled by SEE and the threat tables, so entry 4 is never
        // read. Entry 0 (pawn) has no reader either.
        for (size_t i = knight; i <= rook; ++i)
            result.emplace_back("attk_queen_" + std::to_string(i), &attk_queen_bonus[i]);

        // Both halves of the trapped rook penalty are live: it is applied
        // through taper(), which blends the two by game phase.
        result.emplace_back("trapped_rook_mg", &trapped_rook_penalty[0]);
        result.emplace_back("trapped_rook_eg", &trapped_rook_penalty[1]);

        // Deliberately NOT registered, and not oversights:
        //
        //   sq_score_scaling, attack_scaling, mobility_scaling -- all-ones
        //   multipliers sitting in front of terms that are themselves tuned
        //   (the piece-square tables, the threat tables, the per-piece
        //   mobility scales). Fitting a product of two free weights is rank
        //   deficient: the tuner could trade one against the other without
        //   changing the evaluation, which wastes gradient and makes a tuned
        //   file harder to interpret.
        //
        //   pinned_scaling -- a *divisor* on the mobility score. Exposing it
        //   would let the tuner try zero and divide by zero, and integer
        //   division makes its gradient a staircase rather than a slope.
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
    result.emplace_back("qs_delta_margin", &qs_delta_margin);
    result.emplace_back("qs_delta_pawn7th", &qs_delta_pawn7th);
        result.emplace_back("singular_min_depth", &singular_min_depth);
        result.emplace_back("singular_margin", &singular_margin);
        result.emplace_back("lmr_min_depth", &lmr_min_depth);
        result.emplace_back("lmr_hist_bad", &lmr_hist_bad);
        result.emplace_back("lmr_hist_good", &lmr_hist_good);
        result.emplace_back("best_move_bonus", &best_move_bonus);
        result.emplace_back("history_bonus_scale", &history_bonus_scale);
        result.emplace_back("history_malus_pct", &history_malus_pct);
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
