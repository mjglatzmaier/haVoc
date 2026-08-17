#pragma once

#include <array>
#include <cstdint>
#include <string_view>

namespace havoc {

/// Unsigned 64-bit integer, used for bitboards and hash keys.
using U64 = uint64_t;
/// Unsigned 32-bit integer.
using U32 = uint32_t;
/// Unsigned 16-bit integer, used for castling rights.
using U16 = uint16_t;
/// Unsigned 8-bit integer, used for square/piece indices.
using U8 = uint8_t;

// ---------------------------------------------------------------------------
// Enums — plain enums so they can serve as array indices.
// ---------------------------------------------------------------------------

/// Movement classification (promotions, captures, castles, etc.).
enum Movetype {
    promotion_q,
    promotion_r,
    promotion_b,
    promotion_n,
    capture_promotion_q,
    capture_promotion_r,
    capture_promotion_b,
    capture_promotion_n,
    castle_ks,
    castle_qs,
    quiet,
    capture,
    ep,
    castles,
    pseudo_legal,
    promotion,
    capture_promotion,
    no_type
};

/// True if a move of type `mt` removes an enemy piece from the board.
///
/// Capture-promotions count: they take a piece as well as making one, so any
/// table keyed on what was taken has to see them. The `capture_promotion` and
/// `pseudo_legal` entries above are classification tags the generator uses
/// internally, not types a made move carries, so they are deliberately absent.
constexpr bool is_capture(Movetype mt) {
    return mt == capture || mt == ep || mt == capture_promotion_q ||
           mt == capture_promotion_r || mt == capture_promotion_b || mt == capture_promotion_n;
}

/// Piece types (pawn=0 .. king=5).
enum Piece { pawn, knight, bishop, rook, queen, king, pieces, no_piece };

/// Side to move (white=0, black=1).
enum Color { white, black, colors, no_color };

/// Board squares in little-endian rank-file order (A1=0 .. H8=63).
enum Square {
    A1,
    B1,
    C1,
    D1,
    E1,
    F1,
    G1,
    H1,
    A2,
    B2,
    C2,
    D2,
    E2,
    F2,
    G2,
    H2,
    A3,
    B3,
    C3,
    D3,
    E3,
    F3,
    G3,
    H3,
    A4,
    B4,
    C4,
    D4,
    E4,
    F4,
    G4,
    H4,
    A5,
    B5,
    C5,
    D5,
    E5,
    F5,
    G5,
    H5,
    A6,
    B6,
    C6,
    D6,
    E6,
    F6,
    G6,
    H6,
    A7,
    B7,
    C7,
    D7,
    E7,
    F7,
    G7,
    H7,
    A8,
    B8,
    C8,
    D8,
    E8,
    F8,
    G8,
    H8,
    squares,
    no_square
};

/// Rank indices (r1=0 .. r8=7).
enum Row { r1, r2, r3, r4, r5, r6, r7, r8, rows, no_row };

/// File indices (A=0 .. H=7).
enum Col { A, B, C, D, E, F, G, H, cols, no_col };

/// Search depth limits.
enum Depth { ZERO = 0, MAX_PLY = 64 };

/// Node types for alpha-beta search.
enum Nodetype { root, pv, non_pv, searching = 128 };

/// Move ordering phases.
enum OrderPhase {
    hash_move,
    mate_killer1,
    mate_killer2,
    good_captures,
    killer1,
    killer2,
    bad_captures,
    quiets,
    end
};

/// Endgame material configurations (encoded as piece bit patterns).
enum EndgameType {
    none = -1,
    KpK = 0,
    KnK = 1,
    KNK = 16,
    KbK = 2,
    KBK = 32,
    KnnK = 17,
    KnbK = 18,
    KnrK = 20,
    KnqK = 24,
    KbnK = 33,
    KbbK = 34,
    KbrK = 36,
    KbqK = 40,
    KrnK = 65,
    KrbK = 66,
    KrrK = 68,
    KrqK = 72,
    KqnK = 129,
    KqbK = 130,
    KqrK = 132,
    KqqK = 136,
    // Single piece vs bare king
    KRK = 200,  // King + Rook vs King
    KQK = 201,  // King + Queen vs King
    KBNK = 202, // King + Bishop + Knight vs King
    Unknown = 250
};

/// Castling rights bitmask values.
enum CastleRight {
    wks = 1,
    wqs = 2,
    bks = 4,
    bqs = 8,
    cr_none = 0,
    clearbqs = 7,
    clearbks = 11,
    clearwqs = 13,
    clearwks = 14,
    clearb = 3,
    clearw = 12
};

/// Castling rights surviving a move that touches a given square.
///
/// A right dies when the king leaves its home square, when the rook leaves its
/// corner, and -- the case that used to be missed entirely -- when the rook is
/// captured on its corner. Indexing by both the origin and the destination of
/// a move covers all three without special-casing captures: the destination of
/// a capture on a1/h1/a8/h8 is exactly the corner whose right is lost.
[[nodiscard]] constexpr U16 castle_rights_after(int sq) {
    switch (sq) {
    case 0:  return clearwqs; // A1
    case 7:  return clearwks; // H1
    case 4:  return clearw;   // E1
    case 56: return clearbqs; // A8
    case 63: return clearbks; // H8
    case 60: return clearb;   // E8
    default: return 15;
    }
}

// ---------------------------------------------------------------------------
// Score constants (replaces legacy Score enum).
// ---------------------------------------------------------------------------

/// Named score constants for search bounds and mate detection.
namespace score {
constexpr int16_t kInf = 10000;
constexpr int16_t kNegInf = -10000;
constexpr int16_t kMate = kInf - 1;
constexpr int16_t kMated = kNegInf + 1;
constexpr int16_t kMateMaxPly = kMate - 64;
constexpr int16_t kMatedMaxPly = kMated + 64;

/// Tablebase verdicts sit in their own band, immediately below the mate band
/// and above anything the evaluation can produce.
///
/// They need a band of their own for two reasons. A WDL probe proves the
/// result but says nothing about the distance to it, so it must not be
/// reported to a GUI as "mate in N" -- that would be a claim the engine cannot
/// back. And because the score carries a `- ply` term so the search prefers
/// converting sooner, it is ply-relative in exactly the way a mate score is,
/// and has to be adjusted on its way in and out of the transposition table for
/// the same reason.
constexpr int16_t kTbWin = kMateMaxPly - 1;
constexpr int16_t kTbLoss = static_cast<int16_t>(-kTbWin);
constexpr int16_t kTbWinMaxPly = static_cast<int16_t>(kTbWin - 64);
constexpr int16_t kTbLossMaxPly = static_cast<int16_t>(-kTbWinMaxPly);
constexpr int16_t kDraw = 0;
} // namespace score

// ---------------------------------------------------------------------------
// Move representation
// ---------------------------------------------------------------------------

/// Compact move encoding (from-square, to-square, move type).
struct Move {
    U8 f = 0;
    U8 t = 0;
    U8 type = static_cast<U8>(no_type);

