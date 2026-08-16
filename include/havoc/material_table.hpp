#pragma once

/// @file material_table.hpp
/// @brief Material hash table for caching material evaluation and game phase.

#include "havoc/types.hpp"

#include <cstring>
#include <memory>

namespace havoc {

class position;
struct parameters;

struct material_entry {
    U64 key = 0;
    int16_t score = 0;
    // 0 = middlegame (full board), 24 = endgame (bare board). The counter
    // starts at 24 and is decremented by the material still present, so the
    // starting position scores 0, not 24.
    int phase_interpolant = 0;
    EndgameType endgame = EndgameType::none;
    U8 number[5]{}; // indexed by Piece (knight..queen)

    /// True when the material matches a *specific* endgame this evaluation
    /// knows how to score, such as KPK or KRK.
    ///
    /// This is a statement about classification, not about game phase. It can
    /// only become true once at most two non-pawn pieces remain on the whole
    /// board, which over a random walk of 89,062 positions was 8.5% of them
    /// and only 24% of the positions with phase 16 or beyond. Anything that
    /// wants to know how far into the endgame we are wants the phase, not
    /// this: use taper() or mg_only().
    [[nodiscard]] bool is_endgame() const { return endgame != EndgameType::none; }

    /// Weight of the middlegame half of a tapered term, out of kPhaseMax.
    [[nodiscard]] int mg_weight() const { return kPhaseMax - phase_interpolant; }

    /// Weight of the endgame half of a tapered term, out of kPhaseMax.
    [[nodiscard]] int eg_weight() const { return phase_interpolant; }

    /// Blends a middlegame and an endgame value by this position's game phase.
    [[nodiscard]] int taper(int mg, int eg) const {
        return havoc::taper(mg, eg, phase_interpolant);
    }

    /// A term that is worth its full value in the opening and nothing at all
    /// with bare kings: pawn shelter, the king's open-file penalty, the pawn
    /// storm and space. Spelling these as taper(v, 0) rather than by hand
    /// keeps them on the same phase model as everything else.
    [[nodiscard]] int mg_only(int v) const { return taper(v, 0); }
};

class material_table {
    size_t sz_mb_ = 0;
    size_t count_ = 0;
    std::unique_ptr<material_entry[]> entries_;
    const parameters* params_ = nullptr;

    void init();

  public:
    explicit material_table(const parameters& params);
    material_table(const material_table&) = delete;
    material_table& operator=(const material_table&) = delete;
    ~material_table() = default;

    void clear();
    [[nodiscard]] material_entry* fetch(const position& p) const;
};

} // namespace havoc
