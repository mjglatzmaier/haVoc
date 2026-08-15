#include "havoc/pawn_table.hpp"

#include "havoc/bitboard.hpp"
#include "havoc/parameters.hpp"
#include "havoc/position.hpp"
#include "havoc/squares.hpp"
#include "havoc/utils.hpp"

#include <algorithm>
#include <cstring>
#include <vector>

namespace havoc {

namespace {

inline size_t prev_pow2(size_t x) {
    if (x <= 2)
        return x;
    return prev_pow2(x >> 1) << 1;
}

/// A pawn is backward when no pawn on a neighbouring file can ever defend it,
/// which is to say every one of them stands strictly in front of it.
///
/// The old implementation scanned each neighbouring file for its *most
/// advanced* pawn and asked whether that one was ahead. That is the wrong end
/// of the file: with white pawns on a2, a5 and b3, the a-file's most advanced
/// pawn is a5, which is ahead of b3, so b3 was called backward -- while a2 was
/// defending it. What decides the question is the rearmost neighbour, so the
/// test is simply whether any neighbouring pawn stands on this pawn's rank or
/// behind it. A pawn with no neighbours at all satisfies it vacuously, which
/// is deliberate and matches the old behaviour: the square in front of an
/// isolated pawn is as much a hole as the one in front of a backward pawn.
template <Color c> bool backward_pawn(int row, int col, U64 pawns) {
    const U64 behind_or_level = (c == white) ? ((row == 7) ? ~0ULL : ((1ULL << (8 * (row + 1))) - 1))
                                             : ~((1ULL << (8 * row)) - 1);
    return (bitboards::neighbor_cols[col] & pawns & behind_or_level) == 0ULL;
}

const float pawn_scaling[8] = {0.86f, 0.90f, 0.95f, 1.00f, 1.00f, 0.95f, 0.90f, 0.86f};
const float material_vals[5] = {100.0f, 300.0f, 315.0f, 480.0f, 910.0f};

/// Middlegame and endgame endpoints of one side's pawn-structure score. Only
/// the piece-square term is phase dependent; every other pawn term is a
/// structural judgement that does not taper, so it lands in both.
struct pawn_score {
    int16_t mg = 0;
    int16_t eg = 0;
    int16_t material = 0;

