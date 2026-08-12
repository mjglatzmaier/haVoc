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

    [[nodiscard]] bool is_endgame() const { return endgame != EndgameType::none; }
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
