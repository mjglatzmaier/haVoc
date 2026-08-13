#include "havoc/kpk.hpp"

#include "havoc/bitboard.hpp"

#include <array>
#include <bitset>
#include <cstdint>

namespace havoc::kpk {

namespace {

/// 4 pawn files (a-d, the rest reached by mirroring) x 6 pawn ranks (2-7)
/// x 64 white king squares x 64 black king squares x 2 sides to move.
constexpr int kPawnSquares = 24;
constexpr int kMaxIndex = kPawnSquares * 64 * 64 * 2;

/// Kept as flags rather than an enum so that the value of a position can be
/// accumulated over its successors with a plain OR.
enum State : uint8_t {
    kIllegal = 0,
    kUnknown = 1,
    kDrawn = 2,
    kWon = 4,
};

std::array<uint8_t, kMaxIndex> g_state;
std::bitset<kMaxIndex> g_won;
bool g_ready = false;
unsigned g_won_count = 0;

constexpr int row_of(int s) { return s >> 3; }
constexpr int col_of(int s) { return s & 7; }

constexpr int king_distance(int a, int b) {
    const int dr = row_of(a) - row_of(b);
    const int dc = col_of(a) - col_of(b);
    return std::max(dr < 0 ? -dr : dr, dc < 0 ? -dc : dc);
}

/// Packs a pawn on files a-d, ranks 2-7 into 0..23.
constexpr int pawn_index(int pawn) { return col_of(pawn) * 6 + (row_of(pawn) - 1); }

constexpr int index_of(Color stm, int wk, int bk, int pawn) {
    return ((pawn_index(pawn) * 64 + wk) * 64 + bk) * 2 + static_cast<int>(stm);
}

/// Squares a white pawn on `pawn` attacks.
U64 pawn_attacks(int pawn) {
    U64 m = 0ULL;
    if (col_of(pawn) > 0)
        m |= 1ULL << (pawn + 7);
    if (col_of(pawn) < 7)
        m |= 1ULL << (pawn + 9);
    return m;
}

/// Positions whose value is known without looking at any successor: illegal
/// placements, a promotion white can force through, and the two ways black
/// escapes -- taking the pawn, or having no move at all.
uint8_t classify_terminal(Color stm, int wk, int bk, int pawn) {
    // Kings may not touch, and neither king may stand on the pawn.
    if (king_distance(wk, bk) <= 1 || wk == pawn || bk == pawn)
        return kIllegal;

    // If white is to move, black cannot already be in check from the pawn.
    if (stm == white && (pawn_attacks(pawn) & (1ULL << bk)) != 0ULL)
        return kIllegal;

    if (stm == white) {
        // A pawn on the seventh promotes next move. That wins outright unless
        // white's own king is sitting on the promotion square, or black's king
        // guards it and white's does not.
        const int promo = pawn + 8;
        if (row_of(pawn) == 6 && wk != promo &&
            (king_distance(bk, promo) > 1 || king_distance(wk, promo) == 1))
            return kWon;
        return kUnknown;
    }

    // Black to move. Legal king destinations are those the white king does not
    // cover and the pawn does not attack; capturing the pawn is one of them
    // whenever white's king is not defending it.
    const U64 escape = bitboards::kmask[bk] & ~bitboards::kmask[wk] & ~pawn_attacks(pawn);
    if (escape == 0ULL)
        return kDrawn;  // stalemate
    if ((escape & (1ULL << pawn)) != 0ULL)
        return kDrawn;  // the pawn falls, leaving bare kings
    return kUnknown;
}

/// Recomputes one position from its successors.
///
/// White is trying to reach a won position and settles for a draw; black is
/// trying to reach a drawn one and settles for a loss. A position is only
/// resolved once every successor is, so anything still unknown keeps the
/// position unknown for this pass. Illegal successors contribute nothing,
/// which is exactly right: they are moves that cannot be played.
uint8_t recompute(Color stm, int wk, int bk, int pawn) {
    const uint8_t good = (stm == white) ? kWon : kDrawn;
    const uint8_t bad = (stm == white) ? kDrawn : kWon;
    uint8_t seen = kIllegal;

    if (stm == white) {
        U64 moves = bitboards::kmask[wk];
        while (moves) {
            const int to = bits::pop_lsb(moves);
            seen |= g_state[index_of(black, to, bk, pawn)];
        }
        // Single push. A push to the eighth rank is a promotion, which the
        // terminal test above has already ruled on, so it is not a successor
        // inside the table.
        if (row_of(pawn) < 6 && pawn + 8 != wk && pawn + 8 != bk)
            seen |= g_state[index_of(black, wk, bk, pawn + 8)];
        // Double push from the second rank, over an empty third rank.
        if (row_of(pawn) == 1 && pawn + 8 != wk && pawn + 8 != bk && pawn + 16 != wk &&
            pawn + 16 != bk)
            seen |= g_state[index_of(black, wk, bk, pawn + 16)];
    } else {
        U64 moves = bitboards::kmask[bk];
        while (moves) {
            const int to = bits::pop_lsb(moves);
            seen |= g_state[index_of(white, wk, to, pawn)];
        }
    }

    if (seen & good)
        return good;
    if (seen & kUnknown)
        return kUnknown;
    return bad;
}

}  // namespace

void init() {
    if (g_ready)
        return;

    for (int pi = 0; pi < kPawnSquares; ++pi) {
        const int pawn = (pi / 6) + 8 * ((pi % 6) + 1);
        for (int wk = 0; wk < 64; ++wk)
            for (int bk = 0; bk < 64; ++bk)
                for (int s = 0; s < 2; ++s)
                    g_state[index_of(static_cast<Color>(s), wk, bk, pawn)] =
                        classify_terminal(static_cast<Color>(s), wk, bk, pawn);
    }

    // Value iteration. Every pass can only turn unknowns into decided results,
    // so the table is monotone and the loop terminates; in practice it settles
    // in around twenty passes.
    bool changed = true;
    while (changed) {
        changed = false;
        for (int pi = 0; pi < kPawnSquares; ++pi) {
            const int pawn = (pi / 6) + 8 * ((pi % 6) + 1);
            for (int wk = 0; wk < 64; ++wk)
                for (int bk = 0; bk < 64; ++bk)
                    for (int s = 0; s < 2; ++s) {
                        const Color stm = static_cast<Color>(s);
                        const int idx = index_of(stm, wk, bk, pawn);
                        if (g_state[idx] != kUnknown)
                            continue;
                        const uint8_t v = recompute(stm, wk, bk, pawn);
                        if (v != kUnknown) {
                            g_state[idx] = v;
                            changed = true;
                        }
                    }
        }
    }

    g_won_count = 0;
    for (int i = 0; i < kMaxIndex; ++i) {
        const bool won = g_state[i] == kWon;
        g_won[i] = won;
        g_won_count += won;
    }
    g_ready = true;
}

bool probe(int strong_king, int pawn, int weak_king, Color stm) {
    return g_won[index_of(stm, strong_king, weak_king, pawn)];
}

unsigned won_count() { return g_won_count; }

}  // namespace havoc::kpk
