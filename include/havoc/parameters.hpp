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

    // The queen had no mobility term at all: eval_queens computed its attack
    // set and stored it into piece_attacks, but never scored how many squares
    // it actually had. Knight, bishop and rook each had a table and a scale.
    // A queen on an open board reaches up to 27 squares, so the table is
    // sized to the widest fan-out any single piece can have. The curve is
    // deliberately flatter than the rook's: a queen is already worth so much
    // that being slightly cramped should not read as a large loss, and the
    // shape is a starting point for tuning rather than a fitted answer.
    int queen_mobility_scale = 98;
    std::array<int, 28> queen_mobility_table = {-20, -16, -12, -9, -6, -4, -2, 0, 1,  2,
                                                3,   4,   5,   6,  7,  8,  9,  9, 10, 10,
                                                11,  11,  12,  12, 13, 13, 14, 14};

    // Square attack bonuses (pawn, knight, bishop, rook, queen)
    std::vector<int> square_attks{7, 4, 3, 2, 1};

    // Piece attack scaling
    std::vector<int> attack_scaling{1, 1, 1, 1, 1};

    // Threat tables: value of attacking a given victim, indexed by victim.
    // These were static constexpr and so unreachable by the tuner, even though
    // they are multiplied into every threat score the evaluation produces.
    std::array<int, 6> knight_attks = {1, 3, 4, 9, 16, 25};
    std::array<int, 6> bishop_attks = {1, 3, 4, 9, 16, 25};
    std::array<int, 6> rook_attks = {0, 2, 5, 5, 7, 15};
    std::array<int, 6> queen_attks = {0, 1, 2, 3, 4, 9};

    std::vector<int> trapped_rook_penalty{1, 2}; // mg, eg

    std::vector<int> attk_queen_bonus{2, 1, 1, 1, 0};

    // Pinned piece scaling
    std::vector<int> pinned_scaling{1, 1, 2, 3, 4};

    // Minor piece bonuses
    std::vector<int> knight_outpost_bonus{0, 1, 2, 3, 3, 2, 1, 0};
    std::vector<int> bishop_outpost_bonus{0, 0, 1, 2, 2, 1, 0, 0};
    /// Extra for an outpost a friendly pawn defends, indexed by Piece. Only the
    /// knight and bishop entries are read; the rest are zero and unregistered.
    /// A defended outpost cannot be challenged by a pawn or driven off cheaply,
    /// which is the difference between an outpost and a square a piece happens
    /// to be standing on.
    std::vector<int> outpost_defended_bonus{0, 4, 3, 0, 0, 0};
    std::vector<int> center_influence_bonus{0, 1, 1, 1, 1, 0};

    // King harassment tables: bonus for attacking N squares of the enemy king
    // ring, indexed by that count. Together with attack_combos these are the
    // whole of the king attack weighting, and every one of them used to be
    // static constexpr -- so the single largest group of king-safety weights
    // in the evaluation was the one group the tuner could not reach.
    std::array<int, 3> pawn_king = {1, 2, 3};
    std::array<int, 3> knight_king = {1, 2, 3};
    std::array<int, 3> bishop_king = {1, 2, 3};
    std::array<int, 5> rook_king = {1, 2, 3, 3, 4};
    std::array<int, 7> queen_king = {1, 3, 3, 4, 4, 5, 6};

    /// Extra danger when two different piece types attack the king zone
    /// together, indexed by the two piece types. Row/column order is
    /// pawn, knight, bishop, rook, queen.
    std::array<std::array<int, 5>, 5> attack_combos = {{
        {{0, 0, 0, 4, 10}},     // pawn
        {{0, 4, 4, 4, 15}},     // knight
        {{0, 4, 4, 4, 12}},     // bishop
        {{0, 4, 4, 10, 15}},    // rook
        {{10, 15, 12, 15, 20}}, // queen
    }};

    std::vector<int> attacker_weight{1, 4, 8, 16, 32};
    // Penalty per own pawn standing on the same square colour as the bishop,
    // one value per phase endpoint. The bishop cannot attack any of them and
    // they get in its way, and it matters more once the position simplifies,
    // hence the larger endgame figure. These were hard-coded 1 and 3 in
    // eval_bishops and are exposed here so the tuner can reach them and so
    // EveryTunableParameterReachesTheEvaluation keeps them connected.
    int bishop_own_pawn_penalty_mg = 1;
    int bishop_own_pawn_penalty_eg = 3;

    /// Shelter bonus by the number of own pawns in the king's zone, capped at
    /// three. These used to be halved at the point of use, which truncated:
    /// -3 and -2 both became -1, so two distinct parameter values produced an
    /// identical evaluation and the tuner had no gradient between them. The
    /// halving is gone and these are the values it produced.
    std::vector<int> king_shelter{-1, -1, 1, 1}; // 0,1,2,3 pawns
    /// Charged when the king's flank holds no friendly pawn at all.
    int pawnless_flank_penalty = 2;
    /// Charged by the number of enemy pawns bearing down on the king's side of
    /// the board, capped at three.
    std::vector<int> king_storm_penalty{0, 0, 2, 4};
    std::vector<int> king_safe_sqs{-4, -2, -1, 0, 0, 1, 2, 4};

    /// Danger per square from which an enemy piece could give check without
    /// being recaptured, indexed by the checking piece. Index 0 (pawn) is
    /// unused: a pawn check is never the threat that matters here.
    std::vector<int> safe_check_weight{0, 6, 5, 8, 12};
    // Files beside the king with no pawn cover. A file with none of our own
    // pawns is a highway for enemy heavy pieces; one with no pawns at all is
    // worse still.
    int king_semiopen_file_penalty = 5;
    int king_open_file_penalty = 9;
    int king_open_file_heavy_penalty = 4;

    int uncastled_penalty = 5;
    /// eval_threats weights. Every one of these was a bare literal in the
    /// evaluation, so none of them could be tuned. The defaults reproduce the
    /// values that were hardcoded. The three tables are indexed
    /// 0 = knight, 1 = bishop, 2 = rook, 3 = queen rather than by Piece, so
    /// that no entry is structurally unreachable.
    int threat_by_pawn = 1;
    std::vector<int> threat_weak_pawn{1, 1, 1, 1};
    int queen_pin_minor = 6;
    int queen_pin_rook = 18;
    int discovered_check_bonus = 10;
    int restriction_weight = 1;
    std::vector<int> skewer_bonus{4, 6, 8}; // minor, rook, queen skewered
    /// Per-piece evaluation weights that were bare literals. Every one of
    /// these fires in ordinary middlegame positions -- protection counts, king
    /// distance, x-rays -- so unlike the endgame constants they carry real
    /// gradient and were simply invisible to the tuner. The defaults reproduce
    /// the values that were hardcoded.
    ///
    /// `pawn_attacks_undefended` keeps the `/ 2` it always had so the default
    /// of 1 reproduces the old score exactly. The division is the reason it
    /// needs a parameter at all: as a bare count halved by integer division it
    /// had no gradient below one point per two attacks, and the tuner can now
    /// reach the odd multiples the truncation used to swallow.
    int pawn_attacks_undefended = 1;
    int knight_edge_penalty = 12;
    int knight_king_distance_penalty = 1;
    int knight_behind_pawn_bonus = 12;
    int knight_protection_bonus = 1;
    int bishop_xray_bonus = 1;
    int bishop_king_distance_penalty = 1;
    int bishop_behind_pawn_bonus = 12;
    int bishop_protection_bonus = 1;
    int rook_xray_bonus = 1;
    int rook_protection_bonus = 1;
    int weak_queen_penalty = 1;
    int space_bonus = 1;
    int connected_rook_bonus = 1;
    int doubled_bishop_bonus = 4;
    /// Rook on a file with no pawn of its own in front of it. `open` means no
    /// pawns of either colour, `semiopen` means only the enemy's -- the second
    /// case was not scored at all, and the first was worth one centipawn.
    int open_file_bonus = 12;
    int semiopen_file_bonus = 6;
    /// Applied with a positive sign to a knight and a negative one to a bishop
    /// when the centre is locked, so the name describes the bishop's side of
    /// the trade only. A knight gains what the bishop loses.
    int bishop_open_center_bonus = 1;
    int rook_7th_bonus = 2;


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
    /// A pawn no friendly pawn defends. This was a bare `score -= 1` applied
    /// identically to both accumulators, so it could neither be tuned nor
    /// tapered.
    int undefended_pawn_penalty_mg = 1;
    int undefended_pawn_penalty_eg = 1;
    /// The extra charge for a weak pawn standing on a file the enemy has no
    /// pawn on, where it cannot be defended by a pawn and is exposed to the
    /// heavy pieces. Each of these was `2 * ` the corresponding base penalty,
    /// which froze the aggravation at exactly double and left the tuner unable
    /// to fit it at all: the whole term moved only when the base penalty
    /// moved. The defaults reproduce the doubling, so nothing changes until a
    /// fit says otherwise.
    int backward_pawn_semiopen_mg = 2;
    int backward_pawn_semiopen_eg = 2;
    int isolated_pawn_semiopen_mg = 8;
    int isolated_pawn_semiopen_eg = 8;
    int doubled_pawn_semiopen_mg = 8;
    int doubled_pawn_semiopen_eg = 8;
    /// A doubled pawn that is also isolated -- no neighbour on either side and
    /// a friend stacked in front of it. Also frozen at double the plain
    /// doubled penalty.
    int doubled_isolated_penalty_mg = 8;
    int doubled_isolated_penalty_eg = 8;

    // Connected pawns, indexed by the pawn's rank relative to its own side.
    // Until these were added the pawn evaluation was made entirely of
    // penalties -- doubled, isolated, backward, undefended -- with no term
    // that rewarded a healthy structure. That left the tuner able to express
    // only "less bad" and never "good", and it biased the evaluation against
    // having pawns at all.
    //
    // Two separate cases rather than one bonus with a hardcoded multiplier,
    // so that every number here is something Texel can fit:
    //   phalanx   -- a friendly pawn abreast on an adjacent file
    //   supported -- a friendly pawn defending this one from behind
    // A pawn can be both, and then it collects both.
    //
    // Entries 0 and 7 are unreachable: relative rank 0 is a side's own back
    // rank and 7 is the promotion square, and no pawn ever stands on either.
    // Scaled to haVoc's own pawn structure magnitudes, not to the values these
    // tables conventionally carry elsewhere. haVoc charges 4 for an isolated
    // pawn, 4 for a doubled one and 1 for a backward one, so a phalanx bonus
    // peaking at 40 would have been an order of magnitude larger than every
    // other pawn term and would simply have overridden them. Measured: at the
    // conventional scale the term was worth -12.6 +/- 22.9 Elo over 581 games.
    // The shape is unchanged; only the scale is haVoc's.
    std::array<int, 8> phalanx_pawn_mg = {0, 0, 1, 1, 2, 4, 6, 0};
    std::array<int, 8> phalanx_pawn_eg = {0, 1, 1, 2, 3, 6, 10, 0};
    std::array<int, 8> supported_pawn_mg = {0, 0, 1, 1, 2, 3, 4, 0};
    std::array<int, 8> supported_pawn_eg = {0, 1, 1, 1, 2, 4, 6, 0};

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
    int probcut_min_depth = 5;      // ProbCut needs at least this much depth
    int probcut_margin = 180;       // probcut beta = beta + this
    int probcut_depth_reduction = 4; // verification search depth = depth - this
    int lmr_min_depth = 3;          // late move reductions minimum depth
    int lmr_hist_bad = 2000;        // reduce one extra ply below -this
    int lmr_hist_good = 4000;       // reduce one less ply above this
    int best_move_bonus = 2;        // best-move history bonus, times depth
    int history_bonus_scale = 1;    // history bonus is scale * depth^2
    int history_malus_pct = 0;      // fail-low penalty, percent of the bonus (0 = off)

    /// Continuation history weights, as a percentage of the raw table value.
    /// Plane 1 is keyed on the opponent's reply we are answering, plane 2 on
    /// our own move two plies back. They are separate because the two planes
    /// carry different amounts of signal and only a fit can say how much.
    int cont_hist1_pct = 100;
    int cont_hist2_pct = 100;

    // Parameter serialization
    bool load(const std::string& filename);

    /// Push material_value into static exchange evaluation. load() calls this;
    /// anything that writes the parameters directly -- the Texel tuner does --
    /// has to call it too, or SEE keeps ordering captures by the old values.
    void sync_see_values() const;
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
