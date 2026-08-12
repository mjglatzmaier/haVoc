#pragma once

/// @file pawn_table.hpp
/// @brief Pawn structure hash table for caching pawn evaluations.

#include "havoc/types.hpp"

#include <cstring>
#include <memory>

namespace havoc {

class position;
struct parameters;

struct pawn_entry {
    U64 key = 0;
    /// The pawn hash is keyed on pawn structure alone, so a cached entry has
    /// to be usable at any game phase. Both endpoints of the taper are stored
    /// and the consumer interpolates; storing a single pre-tapered score would
    /// hand whichever phase happened to fill the slot first to every later
    /// probe of the same structure.
    int16_t score_mg = 0;
    int16_t score_eg = 0;

    U64 doubled[2]{};
    U64 isolated[2]{};
    U64 backward[2]{};
    U64 passed[2]{};
    U64 dark[2]{};
    U64 light[2]{};
    U64 king[2]{};
    U64 attacks[2]{};
    U64 undefended[2]{};
    U64 weak_squares[2]{};
    U64 chaintips[2]{};
    U64 chainbases[2]{};
    U64 queenside[2]{};
    U64 kingside[2]{};
    U64 semiopen[2]{};
    int16_t center_pawn_count = 0;
    bool locked_center = false;
};

class pawn_table {
    size_t sz_mb_ = 0;
    size_t count_ = 0;
    std::unique_ptr<pawn_entry[]> entries_;
    const parameters* params_ = nullptr;

    void init();

  public:
    explicit pawn_table(const parameters& params);
    pawn_table(const pawn_table&) = delete;
    pawn_table& operator=(const pawn_table&) = delete;
    ~pawn_table() = default;

    void clear();
    [[nodiscard]] pawn_entry* fetch(const position& p) const;
};

} // namespace havoc