    void operator+=(int v) {
        mg = static_cast<int16_t>(mg + v);
        eg = static_cast<int16_t>(eg + v);
    }
    void operator-=(int v) {
        mg = static_cast<int16_t>(mg - v);
        eg = static_cast<int16_t>(eg - v);
    }
    /// Subtract a penalty that differs between the middlegame and the endgame.
    void sub(int v_mg, int v_eg) {
        mg = static_cast<int16_t>(mg - v_mg);
        eg = static_cast<int16_t>(eg - v_eg);
    }
    /// Add a bonus that differs between the middlegame and the endgame.
    void add(int v_mg, int v_eg) {
        mg = static_cast<int16_t>(mg + v_mg);
        eg = static_cast<int16_t>(eg + v_eg);
    }
};

template <Color c> pawn_score evaluate_pawns(const position& p, pawn_entry& e, const parameters& par) {
    constexpr Color them = Color(c ^ 1);

    U64 pawns = p.get_pieces<c, pawn>();
    U64 epawns = p.get_pieces<them, pawn>();

    Square* sqs = p.squares_of<c, pawn>();

    pawn_score score;
    U64 locked_bb = 0ULL;

    for (Square s = *sqs; s != no_square; s = *++sqs) {
        U64 fbb = bitboards::squares[s];
        int row = util::row(s);
        int col_idx = util::col(s);

        // Phase 0 is pure middlegame and 24 pure endgame; take both ends and
        // let the consumer interpolate against the position's actual phase.
        score.mg = static_cast<int16_t>(
            score.mg + par.sq_score_scaling[pawn] * square_score<c>(par, pawn, s, 0));
        score.eg = static_cast<int16_t>(
            score.eg + par.sq_score_scaling[pawn] * square_score<c>(par, pawn, s, 24));
        score.material =
            static_cast<int16_t>(score.material + pawn_scaling[col_idx] * material_vals[pawn]);

        // Pawn attacks, and every square this pawn could ever attack if it
        // kept advancing. passpawn_mask is the own file plus both neighbours
        // ahead of the pawn, so intersecting it with the neighbour files
        // leaves exactly the squares it can ever cover.
        e.attacks[c] |= bitboards::pattks[c][s];
        e.attack_span[c] |= bitboards::pattks[c][s] |
                            (bitboards::passpawn_mask[c][s] & bitboards::neighbor_cols[col_idx]);

        // Track undefended pawns
        auto defend_mask = (c == white ? bitboards::pattks[black][s] : bitboards::pattks[white][s]);
        auto defenders = pawns & defend_mask;
        if (defenders == 0ULL) {
            score.sub(par.undefended_pawn_penalty_mg, par.undefended_pawn_penalty_eg);
            e.undefended[c] |= fbb;
        }

        // Connected pawns. `defenders` above is exactly the set of friendly
        // pawns defending this square, so support costs nothing extra to
        // detect; a phalanx is a friendly pawn on an adjacent file and the
        // same rank. Both are scored by the pawn's rank relative to its own
        // side, since a connected pair matters more the further it has
        // advanced. A pawn that is both defended and abreast collects both.
        {
            int rel_rank = (c == white ? row : 7 - row);
            if (bitboards::neighbor_cols[col_idx] & pawns & bitboards::row[row])
                score.add(par.phalanx_pawn_mg[rel_rank], par.phalanx_pawn_eg[rel_rank]);
            if (defenders)
                score.add(par.supported_pawn_mg[rel_rank], par.supported_pawn_eg[rel_rank]);
        }

        // Passed pawns
        U64 mask = bitboards::passpawn_mask[c][s] & epawns;
        if (mask == 0ULL) {
            e.passed[c] |= fbb;
        }

        // Isolated pawns
        U64 neighbors_bb = bitboards::neighbor_cols[col_idx] & pawns;
        const bool is_isolated = (neighbors_bb == 0ULL);
        if (is_isolated) {
            e.isolated[c] |= fbb;
            score.sub(par.isolated_pawn_penalty_mg, par.isolated_pawn_penalty_eg);
        }

        // Backward pawns. An isolated pawn trivially satisfies the backward
        // test (no neighbour can ever defend it), so charging both would levy
        // two penalties for one weakness and make the two weights collinear --
        // a tuner can then only ever fit their sum. The bitboard still records
        // it, because the square in front of an isolated pawn is just as much
        // a hole as the one in front of a backward pawn and the outpost code
        // reads that; only the penalty is made exclusive.
        const bool is_backward = backward_pawn<c>(row, col_idx, pawns);
        if (is_backward) {
            e.backward[c] |= fbb;
            if (!is_isolated)
                score.sub(par.backward_pawn_penalty_mg, par.backward_pawn_penalty_eg);
        }

        // Square color
        if (bitboards::colored_sqs[white] & fbb)
            e.light[c] |= fbb;
        if (bitboards::colored_sqs[black] & fbb)
            e.dark[c] |= fbb;

        // Doubled pawns
        U64 doubled = bitboards::col[col_idx] & pawns;
        if (bits::more_than_one(doubled)) {
            e.doubled[c] |= doubled;
            if (e.isolated[c] & doubled)
                score.sub(par.doubled_isolated_penalty_mg, par.doubled_isolated_penalty_eg);
            else
                score.sub(par.doubled_pawn_penalty_mg, par.doubled_pawn_penalty_eg);
        }

        // Semi-open files
        U64 column = bitboards::col[col_idx];
        if ((column & epawns) == 0ULL) {
            if ((fbb & e.backward[c]) && !is_isolated)
                score.sub(par.backward_pawn_semiopen_mg, par.backward_pawn_semiopen_eg);
            if (fbb & e.isolated[c])
                score.sub(par.isolated_pawn_semiopen_mg, par.isolated_pawn_semiopen_eg);
            if (fbb & e.doubled[c])
                score.sub(par.doubled_pawn_semiopen_mg, par.doubled_pawn_semiopen_eg);

        }

        // King/queen side pawns
        if (util::col(s) <= Col::D)
            e.queenside[c] |= fbb;
        else
            e.kingside[c] |= fbb;

        // Locked center pawns
        if ((bitboards::squares[s] & bitboards::small_center_mask) != 0ULL) {
            Square front_sq = Square(c == white ? s + 8 : s - 8);
            if (util::on_board(front_sq)) {
                U64 front_bb = bitboards::squares[front_sq];
                e.center_pawn_count++;
                if (epawns & front_bb)
                    locked_bb |= front_bb;
            }
        }
    }

    if (bits::count(locked_bb) >= 2)
        e.locked_center = true;

    return score;
}

} // namespace

// ─── pawn_table implementation ──────────────────────────────────────────────

pawn_table::pawn_table(const parameters& params) : params_(&params) {
    init();
}

void pawn_table::init() {
    sz_mb_ = 10 * 1024;
    count_ = 1024 * sz_mb_ / sizeof(pawn_entry);
    count_ = prev_pow2(count_);
    if (count_ < 1024)
        count_ = 1024;
    entries_ = std::make_unique<pawn_entry[]>(count_);
    clear();
}

void pawn_table::clear() {
    std::fill_n(entries_.get(), count_, pawn_entry{});
}

pawn_entry* pawn_table::fetch(const position& p) const {
    U64 k = p.pawnkey();
    const size_t idx = k & (count_ - 1);
    if (entries_[idx].key == k) {
        return &entries_[idx];
    }
    entries_[idx] = {};
    entries_[idx].key = k;
    const pawn_score w = evaluate_pawns<white>(p, entries_[idx], *params_);
    const pawn_score b = evaluate_pawns<black>(p, entries_[idx], *params_);
    entries_[idx].score_mg = static_cast<int16_t>(w.mg - b.mg);
    entries_[idx].score_eg = static_cast<int16_t>(w.eg - b.eg);
    entries_[idx].material = static_cast<int16_t>(w.material - b.material);
    return &entries_[idx];
}

} // namespace havoc
