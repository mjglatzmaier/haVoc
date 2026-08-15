#include "havoc/material_table.hpp"

#include "havoc/bitboard.hpp"
#include "havoc/parameters.hpp"
#include "havoc/position.hpp"

#include <algorithm>
#include <cstring>

namespace havoc {

namespace {

int16_t evaluate_material(const position& p, material_entry& e, const parameters& params) {
    constexpr int sign[2] = {1, -1};
    constexpr Piece pieces[4] = {knight, bishop, rook, queen};

    // Piece values come from the tunable parameter set. Pawns are deliberately
    // absent from this loop: they are scored by the pawn table, so counting
    // them here as well would be a double count.

    int16_t total_score = 0;
    e.endgame = EndgameType::none;
    int eg_pieces[2][5] = {};
    int total[2] = {};

    for (Color c = white; c <= black; ++c) {
        for (auto piece : pieces) {
            int n = static_cast<int>(p.number_of(c, piece));
            e.number[piece] += static_cast<U8>(n);
            total_score += static_cast<int16_t>(sign[c] * n * params.material_value[piece]);
            total[c] += n;
            eg_pieces[c][piece] += n;
        }
    }

    int total_eg = total[white] + total[black];

    // Endgame classification
    if (total_eg == 0)
        e.endgame = EndgameType::KpK;
    else if (total_eg == 1 && (eg_pieces[white][rook] == 1 || eg_pieces[black][rook] == 1))
        e.endgame = EndgameType::KRK;
    else if (total_eg == 1 && (eg_pieces[white][queen] == 1 || eg_pieces[black][queen] == 1))
        e.endgame = EndgameType::KQK;
    else if (total_eg == 2 && ((eg_pieces[white][bishop] == 1 && eg_pieces[white][knight] == 1 &&
                                total[black] == 0) ||
                               (eg_pieces[black][bishop] == 1 && eg_pieces[black][knight] == 1 &&
                                total[white] == 0)))
        e.endgame = EndgameType::KBNK;
    else if (total_eg == 2 && eg_pieces[white][rook] == 1 && eg_pieces[black][rook] == 1)
        e.endgame = EndgameType::KrrK;
    else if (total_eg == 2 && eg_pieces[white][bishop] == 1 && eg_pieces[black][knight] == 1)
        e.endgame = EndgameType::KbnK;
    else if (total_eg == 2 && eg_pieces[white][knight] == 1 && eg_pieces[black][bishop] == 1)
        e.endgame = EndgameType::KnbK;
    else if (total_eg == 2 && eg_pieces[white][bishop] == 1 && eg_pieces[black][bishop] == 1)
        e.endgame = EndgameType::KbbK;
    else if (total_eg == 2 && eg_pieces[white][knight] == 1 && eg_pieces[black][knight] == 1)
        e.endgame = EndgameType::KnnK;
    else if (total_eg == 1 && (eg_pieces[white][bishop] == 1 || eg_pieces[black][bishop] == 1))
        e.endgame = EndgameType::KbK;
    else if (total_eg == 1 && (eg_pieces[white][knight] == 1 || eg_pieces[black][knight] == 1))
        e.endgame = EndgameType::KnK;
    else if (total_eg <= 2)
        e.endgame = EndgameType::Unknown;

    // Game phase: 0 = middlegame (full board), 24 = endgame (bare board).
    int game_phase = 24;
    game_phase -= e.number[queen] * 4;
    game_phase -= e.number[rook] * 2;
    game_phase -= e.number[bishop];
    game_phase -= e.number[knight];
    e.phase_interpolant = std::clamp(game_phase, 0, 24);

    return total_score;
}

} // namespace

material_table::material_table(const parameters& params) : params_(&params) {
    init();
}

void material_table::init() {
    // A power of two, so that k & (count_ - 1) can reach every slot. The old
    // size of 1,638,400 entries is not a power of two: masking with 0x18FFFF
    // left 84% of a 50 MB allocation unreachable, and the table behaved like a
    // 262,144-slot one that cost 50 MB. With a genuine material key 131,072
    // slots hold 97.2% of probes, within noise of what the 50 MB table managed.
    count_ = 128 * 1024;
    sz_mb_ = count_ * sizeof(material_entry) / (1024 * 1024);
    entries_ = std::make_unique<material_entry[]>(count_);
}

void material_table::clear() {
    std::fill_n(entries_.get(), count_, material_entry{});
}

material_entry* material_table::fetch(const position& p) const {
    U64 k = p.material_key();
    const size_t idx = k & (count_ - 1);
    if (entries_[idx].key == k) {
        return &entries_[idx];
    }
    entries_[idx] = {};
    entries_[idx].key = k;
    entries_[idx].score = evaluate_material(p, entries_[idx], *params_);
    return &entries_[idx];
}

} // namespace havoc
