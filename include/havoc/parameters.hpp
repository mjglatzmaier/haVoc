#pragma once

/// @file parameters.hpp
/// @brief Evaluation parameters / tuning constants for the HCE evaluator.

#include "havoc/squares.hpp"

#include <array>
#include <string>
#include <utility>
#include <vector>

namespace havoc {

/// Tuning stages for hierarchical parameter optimization.
enum class TuneStage {
    category, // Stage 1: category-level scale factors
    shape,    // Stage 2: curve shapes and individual weights
    fine,     // Stage 3: material values + stage 2
    pst,      // Stage 4: the 768 piece-square table entries
    search    // Stage 5: search pruning and reduction constants
};

struct parameters {
    // ── Category-level scale factors (percentage: 100 = 1.0x) ───────────────
    // All at 100 = original eval scale. Tune individual params directly.
    int sq_score_category_scale = 100;
    int mobility_category_scale = 100;
    /// Endgame counterpart to mobility_category_scale. Mobility had no phase
    /// dependence at all, so a mobile knight was worth the same with a full
    /// board as with four pieces left. Unlike the passed-pawn taper the right
    /// direction here is not obvious, so this defaults equal to the middlegame
    /// endpoint: the change is inert until the tuner fits it.
    int mobility_endgame_scale = 100;
    int king_safety_category_scale = 100;
    int threat_category_scale = 100;
    int passed_pawn_category_scale = 100;
    /// Endgame counterpart to passed_pawn_category_scale. The passed-pawn
    /// evaluation used to be a single phase-independent scalar, so a passer
    /// was worth exactly as much in the opening as in a king-and-pawn ending.
    /// The score is now tapered between these two endpoints by game phase.
    int passed_pawn_endgame_scale = 150;
    int pawn_structure_category_scale = 100;
    int space_category_scale = 100;
    int king_danger_divisor = 256;
    /// Bonus, in centipawns, for having the move. Measured, not assumed: this
    /// term was effectively inert until the side-to-move convention was
    /// unified, because the old `stm_sign * (score + tempo)` is just
    /// `score + tempo` in white-relative terms at every leaf -- a constant
    /// that cancels out of every comparison the search makes. Switching it on
    /// at 5cp measured -40.1 +/- 41.9 Elo over 202 self-play games (LOS 3%):
    /// the side-to-move oscillation it injects costs more search stability
    /// than the term is worth here. Left at 0 pending a proper sweep.
    int tempo = 0;

    // Live piece-square tables [piece][square], seeded from the defaults in
    // squares.hpp. These are the single largest group of evaluation weights
    // (768 numbers) and were previously constexpr, which put them entirely
    // out of the tuner's reach.
    std::array<std::array<int, 64>, 6> pst_mg = kPieceSquareMiddlegame;
    std::array<std::array<int, 64>, 6> pst_eg = endgame_tables_as_seeded();

    // Piece-square score scaling, indexed by the Piece enum, which runs
    // pawn..king -- six entries, not five. These were sized 5 while
    // hce.cpp indexed them with king (== 5), reading one past the end.
    // std::array keeps the size tied to the enum and makes the mistake
    // impossible to repeat silently.
    std::array<int, 6> sq_score_scaling{1, 1, 1, 1, 1, 1};

    // Mobility scaling (indexed by Piece enum)
    std::array<int, 6> mobility_scaling{1, 1, 1, 1, 1, 1};

    // Per-piece mobility curve scale factors (percentage: 100 = 1.0x)
    int knight_mobility_scale = 100;
    int bishop_mobility_scale = 98;
    int rook_mobility_scale = 98;

    // Mobility tables (moved from hce.cpp anonymous namespace)
    std::array<int, 9> knight_mobility_table = {-50, -30, -15, -6, 2, 8, 13, 17, 20};
    std::array<int, 15> bishop_mobility_table = {-50, -30, -15, -6, 2,  8,  13, 17,
                                                 20,  22,  24,  25, 26, 27, 28};
    std::array<int, 15> rook_mobility_table = {0, 1, 2, 3, 5, 6, 8, 9, 10, 11, 13, 14, 15, 17, 18};

    // Square attack bonuses (pawn, knight, bishop, rook, queen)
    std::vector<int> square_attks{7, 4, 3, 2, 1};

    // Piece attack scaling
    std::vector<int> attack_scaling{1, 1, 1, 1, 1};

    // King attack tables per piece type
    static constexpr int knight_attks[6] = {1, 3, 4, 9, 16, 25};
    static constexpr int bishop_attks[6] = {1, 3, 4, 9, 16, 25};
    static constexpr int rook_attks[6] = {0, 2, 5, 5, 7, 15};
    static constexpr int queen_attks[6] = {0, 1, 2, 3, 4, 9};

