#pragma once

#include "havoc/bitboard.hpp"
#include "havoc/magics.hpp"
#include "havoc/position.hpp"
#include "havoc/types.hpp"

#include <iostream>
#include <vector>

namespace havoc {

/// Direction enum for pawn shift templates.
enum Dir { N, S, NN, SS, NW, NE, SW, SE, no_dir };

/// Move generator — generates pseudo-legal moves for a position.
class Movegen {
    int last = 0;
    Move list[218];
    Color us, them;
    U64 rank2, rank7;
    U64 empty, pawns, pawns2, pawns7;
    Square* knights;
    Square* bishops;
    Square* rooks;
    Square* queens;
    Square* kings;
    U64 enemies, all_pieces, qtarget, ctarget, check_target, evasion_target;
    Square eps;
    bool can_castle_ks_, can_castle_qs_;

    inline void initialize(const position& p);
    inline void pawn_caps(U64& left, U64& right, U64& ep_left, U64& ep_right);

    template <Movetype mt> inline void encode(U64& b, const int& f);
    template <Movetype mt> inline void encode_pawn_pushes(U64& b, const int& dir);

    inline void pawn_quiets(U64& single, U64& dbl);
    inline void encode_promotions(U64& b, const int& dir);
    inline void capture_promotions(U64& right_caps, U64& left_caps);
    inline void quiet_promotions(U64& quiets);
    inline void encode_capture_promotions(U64& b, const int& f);

  public:
    Movegen() = default;
    explicit Movegen(const position& pos) { initialize(pos); }
    Movegen(const Movegen&) = delete;
    Movegen(Movegen&&) = delete;
    ~Movegen() = default;

    Movegen& operator=(const Movegen&) = delete;
    Movegen& operator=(Movegen&&) = delete;
    Move& operator[](int idx) { return list[idx]; }

    template <Movetype mt, Piece p> inline void generate();
    template <Piece p> inline void generate();

