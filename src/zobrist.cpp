#include "havoc/zobrist.hpp"

#include <array>

namespace havoc::zobrist {

// ---------------------------------------------------------------------------
// Zobrist random numbers
// ---------------------------------------------------------------------------
//
// These used to be a hard-coded table of 1835 constants inherited from the
// legacy engine. They were not random. Every value fitted in 32 bits and had
// a Hamming weight of at most 4 (mean 3.84 bits set), so all 1835 of them
// spanned a GF(2) space of dimension 32. With 1835 vectors in a 32-dimensional
// space the table was saturated with short linear dependencies: 1715 triples
// XORed to zero and 400785 quadruples satisfied a^b == c^d.
//
// A four-term dependency is a key collision between two positions that differ
// by only two pieces. Three of them involve a single piece type, so they are
// collisions between positions with *identical material*, reachable inside one
// game. The cleanest example: two white knights on a2 and b2 hash to exactly
// the same key as the same two knights on h4 and d6.
//
// That is not a theoretical hazard. is_draw() compares repetition keys for
// equality, so such a pair fabricates a repetition that never happened and the
// engine claims a draw in a position it may be winning. The transposition
// table would likewise return a validated hit belonging to a different
// position.
//
// Generated instead by splitmix64 from a fixed seed, so the table is still
// fully deterministic across runs and platforms, but the values are full
// 64-bit and statistically independent.

static constexpr U64 splitmix64(U64& state) {
    state += 0x9e3779b97f4a7c15ULL;
    U64 z = state;
    z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ULL;
    z = (z ^ (z >> 27)) * 0x94d049bb133111ebULL;
    return z ^ (z >> 31);
}

static constexpr std::array<U64, 1835> make_zobrist_rands() {
    std::array<U64, 1835> a{};
    U64 state = 0x2f6d1c4b8a3e5709ULL;
    for (auto& v : a)
        v = splitmix64(state);
    return a;
}

static constexpr std::array<U64, 1835> kZobristRands = make_zobrist_rands();
// clang-format on

// ---------------------------------------------------------------------------
// Internal tables
// ---------------------------------------------------------------------------

static U64 piece_rands[64][2][6];
static U64 castle_rands[2][16];
static U64 ep_rands[8];
static U64 stm_rands[2];

// ---------------------------------------------------------------------------
// Initialization — ported from legacy zobrist::load()
// ---------------------------------------------------------------------------

void init() {
    unsigned int idx = 0;

    // Piece keys
    for (Square sq = A1; sq <= H8; ++sq) {
        for (Color c = white; c <= black; ++c) {
            for (Piece p = pawn; p <= king; ++p, ++idx) {
                piece_rands[sq][c][p] = kZobristRands[idx];
            }
        }
    }

    // Castle rights keys
    for (Color c = white; c <= black; ++c) {
        for (int bit = 0; bit < 16; ++bit, ++idx) {
            castle_rands[c][bit] = kZobristRands[idx];
        }
    }

    // En-passant file keys
    for (int col = 0; col < 8; ++col, ++idx) {
        ep_rands[col] = kZobristRands[idx];
    }

    // Side-to-move keys
    stm_rands[white] = kZobristRands[idx++];
    stm_rands[black] = kZobristRands[idx++];
}

// ---------------------------------------------------------------------------
// Accessor functions
// ---------------------------------------------------------------------------

U64 piece(Square sq, Color c, Piece p) {
    return piece_rands[sq][c][p];
}
U64 castle(Color c, U16 rights) {
    return castle_rands[c][rights];
}

U64 castle_rights(U16 mask) {
    return castle_rands[0][mask & 15];
}
U64 ep(int file) {
    return ep_rands[file];
}
U64 stm(Color c) {
    return stm_rands[c];
}

} // namespace havoc::zobrist