    std::vector<int> trapped_rook_penalty{1, 2}; // mg, eg

    std::vector<int> attk_queen_bonus{2, 1, 1, 1, 0};

    // Pinned piece scaling
    std::vector<int> pinned_scaling{1, 1, 2, 3, 4};

    // Minor piece bonuses
    std::vector<int> knight_outpost_bonus{0, 1, 2, 3, 3, 2, 1, 0};
    std::vector<int> bishop_outpost_bonus{0, 0, 1, 2, 2, 1, 0, 0};
    std::vector<int> center_influence_bonus{0, 1, 1, 1, 1, 0};

    // King harassment tables
    static constexpr int pawn_king[3] = {1, 2, 3};
    static constexpr int knight_king[3] = {1, 2, 3};
    static constexpr int bishop_king[3] = {1, 2, 3};
    static constexpr int rook_king[5] = {1, 2, 3, 3, 4};
    static constexpr int queen_king[7] = {1, 3, 3, 4, 4, 5, 6};

    static constexpr int attack_combos[5][5] = {
        {0, 0, 0, 4, 10},     // pawn
        {0, 4, 4, 4, 15},     // knight
        {0, 4, 4, 4, 12},     // bishop
        {0, 4, 4, 10, 15},    // rook
        {10, 15, 12, 15, 20}, // queen
    };

    std::vector<int> attacker_weight{1, 4, 8, 16, 32};
    // Penalty per own pawn standing on the same square colour as the bishop,
    // one value per phase endpoint. The bishop cannot attack any of them and
    // they get in its way, and it matters more once the position simplifies,
    // hence the larger endgame figure. These were hard-coded 1 and 3 in
    // eval_bishops and are exposed here so the tuner can reach them and so
    // EveryTunableParameterReachesTheEvaluation keeps them connected.
    int bishop_own_pawn_penalty_mg = 1;
    int bishop_own_pawn_penalty_eg = 3;

    std::vector<int> king_shelter{-3, -2, 2, 3}; // 0,1,2,3 pawns
    std::vector<int> king_safe_sqs{-4, -2, -1, 0, 0, 1, 2, 4};

    /// Danger per square from which an enemy piece could give check without
    /// being recaptured, indexed by the checking piece. Index 0 (pawn) is
    /// unused: a pawn check is never the threat that matters here.
    std::vector<int> safe_check_weight{0, 6, 5, 8, 12};

    int uncastled_penalty = 5;
    static constexpr int connected_rook_bonus = 1;
    static constexpr int doubled_bishop_bonus = 4;
    static constexpr int open_file_bonus = 1;
    static constexpr int bishop_open_center_bonus = 1;
    static constexpr int bishop_color_complex_penalty = 1;
    static constexpr int bishop_penalty_pawns_same_color = 1;
    static constexpr int rook_7th_bonus = 2;

    // Pawn structure
    /// Pawn-structure penalties, split by game phase.
    ///
    /// These used to be static constexpr, so they were invisible to the
    /// tuner, and pawn_score applied every penalty identically to the
    /// middlegame and endgame accumulators. A pawn weakness is worth far more
    /// in an endgame, where it cannot be covered by pieces and becomes a
    /// fixed target, so the endgame endpoints default higher.
    // Endgame endpoints deliberately equal the middlegame ones, which makes
    // the split exactly inert: sub(v, v) is the old operator-=(v) bit for bit.
    // Raising them (doubled 4->12, isolated 4->12, backward 1->3) was measured
    // at -85 +/- 35 Elo over 274 games, LLR -2.96, H0 accepted. The parameters
    // exist so Texel can fit the endgame endpoints from data; guessing them by
    // hand is what failed.
    int doubled_pawn_penalty_mg = 4;
    int doubled_pawn_penalty_eg = 4;
    int backward_pawn_penalty_mg = 1;
    int backward_pawn_penalty_eg = 1;
    int isolated_pawn_penalty_mg = 4;
    int isolated_pawn_penalty_eg = 4;
    static constexpr int semi_open_pawn_penalty = 1;

    // Material values
    std::array<int, 6> material_value = {100, 300, 315, 480, 910, 20000};

    // Endgame scaling (out of 128; 128 = no scaling)
    int opposite_bishop_scale = 24;
    int no_pawn_scale = 32;
    int minor_advantage_no_pawn_scale = 8;
    int wrong_rook_pawn_scale = 0;

    // Passed pawn rank bonuses
    /// Passed-pawn bonus by distance to promotion, indexed [4 - row_dist],
    /// so entry 0 is a passer still four or more ranks away and entry 3 is one
    /// step from queening. These values were hard-coded inside
    /// eval_passed_pawns while this array sat registered with the tuner and
    /// read by nothing.
    std::array<int, 4> passed_pawn_rank_bonus = {2, 45, 90, 180};