    constexpr Move() = default;
    constexpr Move(U8 frm, U8 to, U8 mt) : f(frm), t(to), type(mt) {}

    constexpr bool operator==(const Move&) const = default;

    /// Set all move fields at once.
    constexpr void set(U8 frm, U8 to, Movetype mt) {
        f = frm;
        t = to;
        type = static_cast<U8>(mt);
    }

    /// Returns true if this move is the null/empty sentinel.
    [[nodiscard]] constexpr bool is_null() const {
        return f == t && type == static_cast<U8>(no_type);
    }
};

// ---------------------------------------------------------------------------
// Enum iteration operators
// ---------------------------------------------------------------------------

/// Trait to enable ++/-- iteration for specific enum types.
template <typename T> struct is_havoc_enum : std::false_type {};
template <> struct is_havoc_enum<Piece> : std::true_type {};
template <> struct is_havoc_enum<Color> : std::true_type {};
template <> struct is_havoc_enum<Square> : std::true_type {};
template <> struct is_havoc_enum<Row> : std::true_type {};
template <> struct is_havoc_enum<Col> : std::true_type {};
template <> struct is_havoc_enum<OrderPhase> : std::true_type {};

/// Prefix increment for iterable enums.
template <typename T>
    requires is_havoc_enum<T>::value
constexpr int operator++(T& e) {
    e = static_cast<T>(static_cast<int>(e) + 1);
    return static_cast<int>(e);
}

/// Prefix decrement for iterable enums.
template <typename T>
    requires is_havoc_enum<T>::value
constexpr int operator--(T& e) {
    e = static_cast<T>(static_cast<int>(e) - 1);
    return static_cast<int>(e);
}

/// Compound addition for iterable enums.
template <typename T, typename T2>
    requires is_havoc_enum<T>::value
constexpr T& operator+=(T& lhs, const T2& rhs) {
    lhs = static_cast<T>(lhs + rhs);
    return lhs;
}

/// Compound subtraction for iterable enums.
template <typename T, typename T2>
    requires is_havoc_enum<T>::value
constexpr T& operator-=(T& lhs, const T2& rhs) {
    lhs = static_cast<T>(lhs - rhs);
    return lhs;
}

// ---------------------------------------------------------------------------
// Compile-time lookup arrays
// ---------------------------------------------------------------------------

/// All piece types including sentinels.
constexpr std::array<Piece, 8> kPieces = {pawn,  knight, bishop, rook,
                                          queen, king,   pieces, no_piece};

/// SAN square names indexed by Square enum value.
constexpr std::array<std::string_view, 64> kSanSquares = {
    "a1", "b1", "c1", "d1", "e1", "f1", "g1", "h1", "a2", "b2", "c2", "d2", "e2", "f2", "g2", "h2",
    "a3", "b3", "c3", "d3", "e3", "f3", "g3", "h3", "a4", "b4", "c4", "d4", "e4", "f4", "g4", "h4",
    "a5", "b5", "c5", "d5", "e5", "f5", "g5", "h5", "a6", "b6", "c6", "d6", "e6", "f6", "g6", "h6",
    "a7", "b7", "c7", "d7", "e7", "f7", "g7", "h7", "a8", "b8", "c8", "d8", "e8", "f8", "g8", "h8",
};

/// SAN piece characters: P N B R Q K p n b r q k.
constexpr std::array<char, 12> kSanPiece = {'P', 'N', 'B', 'R', 'Q', 'K',
                                            'p', 'n', 'b', 'r', 'q', 'k'};

/// SAN file characters a-h.
constexpr std::array<char, 8> kSanCols = {'a', 'b', 'c', 'd', 'e', 'f', 'g', 'h'};

/// Convert a FEN castling character to its CastleRight bitmask.
[[nodiscard]] constexpr U16 castle_right_from_char(char c) {
    switch (c) {
    case 'K':
        return wks;
    case 'Q':
        return wqs;
    case 'k':
        return bks;
    case 'q':
        return bqs;
    default:
        return 0;
    }
}

// ---------------------------------------------------------------------------
// Square utility functions
// ---------------------------------------------------------------------------

/// Rank (0-7) of a square index.
[[nodiscard]] constexpr int sq_row(int s) {
    return s >> 3;
}

/// File (0-7) of a square index.
[[nodiscard]] constexpr int sq_col(int s) {
    return s & 7;
}

/// Absolute rank distance between two squares.
[[nodiscard]] constexpr int row_dist(int s1, int s2) {
    int d = sq_row(s1) - sq_row(s2);
    return d < 0 ? -d : d;
}

/// Absolute file distance between two squares.
[[nodiscard]] constexpr int col_dist(int s1, int s2) {
    int d = sq_col(s1) - sq_col(s2);
    return d < 0 ? -d : d;
}

/// True if a square index is in [0, 63].
[[nodiscard]] constexpr bool on_board(int s) {
    return s >= 0 && s <= 63;
}

/// True if two squares share the same rank.
[[nodiscard]] constexpr bool same_row(int s1, int s2) {
    return sq_row(s1) == sq_row(s2);
}

/// True if two squares share the same file.
[[nodiscard]] constexpr bool same_col(int s1, int s2) {
    return sq_col(s1) == sq_col(s2);
}

/// True if two squares lie on the same diagonal/anti-diagonal.
[[nodiscard]] constexpr bool on_diagonal(int s1, int s2) {
    return col_dist(s1, s2) == row_dist(s1, s2);
}

/// True if two squares are aligned (same rank, file, or diagonal).
[[nodiscard]] constexpr bool aligned(int s1, int s2) {
    return on_diagonal(s1, s2) || same_row(s1, s2) || same_col(s1, s2);
}

/// True if three squares are collinear.
[[nodiscard]] constexpr bool aligned(int s1, int s2, int s3) {
    return (same_col(s1, s2) && same_col(s1, s3)) || (same_row(s1, s2) && same_row(s1, s3)) ||
           (on_diagonal(s1, s3) && on_diagonal(s1, s2) && on_diagonal(s2, s3));
}

// ─── Game phase ─────────────────────────────────────────────────────────────
//
// The phase runs from 0 (opening, every piece still on) to kPhaseMax (bare
// kings). Every tapered term in the evaluation is blended by the one function
// below.
//
// It lives here, at the bottom of the include graph, because the alternative
// is what it replaced: the same expression written out by hand in four places
// -- material_entry::taper, square_score, the pawn-hash blend in hce.cpp, and
// the middlegame-only king and space terms -- two of them spelled with a bare
// literal 24 rather than the named constant. A phase model written four ways
// is a phase model that will eventually get changed in three of them.

/// Number of phase units between a full opening position and bare kings.
inline constexpr int kPhaseMax = 24;

/// Blends a middlegame and an endgame value by game phase.
///
/// @param mg    the value this term is worth with every piece on the board
/// @param eg    the value it is worth with bare kings
/// @param phase 0 (opening) to kPhaseMax (endgame)
///
/// Integer division truncates towards zero, which pulls both bonuses and
/// penalties slightly towards zero. That is symmetric, so it introduces no
/// side-to-move or sign bias, but it does mean a term tapered in several small
/// pieces loses more than the same term tapered once. Prefer to sum first and
/// taper the total.
[[nodiscard]] constexpr int taper(int mg, int eg, int phase) {
    return (mg * (kPhaseMax - phase) + eg * phase) / kPhaseMax;
}

} // namespace havoc