    [[nodiscard]] inline int size() const { return last; }
    void print();
    void print_legal(position& p);
    inline void reset() { last = 0; }
};

// ─── shift templates ────────────────────────────────────────────────────────

namespace detail {
template <Dir> inline void shift(U64& b);
template <> inline void shift<N>(U64& b) {
    b <<= 8;
}
template <> inline void shift<S>(U64& b) {
    b >>= 8;
}
template <> inline void shift<NN>(U64& b) {
    b <<= 16;
}
template <> inline void shift<SS>(U64& b) {
    b >>= 16;
}
template <> inline void shift<NW>(U64& b) {
    b <<= 7;
}
template <> inline void shift<NE>(U64& b) {
    b <<= 9;
}
template <> inline void shift<SW>(U64& b) {
    b >>= 7;
}
template <> inline void shift<SE>(U64& b) {
    b >>= 9;
}
} // namespace detail

// ─── encode helpers ─────────────────────────────────────────────────────────

template <Movetype mt> inline void Movegen::encode(U64& b, const int& f) {
    while (b)
        list[last++].set(f, bits::pop_lsb(b), mt);
}

template <Movetype mt> inline void Movegen::encode_pawn_pushes(U64& b, const int& dir) {
    while (b) {
        int to = bits::pop_lsb(b);
        list[last++].set(to + dir, to, mt);
    }
}

inline void Movegen::encode_promotions(U64& b, const int& dir) {
    while (b) {
        Square to = Square(bits::pop_lsb(b));
        Square f = Square(to + dir);
        list[last++].set(f, to, promotion_q);
        list[last++].set(f, to, promotion_r);
        list[last++].set(f, to, promotion_b);
        list[last++].set(f, to, promotion_n);
    }
}

inline void Movegen::encode_capture_promotions(U64& b, const int& dir) {
    while (b) {
        Square to = Square(bits::pop_lsb(b));
        Square f = Square(to + dir);
        list[last++].set(f, to, capture_promotion_q);
        list[last++].set(f, to, capture_promotion_r);
        list[last++].set(f, to, capture_promotion_b);
        list[last++].set(f, to, capture_promotion_n);
    }
}

// ─── initialize ─────────────────────────────────────────────────────────────

inline void Movegen::initialize(const position& p) {
    us = p.to_move();
    them = Color(us ^ 1);
    all_pieces = p.all_pieces();
    empty = ~all_pieces;

    // Castling rights alone are not enough to generate a castle move: the
    // squares between king and rook must be empty and the rook must actually
    // be there. Both were previously left to is_legal(), so every node with
    // rights still on generated one or two castle moves that were scored,
    // sorted and then thrown away -- in the opening that is almost all of
    // them. Testing here is two bitboard ANDs and prunes them at the source.
    // is_legal() still owns the check and transit-square-attacked rules, so
    // the surviving move list is unchanged.
    constexpr U64 wks_between = (1ULL << F1) | (1ULL << G1);
    constexpr U64 wqs_between = (1ULL << B1) | (1ULL << C1) | (1ULL << D1);
    constexpr U64 bks_between = (1ULL << F8) | (1ULL << G8);
    constexpr U64 bqs_between = (1ULL << B8) | (1ULL << C8) | (1ULL << D8);
    if (us == white) {
        const U64 wr = p.get_pieces<white, rook>();
        can_castle_ks_ = p.can_castle_ks() && (all_pieces & wks_between) == 0ULL &&
                         (wr & (1ULL << H1)) != 0ULL;
        can_castle_qs_ = p.can_castle_qs() && (all_pieces & wqs_between) == 0ULL &&
                         (wr & (1ULL << A1)) != 0ULL;
    } else {
        const U64 br = p.get_pieces<black, rook>();
        can_castle_ks_ = p.can_castle_ks() && (all_pieces & bks_between) == 0ULL &&
                         (br & (1ULL << H8)) != 0ULL;
        can_castle_qs_ = p.can_castle_qs() && (all_pieces & bqs_between) == 0ULL &&
                         (br & (1ULL << A8)) != 0ULL;
    }

    check_target = p.checkers();
    evasion_target = 0ULL;

    if (check_target != 0ULL && !bits::more_than_one(check_target)) {
        U64 tmp = check_target;
        Square frm = Square(bits::pop_lsb(tmp));
        Piece checker = p.piece_on(frm);
        if (checker == bishop || checker == rook || checker == queen) {
            evasion_target = bitboards::between[frm][p.king_square()] & empty;
        }
    }

    if (us == white) {
        rank2 = bitboards::row[r2];
        rank7 = bitboards::row[r7];
        pawns = p.get_pieces<white, pawn>();
        knights = p.squares_of<white, knight>();
        bishops = p.squares_of<white, bishop>();
        rooks = p.squares_of<white, rook>();
        queens = p.squares_of<white, queen>();
        kings = p.squares_of<white, king>();
        enemies = p.get_pieces<black>();
    } else {
        rank2 = bitboards::row[r7];
        rank7 = bitboards::row[r2];
        pawns = p.get_pieces<black, pawn>();
        knights = p.squares_of<black, knight>();
        bishops = p.squares_of<black, bishop>();
        rooks = p.squares_of<black, rook>();
        queens = p.squares_of<black, queen>();
        kings = p.squares_of<black, king>();
        enemies = p.get_pieces<white>();
    }

    qtarget = (evasion_target != 0ULL ? evasion_target : empty);
    ctarget = (check_target == 0ULL ? enemies : check_target);

    eps = p.eps();
    pawns2 = pawns & rank2;
    pawns7 = pawns & rank7;
}

// ─── pawn utilities ─────────────────────────────────────────────────────────

inline void Movegen::pawn_quiets(U64& single, U64& dbl) {
    if (pawns == 0ULL)
        return;

    single = pawns & bitboards::pawnmask[us];
    dbl = pawns & rank2;

    auto s = (us == white ? detail::shift<N> : detail::shift<S>);

    s(single);
    single &= qtarget;

    for (int i = 0; i < 2; ++i) {
        s(dbl);
        dbl &= empty;
    }
    if (evasion_target != 0ULL)
        dbl &= evasion_target;
}

inline void Movegen::pawn_caps(U64& left, U64& right, U64& ep_left, U64& ep_right) {
    if (pawns == 0ULL)
        return;

    left = pawns & bitboards::pawnmaskleft[us];
    right = pawns & bitboards::pawnmaskright[us];

    auto sw = (us == white ? detail::shift<NW> : detail::shift<SE>);
    auto se = (us == white ? detail::shift<NE> : detail::shift<SW>);

    if (left != 0ULL) {
        se(left);
        left &= ctarget;
    }

    if (right != 0ULL) {
        sw(right);
        right &= ctarget;
    }

    if (eps == no_square)
        return;
    auto r = (us == white ? r5 : r4);
    ep_left = pawns & bitboards::pawnmaskleft[us] & bitboards::row[r];
    ep_right = pawns & bitboards::pawnmaskright[us] & bitboards::row[r];

    if (ep_left != 0ULL) {
        se(ep_left);
        ep_left &= bitboards::squares[eps];
    }

    if (ep_right != 0ULL) {
        sw(ep_right);
        ep_right &= bitboards::squares[eps];
    }
}

inline void Movegen::quiet_promotions(U64& quiets) {
    if (pawns7 == 0ULL)
        return;
    quiets = pawns7;
    auto s = (us == white ? detail::shift<N> : detail::shift<S>);
    s(quiets);
    quiets &= qtarget;
}

inline void Movegen::capture_promotions(U64& right_caps, U64& left_caps) {
    if (pawns7 == 0ULL)
        return;

    right_caps = pawns7 & bitboards::pawnmaskright[us ^ 1];
    left_caps = pawns7 & bitboards::pawnmaskleft[us ^ 1];

    auto sw = (us == white ? detail::shift<NW> : detail::shift<SE>);
    auto se = (us == white ? detail::shift<NE> : detail::shift<SW>);

    if (left_caps) {
        se(left_caps);
        left_caps &= ctarget;
    }

    if (right_caps) {
        sw(right_caps);
        right_caps &= ctarget;
    }
}

// ─── pawn moves ─────────────────────────────────────────────────────────────

template <> inline void Movegen::generate<quiet, pawn>() {
    U64 single_pushes = 0ULL;
    U64 double_pushes = 0ULL;
    int d1 = (us == white ? -8 : 8);
    int d2 = 2 * d1;

    pawn_quiets(single_pushes, double_pushes);

    if (single_pushes != 0ULL)
        encode_pawn_pushes<quiet>(single_pushes, d1);
    if (double_pushes != 0ULL)
        encode_pawn_pushes<quiet>(double_pushes, d2);
}

template <> inline void Movegen::generate<capture, pawn>() {
    U64 left = 0ULL;
    U64 right = 0ULL;
    U64 ep_left = 0ULL;
    U64 ep_right = 0ULL;

    int d1 = (us == white ? -7 : 9);
    int d2 = (us == white ? -9 : 7);

    pawn_caps(left, right, ep_left, ep_right);

    if (left != 0ULL)
        encode_pawn_pushes<capture>(left, d2);
    if (right != 0ULL)
        encode_pawn_pushes<capture>(right, d1);

    if (ep_left != 0ULL)
        encode_pawn_pushes<ep>(ep_left, d2);
    if (ep_right != 0ULL)
        encode_pawn_pushes<ep>(ep_right, d1);
}

template <> inline void Movegen::generate<promotion, pawn>() {
    U64 mvs = 0;
    quiet_promotions(mvs);
    if (mvs)
        encode_promotions(mvs, us == white ? -8 : 8);
}

template <> inline void Movegen::generate<capture_promotion, pawn>() {
    U64 caps_l = 0;
    U64 caps_r = 0;
    int d1 = (us == white ? -7 : 9);
    int d2 = (us == white ? -9 : 7);
    capture_promotions(caps_r, caps_l);
    if (caps_r != 0ULL)
        encode_capture_promotions(caps_r, d1);
    if (caps_l != 0ULL)
        encode_capture_promotions(caps_l, d2);
}

// ─── knight moves ───────────────────────────────────────────────────────────

template <> inline void Movegen::generate<quiet, knight>() {
    for (Square* s = &knights[0]; *s != no_square; ++s) {
        U64 mvs = bitboards::nmask[*s] & qtarget;
        if (mvs != 0ULL)
            encode<quiet>(mvs, *s);
    }
}

template <> inline void Movegen::generate<capture, knight>() {
    for (Square* s = &knights[0]; *s != no_square; ++s) {
        U64 mvs = bitboards::nmask[*s] & ctarget;
        if (mvs != 0ULL)
            encode<capture>(mvs, *s);
    }
}

template <> inline void Movegen::generate<pseudo_legal, knight>() {
    for (Square* s = &knights[0]; *s != no_square; ++s) {
        U64 qmvs = bitboards::nmask[*s] & qtarget;
        if (qmvs != 0ULL)
            encode<quiet>(qmvs, *s);
        U64 cmvs = bitboards::nmask[*s] & ctarget;
        if (cmvs != 0ULL)
            encode<capture>(cmvs, *s);
    }
}

// ─── bishop moves ───────────────────────────────────────────────────────────

template <> inline void Movegen::generate<quiet, bishop>() {
    for (Square* s = &bishops[0]; *s != no_square; ++s) {
        U64 mvs = magics::attacks<bishop>(all_pieces, *s) & qtarget;
        if (mvs != 0ULL)
            encode<quiet>(mvs, *s);
    }
}

template <> inline void Movegen::generate<capture, bishop>() {
    for (Square* s = &bishops[0]; *s != no_square; ++s) {
        U64 mvs = magics::attacks<bishop>(all_pieces, *s) & ctarget;
        if (mvs != 0ULL)
            encode<capture>(mvs, *s);
    }
}

template <> inline void Movegen::generate<pseudo_legal, bishop>() {
    for (Square* s = &bishops[0]; *s != no_square; ++s) {
        U64 mvs = magics::attacks<bishop>(all_pieces, *s);
        U64 q = mvs & qtarget;
        if (q != 0ULL)
            encode<quiet>(q, *s);
        U64 c = mvs & ctarget;
        if (c != 0ULL)
            encode<capture>(c, *s);
    }
}

// ─── rook moves ─────────────────────────────────────────────────────────────

template <> inline void Movegen::generate<quiet, rook>() {
    for (Square* s = &rooks[0]; *s != no_square; ++s) {
        U64 mvs = magics::attacks<rook>(all_pieces, *s) & qtarget;
        if (mvs != 0ULL)
            encode<quiet>(mvs, *s);
    }
}

template <> inline void Movegen::generate<capture, rook>() {
    for (Square* s = &rooks[0]; *s != no_square; ++s) {
        U64 mvs = magics::attacks<rook>(all_pieces, *s) & ctarget;
        if (mvs != 0ULL)
            encode<capture>(mvs, *s);
    }
}

template <> inline void Movegen::generate<pseudo_legal, rook>() {
    for (Square* s = &rooks[0]; *s != no_square; ++s) {
        U64 mvs = magics::attacks<rook>(all_pieces, *s);
        U64 q = mvs & qtarget;
        if (q != 0ULL)
            encode<quiet>(q, *s);
        U64 c = mvs & ctarget;
        if (c != 0ULL)
            encode<capture>(c, *s);
    }
}

// ─── queen moves ────────────────────────────────────────────────────────────

template <> inline void Movegen::generate<quiet, queen>() {
    for (Square* s = &queens[0]; *s != no_square; ++s) {
        U64 mvs =
            (magics::attacks<bishop>(all_pieces, *s) | magics::attacks<rook>(all_pieces, *s)) &
            qtarget;
        if (mvs != 0ULL)
            encode<quiet>(mvs, *s);
    }
}

template <> inline void Movegen::generate<capture, queen>() {
    for (Square* s = &queens[0]; *s != no_square; ++s) {
        U64 mvs =
            (magics::attacks<bishop>(all_pieces, *s) | magics::attacks<rook>(all_pieces, *s)) &
            ctarget;
        if (mvs != 0ULL)
            encode<capture>(mvs, *s);
    }
}

template <> inline void Movegen::generate<pseudo_legal, queen>() {
    for (Square* s = &queens[0]; *s != no_square; ++s) {
        U64 mvs = magics::attacks<bishop>(all_pieces, *s) | magics::attacks<rook>(all_pieces, *s);
        U64 q = mvs & qtarget;
        if (q != 0ULL)
            encode<quiet>(q, *s);
        U64 c = mvs & ctarget;
        if (c != 0ULL)
            encode<capture>(c, *s);
    }
}

// ─── king moves ─────────────────────────────────────────────────────────────

template <> inline void Movegen::generate<quiet, king>() {
    U64 mvs = bitboards::kmask[kings[0]] & empty;
    if (mvs != 0ULL)
        encode<quiet>(mvs, kings[0]);
}

template <> inline void Movegen::generate<castles, king>() {
    if (can_castle_ks_)
        list[last++].set(kings[0], (us == white ? G1 : G8), castle_ks);
    if (can_castle_qs_)
        list[last++].set(kings[0], (us == white ? C1 : C8), castle_qs);
}

template <> inline void Movegen::generate<capture, king>() {
    U64 mvs = bitboards::kmask[kings[0]] & enemies;
    if (mvs != 0ULL)
        encode<capture>(mvs, kings[0]);
}

// ─── by piece type ──────────────────────────────────────────────────────────

template <> inline void Movegen::generate<king>() {
    generate<capture, king>();
    generate<quiet, king>();
    generate<castles, king>();
}

template <> inline void Movegen::generate<queen>() {
    generate<pseudo_legal, queen>();
}
template <> inline void Movegen::generate<rook>() {
    generate<pseudo_legal, rook>();
}
template <> inline void Movegen::generate<bishop>() {
    generate<pseudo_legal, bishop>();
}
template <> inline void Movegen::generate<knight>() {
    generate<pseudo_legal, knight>();
}

template <> inline void Movegen::generate<pawn>() {
    generate<capture_promotion, pawn>();
    generate<promotion, pawn>();
    generate<capture, pawn>();
    generate<quiet, pawn>();
}

// ─── pseudo-legal quiet ─────────────────────────────────────────────────────

template <> inline void Movegen::generate<quiet, pieces>() {
    if (check_target == 0ULL) {
        generate<promotion, pawn>();
        generate<quiet, knight>();
        generate<quiet, bishop>();
        generate<quiet, rook>();
        generate<quiet, queen>();
        generate<quiet, pawn>();
        generate<quiet, king>();
        generate<castles, king>();
    } else if (check_target != 0ULL && evasion_target == 0ULL) {
        generate<quiet, king>();
    } else if (check_target != 0ULL && evasion_target != 0ULL) {
        if (!bits::more_than_one(check_target)) {
            generate<quiet, knight>();
            generate<quiet, bishop>();
            generate<quiet, rook>();
            generate<quiet, queen>();
            generate<promotion, pawn>();
            generate<quiet, pawn>();
        }
        generate<quiet, king>();
    }
}

// ─── pseudo-legal capture ───────────────────────────────────────────────────

template <> inline void Movegen::generate<capture, pieces>() {
    if (check_target == 0ULL) {
        generate<capture_promotion, pawn>();
        generate<capture, pawn>();
        generate<capture, knight>();
        generate<capture, bishop>();
        generate<capture, rook>();
        generate<capture, queen>();
        generate<capture, king>();
    } else if (check_target != 0ULL && evasion_target == 0ULL) {
        if (!bits::more_than_one(check_target)) {
            generate<capture_promotion, pawn>();
            generate<capture, king>();
            generate<capture, pawn>();
            generate<capture, knight>();
            generate<capture, bishop>();
            generate<capture, rook>();
            generate<capture, queen>();
        } else {
            generate<capture, king>();
        }
    } else if (check_target != 0ULL && evasion_target != 0ULL) {
        if (!bits::more_than_one(check_target)) {
            generate<capture_promotion, pawn>();
            generate<capture, pawn>();
            generate<capture, king>();
            generate<capture, knight>();
            generate<capture, bishop>();
            generate<capture, rook>();
            generate<capture, queen>();
        } else {
            generate<capture, king>();
        }
    }
}

// ─── pseudo-legal all ───────────────────────────────────────────────────────

template <> inline void Movegen::generate<pseudo_legal, pieces>() {
    if (check_target == 0ULL) {
        generate<capture_promotion, pawn>();
        generate<promotion, pawn>();
        generate<pseudo_legal, knight>();
        generate<pseudo_legal, bishop>();
        generate<pseudo_legal, rook>();
        generate<pseudo_legal, queen>();
        generate<capture, pawn>();
        generate<quiet, pawn>();
        generate<quiet, king>();
        generate<castles, king>();
        generate<capture, king>();
    } else if (check_target != 0ULL && evasion_target == 0ULL) {
        if (!bits::more_than_one(check_target)) {
            generate<capture_promotion, pawn>();
            generate<capture, king>();
            generate<capture, pawn>();
            generate<capture, knight>();
            generate<capture, bishop>();
            generate<capture, rook>();
            generate<capture, queen>();
            generate<quiet, king>();
        } else {
            generate<capture, king>();
            generate<quiet, king>();
        }
    } else if (check_target != 0ULL && evasion_target != 0ULL) {
        if (!bits::more_than_one(check_target)) {
            generate<capture_promotion, pawn>();
            generate<capture, king>();
            generate<capture, pawn>();
            generate<pseudo_legal, knight>();
            generate<pseudo_legal, bishop>();
            generate<pseudo_legal, rook>();
            generate<pseudo_legal, queen>();
            generate<promotion, pawn>();
            generate<quiet, king>();
            generate<quiet, pawn>();
        } else {
            generate<capture, king>();
            generate<quiet, king>();
        }
    }
}

} // namespace havoc
