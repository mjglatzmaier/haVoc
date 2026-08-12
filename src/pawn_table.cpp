#include "havoc/pawn_table.hpp"

#include "havoc/bitboard.hpp"
#include "havoc/parameters.hpp"
#include "havoc/position.hpp"
#include "havoc/squares.hpp"
#include "havoc/utils.hpp"

#include <cstring>
#include <vector>

namespace havoc {

namespace {

inline size_t prev_pow2(size_t x) {
    if (x <= 2)
        return x;
    return prev_pow2(x >> 1) << 1;
}

template <Color c> bool backward_pawn(int row, int col, U64 pawns) {
    int left = col - 1 < Col::A ? -1 : col - 1;
    int right = col + 1 > Col::H ? -1 : col + 1;
    bool left_greater = false;
    bool right_greater = false;

    if constexpr (c == white) {
        if (left != -1) {
            int sq = -1;
            U64 left_pawns = bitboards::col[left] & pawns;
            bool no_left_pawns = (left_pawns == 0ULL);
            while (left_pawns) {
                int tmp = bits::pop_lsb(left_pawns);
                if (tmp > sq)
                    sq = tmp;
            }
            left_greater = (sq > 0 && util::row(sq) > row) || no_left_pawns;
        }
        if (right != -1) {
            int sq = -1;
            U64 right_pawns = bitboards::col[right] & pawns;
            bool no_right_pawns = (right_pawns == 0ULL);
            while (right_pawns) {
                int tmp = bits::pop_lsb(right_pawns);
                if (tmp > sq)
                    sq = tmp;
            }
            right_greater = (sq > 0 && util::row(sq) > row) || no_right_pawns;
        }
    } else {
        if (left != -1) {
            int sq = 100;
            U64 left_pawns = bitboards::col[left] & pawns;
            bool no_left_pawns = (left_pawns == 0ULL);
            while (left_pawns) {
                int tmp = bits::pop_lsb(left_pawns);
                if (tmp < sq)
                    sq = tmp;
            }
            left_greater = (sq < 100 && util::row(sq) < row) || no_left_pawns;
        }
        if (right != -1) {
            int sq = 100;
            U64 right_pawns = bitboards::col[right] & pawns;
            bool no_right_pawns = (right_pawns == 0ULL);
            while (right_pawns) {
                int tmp = bits::pop_lsb(right_pawns);
                if (tmp < sq)
                    sq = tmp;
            }
            right_greater = (sq < 100 && util::row(sq) < row) || no_right_pawns;
        }
    }

    return (left == -1 && right_greater) || (right == -1 && left_greater) ||
           (left_greater && right_greater);
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
        U64 front = (c == white ? bitboards::squares[s + 8] : bitboards::squares[s - 8]);
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

        // Pawn attacks
        e.attacks[c] |= bitboards::pattks[c][s];

        // Track undefended pawns
        auto defend_mask = (c == white ? bitboards::pattks[black][s] : bitboards::pattks[white][s]);
        auto defenders = pawns & defend_mask;
        if (defenders == 0ULL) {
            score -= 1;
            e.undefended[c] |= fbb;
        }

        // Passed pawns
        U64 mask = bitboards::passpawn_mask[c][s] & epawns;
        if (mask == 0ULL) {
            e.passed[c] |= fbb;
            e.weak_squares[c] |= front;
        }

        // Isolated pawns
        U64 neighbors_bb = bitboards::neighbor_cols[col_idx] & pawns;
        if (neighbors_bb == 0ULL) {
            e.isolated[c] |= fbb;
            score -= par.isolated_pawn_penalty;
            e.weak_squares[c] |= front;
        }

        // Backward pawns
        if (backward_pawn<c>(row, col_idx, pawns)) {
            e.backward[c] |= fbb;
            score -= par.backward_pawn_penalty;
            e.weak_squares[c] |= front;
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
                score -= static_cast<int16_t>(2 * par.doubled_pawn_penalty);
            else
                score -= par.doubled_pawn_penalty;
        }

        // Semi-open files
        U64 column = bitboards::col[col_idx];
        if ((column & epawns) == 0ULL) {
            e.semiopen[c] |= fbb;
            if (fbb & e.backward[c])
                score -= static_cast<int16_t>(2 * par.backward_pawn_penalty);
            if (fbb & e.isolated[c])
                score -= static_cast<int16_t>(2 * par.isolated_pawn_penalty);
            if (fbb & e.doubled[c])
                score -= static_cast<int16_t>(2 * par.doubled_pawn_penalty);
            e.weak_squares[c] |= front;
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
    std::memset(entries_.get(), 0, count_ * sizeof(pawn_entry));
}

pawn_entry* pawn_table::fetch(const position& p) const {
    U64 k = p.pawnkey();
    unsigned idx = k & (count_ - 1);
    if (entries_[idx].key == k) {
        return &entries_[idx];
    }
    std::memset(&entries_[idx], 0, sizeof(pawn_entry));
    entries_[idx].key = k;
    const pawn_score w = evaluate_pawns<white>(p, entries_[idx], *params_);
    const pawn_score b = evaluate_pawns<black>(p, entries_[idx], *params_);
    entries_[idx].score_mg = static_cast<int16_t>(w.mg - b.mg);
    entries_[idx].score_eg = static_cast<int16_t>(w.eg - b.eg);
    entries_[idx].material = static_cast<int16_t>(w.material - b.material);
    return &entries_[idx];
}

} // namespace havoc
