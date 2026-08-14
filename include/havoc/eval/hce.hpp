#pragma once

/// @file hce.hpp
/// @brief Hand-Crafted Evaluation function.

#include "havoc/eval/evaluator.hpp"
#include "havoc/material_table.hpp"
#include "havoc/parameters.hpp"
#include "havoc/pawn_table.hpp"

namespace havoc {

struct einfo {
    pawn_entry* pe = nullptr;
    material_entry* me = nullptr;
    U64 pawn_holes[2]{};
    U64 all_pieces = 0;
    U64 pieces[2]{};
    U64 weak_pawns[2]{};
    U64 empty = 0;
    U64 kmask[2]{};
    /// The squares an attack on which counts as an attack on the king.
    /// Wider than kmask: the ring plus the three squares two ranks ahead of
    /// the king, shifted back onto the board for kings on the edge files.
    /// kmask stays the true ring and is what defends, shelters and gives the
    /// king its flight squares; kzone is only ever read to decide whether an
    /// enemy piece is pointed at the king.
    U64 kzone[2]{};
    U64 kattk_points[2][5]{};
    U64 piece_attacks[2][5]{};
    U64 queen_sqs[2]{};
    /// Own pawns standing on light and on dark squares, indexed by colour.
    /// Used to penalise a bishop for its own pawns fixed on its own colour.
    U64 light_sq_pawns[2]{};
    U64 dark_sq_pawns[2]{};
    unsigned kattackers[2][5]{};
    /// Three running totals that are collected inside the piece, king and pawn
    /// terms but must not be scaled by the category factor those terms sit
    /// under. Each category scale is supposed to control one coherent idea,
    /// and without these it does not:
    ///
    ///   mobility already carries mobility_category_scale, applied inside each
    ///   piece function. Leaving it inside the piece total multiplied it by
    ///   sq_score_category_scale as well, so the two scales compounded and the
    ///   tuner could trade one against the other along a flat direction
    ///   instead of fitting either.
    ///
    ///   king_placement is the king piece-square score, which is positional
    ///   king activity, not danger. Inside the king total it was scaled by
    ///   king_safety_category_scale, so raising king safety silently pulled
    ///   the king towards or away from the centre.
    ///
    ///   king_pressure is a pawn attacking the enemy king ring. It is
    ///   collected in the pawn term and so was scaled by
    ///   pawn_structure_category_scale, which is a scale for weak pawns.
    ///
    /// All three are exact no-ops at the default scales of 100, since integer
    /// (x * 100) / 100 is x. They change what the scales mean, not what the
    /// evaluation currently returns.
    int mobility[2]{};
    int king_placement[2]{};
    int king_pressure[2]{};
};

/// Hand-Crafted Evaluation function.
class HCEEvaluator : public IEvaluator {
  public:
    HCEEvaluator(pawn_table& pt, material_table& mt, const parameters& params);

    [[nodiscard]] int evaluate(const position& pos, int lazy_margin = -1) override;
    [[nodiscard]] std::string name() const override { return "HCE"; }

  private:
    pawn_table& pawn_table_;
    material_table& material_table_;
    const parameters& params_;

    template <Color c> int eval_pawns(const position& p, einfo& ei);
    template <Color c> int eval_knights(const position& p, einfo& ei);
    template <Color c> int eval_bishops(const position& p, einfo& ei);
    template <Color c> int eval_rooks(const position& p, einfo& ei);
    template <Color c> int eval_queens(const position& p, einfo& ei);
    template <Color c> int eval_king(const position& p, einfo& ei);
    template <Color c> int eval_space(const position& p, einfo& ei);
    template <Color c> int eval_threats(const position& p, einfo& ei);
    template <Color c> int eval_passed_pawns(const position& p, einfo& ei);
    template <Color c> int eval_kpk(const position& p, einfo& ei);
    template <Color c> int eval_krrk(const position& p, einfo& ei);
    template <Color c> int eval_knbk(const position& p, einfo& ei);
    template <Color c> int eval_krk(const position& p, einfo& ei);
    template <Color c> int eval_kqk(const position& p, einfo& ei);
    template <Color c> int eval_kbnk(const position& p, einfo& ei);
    template <Color c> bool trapped_rook(const position& p, einfo& ei, Square rs);

    // Endgame helpers
    template <Color c> bool has_opposition(const position& p, einfo& ei);
    template <Color c> float eval_passed_kpk(const position& p, einfo& ei, Square f, bool has_opp);
    template <Color c> float eval_passed_krrk(const position& p, einfo& ei, Square f, bool has_opp);
    template <Color c> float eval_passed_knbk(const position& p, einfo& ei, Square f, bool has_opp);
};

} // namespace havoc