    /// The rest of eval_passed_pawns, which carried these as bare literals.
    /// A passed pawn is the single most decisive structural feature on the
    /// board, and every term below the rank ladder was fixed at a number
    /// nobody could fit. Defaults reproduce the previous literals exactly.
    int passed_pawn_unblocked = 1;      ///< square in front is empty
    int passed_pawn_control = 3;        ///< per attacker of the square in front, each side
    int passed_pawn_rook_behind = 1;    ///< own rook anywhere behind on the file
    int passed_pawn_rook_support = 30;  ///< own rook behind with a clear path
    int passed_pawn_connected = 30;     ///< another passer on a neighbouring file
    /// Penalty when the square in front is controlled by the opponent, by
    /// distance to promotion, indexed [3 - row_dist]: entry 0 is three ranks
    /// out and entry 2 is one step from queening.
    std::array<int, 3> passed_pawn_blocked_penalty = {30, 55, 120};

    // Search
    int fixed_depth = -1;

    // ── Search pruning and reduction constants ──────────────────────────────
    // These do not appear in the static evaluation, so they have no Texel
    // gradient: changing one changes *which nodes get visited*, not what any
    // position is worth. They can only be tuned against game results, which
    // is what SPSA is for. They live here so the tuner and the ParamFile
    // machinery can reach them; search.cpp reads them through params_.
    int rfp_max_depth = 6;          // reverse futility only below this depth
    int rfp_margin = 80;            // ...and only if eval - margin*depth >= beta
    int nmp_min_depth = 3;          // null move needs at least this much depth
    int nmp_base_r = 3;             // base null-move reduction
    int nmp_depth_div = 6;          // extra reduction of depth/this
    int nmp_eval_div = 200;         // extra reduction of (eval-beta)/this
    int nmp_eval_max = 3;           // ...capped here
    int futility_base = 6;          // (base + depth^2) / (2 - improving)
    int history_prune_depth = 3;    // history pruning only below this depth
    int history_prune_margin = 4096; // ...and only below -margin*depth
    int see_prune_depth = 1;        // negative-SEE capture pruning depth
    int qs_delta_margin = 910;      // quiescence delta pruning margin
    int qs_delta_pawn7th = 775;     // ...widened by this with a pawn near promotion
    int singular_min_depth = 8;     // singular extension minimum depth
    int singular_margin = 2;        // singular beta = ttvalue - margin*depth
    int lmr_min_depth = 3;          // late move reductions minimum depth
    int lmr_hist_bad = 2000;        // reduce one extra ply below -this
    int lmr_hist_good = 4000;       // reduce one less ply above this
    int best_move_bonus = 2;        // best-move history bonus, times depth
    int history_bonus_scale = 1;    // history bonus is scale * depth^2
    int history_malus_pct = 0;      // fail-low penalty, percent of the bonus (0 = off)
    int lazy_margin = 225;          // lazy evaluation cutoff margin

    static constexpr int pawn_lever_score[64] = {
        1, 2, 3, 4, 4, 3, 2, 1, 1, 2, 3, 4, 4, 3, 2, 1, 1, 2, 3, 4, 4, 3,
        2, 1, 1, 2, 3, 4, 4, 3, 2, 1, 1, 2, 3, 4, 4, 3, 2, 1, 1, 2, 3, 4,
        4, 3, 2, 1, 1, 2, 3, 4, 4, 3, 2, 1, 1, 2, 3, 4, 4, 3, 2, 1,
    };

    // Parameter serialization
    bool load(const std::string& filename);
    bool save(const std::string& filename) const;

    /// Returns all tunable params as name/pointer pairs for the tuner.
    std::vector<std::pair<std::string, int*>> all_params(TuneStage stage = TuneStage::shape);

    /// Every tunable parameter across every stage. save() and load() both go
    /// through this so they cannot drift apart.
    std::vector<std::pair<std::string, int*>> every_param();
};

/// Tapered piece-square score interpolated between middlegame and endgame.
/// game_phase: 0 = pure middlegame, 24 = pure endgame. This matches
/// material_entry::phase_interpolant, which counts *down* from 24 as material
/// is removed; the previous comments here and on that field had it backwards.
template <Color c>
[[nodiscard]] inline int square_score(const parameters& p, Piece pc, Square s, int game_phase) {
    const int idx = (c == white) ? static_cast<int>(s) : mirror_square(s);
    const int mid = p.pst_mg[pc][idx];
    const int eg = p.pst_eg[pc][idx];
    return (eg * game_phase + mid * (24 - game_phase)) / 24;
}

} // namespace havoc
