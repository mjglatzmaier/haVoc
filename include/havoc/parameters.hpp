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
    int king_danger_divisor = 64;

    /// Percentage of the king danger terms -- safe checks, the quadratic
    /// attacker score and the attack combinations -- that survives at the
    /// pure-endgame end of the phase taper. None of them were tapered at all,
    /// so a rook endgame was charged the same for an exposed king as a
    /// middlegame with two queens on, and that pulls directly against the
    /// endgame king table, which wants the king out in the open. The
    /// middlegame end stays at 100. See docs/revisit-after-tuning.md: the
    /// value here is a chess argument, not a fitted number.
    int king_danger_endgame_scale = 40;
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
    //
    // Indexed by the number of squares the piece attacks that are not occupied
    // by one of its own men and are not covered by an enemy pawn. Capturable
    // enemy pieces count: a knight bearing down on a queen has that square as
    // a real option, and calling it immobile because something stands there is
    // backwards. The table sizes are the evidence that this was the intent all
    // along -- 9, 15, 15 and 28 entries are the full fan-out of each piece
    // (8, 13, 14 and 27 squares plus zero), and the earlier code, which counted
    // only empty squares, could reach the top third of them just about never.
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
    std::vector<int> knight_outpost_bonus{0, 4, 10, 16, 16, 10, 4, 0};
    std::vector<int> bishop_outpost_bonus{0, 0, 5, 9, 9, 5, 0, 0};
    /// Extra for an outpost a friendly pawn defends, indexed by Piece. Only the
    /// knight and bishop entries are read; the rest are zero and unregistered.
    /// A defended outpost cannot be challenged by a pawn or driven off cheaply,
    /// which is the difference between an outpost and a square a piece happens
    /// to be standing on.
    std::vector<int> outpost_defended_bonus{0, 10, 7, 0, 0, 0};
    std::vector<int> center_influence_bonus{0, 0, 0, 0, 0, 0};

    // King harassment tables: bonus for attacking N squares of the enemy king
    // ring, indexed by that count. Together with attack_combos these are the
    // whole of the king attack weighting, and every one of them used to be
    // static constexpr -- so the single largest group of king-safety weights
    // in the evaluation was the one group the tuner could not reach.
    std::array<int, 3> pawn_king = {0, 0, 0};
    std::array<int, 3> knight_king = {0, 0, 0};
    std::array<int, 3> bishop_king = {0, 0, 0};
    std::array<int, 5> rook_king = {0, 0, 0, 0, 0};
    std::array<int, 7> queen_king = {0, 0, 0, 0, 0, 0, 0};

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
    int bishop_own_pawn_penalty_mg = 3;
    int bishop_own_pawn_penalty_eg = 6;

    /// Shelter bonus by the number of own pawns in the king's zone, capped at
    /// three. These used to be halved at the point of use, which truncated:
    /// -3 and -2 both became -1, so two distinct parameter values produced an
    /// identical evaluation and the tuner had no gradient between them. The
    /// halving is gone and these are the values it produced.
    // Indexed by the distance of the nearest friendly pawn in front of the
    // king on a given file, summed over the king file and its two neighbours.
    // Index 0 means no pawn in front at all, 1 means the pawn is unmoved
    // beside the king, 3 means three ranks away or further.
    //
    // The previous model indexed by a *count* of pawns touching the king,
    // which could express four states in total and could not tell h3 from h4.
    // Summing over three files multiplies the range by three, which is easy to
    // miss: these values give +21 for a perfect shield and -45 for a bare king,
    // against the old count model's +6 and -22. Halving them to {-8,4,1,-2}
    // restores the old range and was tried -- it puts the pawn shield pair back
    // to a 4cp margin that king_shelter no longer carries, and bench back to
    // 790209 from 697583. So the discrimination and the node win both come from
    // the range, not from the shape alone, and the two cannot be separated by
    // halving. Left at the wider range, which is the variant that was actually
    // measured: -11.9 +/- 31.8 over 334 games. Needs SPSA, not more hand values.
    std::vector<int> king_shelter{-15, 7, 1, -4}; // distance 0(none),1,2,3+
    /// Charged when the king's flank holds no friendly pawn at all.
    int pawnless_flank_penalty = 12;
    /// Charged by the number of enemy pawns bearing down on the king's side of
    /// the board, capped at three.
    std::vector<int> king_storm_penalty{0, 0, 4, 8};

    /// Per-pawn storm penalty, indexed by how many ranks in front of the king
    /// the storming pawn stands, minus one -- so entry 0 is a pawn on the very
    /// next rank and entry 5 is one still on its starting square. The count
    /// table above says how many pawns are coming; this says how close they
    /// have got.
    /// Kept in family with the term it replaces, which is what matters here:
    /// the entries are summed over up to three pawns, so what has to stay in
    /// scale is the total, not the entry. The count table above tops out at 4
    /// centipawns for a full storm. A first attempt peaked at 32 per pawn --
    /// 96 for three, a twenty-fourfold change -- and measured -22 Elo; a second
    /// at 12 per pawn, 36 for three, measured -37 over 228 games. At 6 a full
    /// storm is worth 18 plus the count term, which is still four times the old
    /// value but is an adjustment to it rather than a replacement of it.
    std::vector<int> king_storm_rank_penalty{6, 4, 2, 1, 0, 0};
    std::vector<int> king_safe_sqs{-12, -6, -3, 0, 0, 2, 4, 6};

    /// Danger per square from which an enemy piece could give check without
    /// being recaptured, indexed by the checking piece. Index 0 (pawn) is
    /// unused: a pawn check is never the threat that matters here.
    std::vector<int> safe_check_weight{0, 15, 12, 20, 25};
    // Files beside the king with no pawn cover. A file with none of our own
    // pawns is a highway for enemy heavy pieces; one with no pawns at all is
    // worse still.
    int king_semiopen_file_penalty = 12;
    int king_open_file_penalty = 20;
    int king_open_file_heavy_penalty = 10;

    /// eval_threats weights. Every one of these was a bare literal in the
    /// evaluation, so none of them could be tuned. The defaults reproduce the
    /// values that were hardcoded. The three tables are indexed
    /// 0 = knight, 1 = bishop, 2 = rook, 3 = queen rather than by Piece, so
    /// that no entry is structurally unreachable.
    int threat_by_pawn = 1;
    std::vector<int> threat_weak_pawn{1, 1, 1, 1};
    int queen_pin_minor = 15;
    int queen_pin_rook = 25;
    int discovered_check_bonus = 10;
    int restriction_weight = 0;
    std::vector<int> skewer_bonus{15, 25, 40}; // minor, rook, queen skewered
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
    int knight_king_distance_penalty = 0;
    int knight_behind_pawn_bonus = 12;
    int knight_protection_bonus = 0;
    int bishop_xray_bonus = 4;
    int bishop_king_distance_penalty = 0;
    int bishop_behind_pawn_bonus = 12;
    int bishop_protection_bonus = 0;
    int rook_xray_bonus = 5;
    int rook_protection_bonus = 0;
    int weak_queen_penalty = 15;
    int space_bonus = 2;
    int connected_rook_bonus = 8;
    int doubled_bishop_bonus = 30;
    /// Rook on a file with no pawn of its own in front of it. `open` means no
    /// pawns of either colour, `semiopen` means only the enemy's -- the second
    /// case was not scored at all, and the first was worth one centipawn.
    int open_file_bonus = 18;
    int semiopen_file_bonus = 8;
    /// Applied with a positive sign to a knight and a negative one to a bishop
    /// when the centre is locked, so the name describes the bishop's side of
    /// the trade only. A knight gains what the bishop loses.
    int bishop_open_center_bonus = 8;
    int rook_7th_bonus = 8;


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
    int doubled_pawn_penalty_mg = 10;
    int doubled_pawn_penalty_eg = 20;
    int backward_pawn_penalty_mg = 9;
    int backward_pawn_penalty_eg = 12;
    int isolated_pawn_penalty_mg = 12;
    int isolated_pawn_penalty_eg = 18;
    /// A pawn no friendly pawn defends. This was a bare `score -= 1` applied
    /// identically to both accumulators, so it could neither be tuned nor
    /// tapered.
    int undefended_pawn_penalty_mg = 3;
    int undefended_pawn_penalty_eg = 6;
    /// The extra charge for a weak pawn standing on a file the enemy has no
    /// pawn on, where it cannot be defended by a pawn and is exposed to the
    /// heavy pieces. Each of these was `2 * ` the corresponding base penalty,
    /// which froze the aggravation at exactly double and left the tuner unable
    /// to fit it at all: the whole term moved only when the base penalty
    /// moved. The defaults reproduce the doubling, so nothing changes until a
    /// fit says otherwise.
    int backward_pawn_semiopen_mg = 6;
    int backward_pawn_semiopen_eg = 8;
    int isolated_pawn_semiopen_mg = 6;
    int isolated_pawn_semiopen_eg = 10;
    int doubled_pawn_semiopen_mg = 5;
    int doubled_pawn_semiopen_eg = 8;
    /// A doubled pawn that is also isolated -- no neighbour on either side and
    /// a friend stacked in front of it. Also frozen at double the plain
    /// doubled penalty.
    int doubled_isolated_penalty_mg = 16;
    int doubled_isolated_penalty_eg = 28;

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
    std::array<int, 8> phalanx_pawn_mg = {0, 0, 3, 4, 8, 16, 28, 0};
    std::array<int, 8> phalanx_pawn_eg = {0, 2, 4, 6, 12, 24, 44, 0};
    std::array<int, 8> supported_pawn_mg = {0, 0, 4, 5, 7, 12, 18, 0};
    std::array<int, 8> supported_pawn_eg = {0, 3, 5, 6, 9, 16, 24, 0};

    // Material values
    std::array<int, 6> material_value = {100, 300, 315, 480, 910, 20000};

    // Endgame scaling (out of 128; 128 = no scaling)
    int opposite_bishop_scale = 24;
    int no_pawn_scale = 32;
    int minor_advantage_no_pawn_scale = 8;
    int wrong_rook_pawn_scale = 0;

    // Passed pawn rank bonuses
    /// Passed-pawn bonus by distance to promotion, indexed [6 - row_dist], so
    /// entry 0 is a passer on its starting rank and entry 5 is one step from
    /// queening.
    ///
    /// This used to be four entries covering only the last three ranks, with
    /// everything further out collapsed into entry 0 at 2 centipawns -- and
    /// eval_passed_pawns took an early exit at that point, so a passer on
    /// rank 4 was not merely worth two centipawns, it was also not credited
    /// with a rook behind it, a connected neighbour, or control of the square
    /// in front. The bottom three entries continue the existing ladder's
    /// roughly doubling shape downward rather than inventing a new scale.
    std::array<int, 6> passed_pawn_rank_bonus = {5, 11, 22, 45, 90, 180};

    /// The rest of eval_passed_pawns, which carried these as bare literals.
    /// A passed pawn is the single most decisive structural feature on the
    /// board, and every term below the rank ladder was fixed at a number
    /// nobody could fit. Defaults reproduce the previous literals exactly.
    /// The five terms below are flat: they say the same thing about a passer on
    /// rank 2 as about one on rank 7. That was harmless while eval_passed_pawns
    /// only looked at the last three ranks, and became wrong the moment it
    /// looked at all of them -- a rank 2 passer with a rook behind it collected
    /// passed_pawn_rook_support, which is 30, against a rank bonus of 5.
    ///
    /// Rather than give each of them its own ladder, they are scaled together
    /// by distance to promotion, indexed [6 - row_dist] like the two tables
    /// above. The last three entries are 100 on purpose: those are the ranks
    /// the old code evaluated, and this change must leave them untouched and
    /// only add to the ranks that were previously scored at 2 centipawns and
    /// skipped.
    std::array<int, 6> passed_pawn_support_scale = {10, 25, 50, 100, 100, 100};
    int passed_pawn_unblocked = 1;      ///< square in front is empty
    int passed_pawn_control = 3;        ///< per attacker of the square in front, each side
    int passed_pawn_rook_behind = 1;    ///< own rook anywhere behind on the file
    int passed_pawn_rook_support = 30;  ///< own rook behind with a clear path
    int passed_pawn_connected = 30;     ///< another passer on a neighbouring file
    /// Penalty when the square in front is controlled by the opponent, by
    /// distance to promotion, indexed [6 - row_dist] to match the rank ladder
    /// above: entry 0 is a passer on its starting rank and entry 5 is one step
    /// from queening.
    ///
    /// This had three entries covering the last three ranks, because that was
    /// all eval_passed_pawns looked at. Widening the ladder without widening
    /// this would charge a passer six ranks from promotion the same 30 that a
    /// passer three ranks out pays, against a rank bonus of 5 -- a pawn worth
    /// five centipawns losing thirty for a square it was never going to reach
    /// this move. The three new entries stay in proportion to the bonuses they
    /// sit beside.
    std::array<int, 6> passed_pawn_blocked_penalty = {3, 7, 15, 30, 55, 120};

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
    int fp_max_depth = 6;           // forward futility pruning of quiets below this depth
    int fp_base = 100;              // ...prune when eval + base + margin*depth <= alpha
    int fp_margin = 90;
    int history_prune_depth = 3;    // history pruning only below this depth
    // DEAD RULE -- DO NOT RETUNE. See docs/revisit-after-tuning.md, "The quiet
    // history table cannot support the rules that read it". This margin sits
    // far outside the range the butterfly table reaches, so the rule never
    // fires: instrumenting a full bench gives 61534 evaluations of the site and
    // 0 firings, with an observed minimum of -193 against a depth-1 threshold
    // of -4096. Retargeting it onto the observed percentiles has been measured
    // twice (-21.6 +/- 21.6 and -29.7 +/- 28.3) and developing the negative
    // half of the table to meet it once (-67.4 +/- 29.3). Both directions are
    // measured, so SPSA cannot rescue it either. The prerequisite is a
    // better-conditioned table -- the key carries no piece identity -- not a
    // different number here.
    int history_prune_margin = 4096; // ...and only below -margin*depth
    int see_prune_depth = 1;        // negative-SEE capture pruning depth
    int see_quiet_prune_depth = 5; // quiet moves losing material pruned below this depth
    int see_quiet_margin = 50;      // ...when see < -margin * depth
    int qs_delta_margin = 910;      // quiescence delta pruning margin
    int qs_delta_pawn7th = 775;     // ...widened by this with a pawn near promotion

    /// Margin on the qsearch capture futility test, on both the fail-low and
    /// the fail-high side. Lived as a bare `int margin = 200;` in the middle of
    /// qsearch, so no tuning run could ever reach it.
    int qs_capture_margin = 200;

    /// Half-width of the first aspiration window at each iteration, before any
    /// widening. Both this and `aspiration_reseed_delta` were function-local
    /// constants and so were invisible to the tuner.
    int aspiration_delta = 65;
    /// Half-width used to re-seed the window from the last *completed* score.
    int aspiration_reseed_delta = 33;
    int singular_min_depth = 8;     // singular extension minimum depth
    int singular_margin = 2;        // singular beta = ttvalue - margin*depth
    int probcut_min_depth = 5;      // ProbCut needs at least this much depth
    int probcut_margin = 180;       // probcut beta = beta + this
    int probcut_depth_reduction = 4; // verification search depth = depth - this
    int lmr_min_depth = 3;          // late move reductions minimum depth
    // lmr_hist_bad is dead for the same reason as history_prune_margin above,
    // and under the same measurements -- do not retune it in isolation.
    // Helper threads start their iterative deepening a few iterations apart so
    // they do not all walk the identical path. The offset must wrap: it used to
    // be the raw thread id, so thread N began at depth N+1, and a time-based
    // search passes MAX_PLY (64) as the target. On a 32-thread search that gave
    // the upper half of the threads a first iteration of depth 17 to 32 with no
    // iterative-deepening warmup behind it -- searches no move's time budget
    // can finish, so those cores contributed nothing but TT traffic.
    //
    // Deliberately *not* registered with the tuner. It only changes behaviour
    // with helper threads running, and the tuner drives a single-threaded
    // search, so SPSA would perturb it against an identically zero gradient --
    // which is exactly what EverySearchParameterReachesTheSearch asserts
    // cannot happen. Registering it fails that test, correctly.
    //
    // The span itself is a heuristic, not a measured optimum: the differences
    // between spans of 2, 4 and 8 in the table in docs/thread-scaling.md are
    // smaller than the run-to-run spread of the measurement that produced
    // them. What is measured is that *some* bound beats none at 32 threads.
    int smp_stagger_span = 4;       // helper start depths wrap modulo this

    int lmr_hist_bad = 2000;        // reduce one extra ply below -this
    int lmr_hist_good = 4000;       // reduce one less ply above this
    int best_move_bonus = 2;        // best-move history bonus, times depth
    int history_bonus_scale = 1;    // history bonus is scale * depth^2
    int history_malus_pct = 0;      // fail-low penalty, percent of the bonus (0 = off)
    /// Internal iterative reduction: with no hash move, search one ply shallower
    /// so the next visit has one. Both of these were bare literals in search.cpp
    /// and so invisible to SPSA, which is the whole reason this block exists.
    int iir_min_depth = 4;          // IIR only at or above this depth
    int iir_cut_margin = 200;       // ...and at a cut node only if eval + this >= beta
    /// The two shallow-quiet rules near the bottom of the move loop: one extra
    /// reduction for an uninteresting quiet, one extra ply for a quiet that
    /// answers a threat or pushes a pawn. Both were spelled `depth <= 2`.
    int lmr_extra_max_depth = 2;    // extra reduction for dull quiets at or below this
    int quiet_ext_max_depth = 2;    // extension for dangerous quiets at or below this

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
