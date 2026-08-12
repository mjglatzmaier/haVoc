#pragma once

/// @file hce.hpp
/// @brief Hand-Crafted Evaluation function.

#include "havoc/eval/evaluator.hpp"
#include "havoc/material_table.hpp"
#include "havoc/parameters.hpp"
#include "havoc/pawn_table.hpp"

namespace havoc {

struct endgame_info {
    bool evaluated_fence = false;
    bool is_fence = false;
};

/// Evaluation info gathered during HCE evaluation.
struct einfo {
    pawn_entry* pe = nullptr;
    material_entry* me = nullptr;
    endgame_info endgame;
    U64 pawn_holes[2]{};
    U64 all_pieces = 0;
    U64 pieces[2]{};
    U64 weak_pawns[2]{};
    U64 empty = 0;
    U64 kmask[2]{};
    U64 kattk_points[2][5]{};
    U64 piece_attacks[2][5]{};
    bool bishop_colors[2][2]{};
    U64 central_pawns[2]{};
    U64 queen_sqs[2]{};
    /// Own pawns standing on light and on dark squares, indexed by colour.
    /// Used to penalise a bishop for its own pawns fixed on its own colour.
    U64 light_sq_pawns[2]{};
    U64 dark_sq_pawns[2]{};
    bool closed_center = false;
    unsigned kattackers[2][5]{};
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
