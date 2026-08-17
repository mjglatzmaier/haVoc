#pragma once

#include "havoc/bitboard.hpp"
#include "havoc/feature_delta.hpp"
#include "havoc/magics.hpp"
#include "havoc/single_writer_counter.hpp"
#include "havoc/types.hpp"
#include "havoc/utils.hpp"
#include "havoc/zobrist.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

namespace havoc {

// ─── Position state ─────────────────────────────────────────────────────────

struct info {
    U64 checkers = 0;
    U64 pinned[2] = {};
    U64 key = 0;
    U64 mkey = 0;
    U64 pawnkey = 0;
    U64 repkey = 0;
    U16 hmvs = 0;
    U16 cmask = 0;
    U8 move50 = 0;
    Color stm = white;
    Square eps = no_square;
    Square ks[2] = {no_square, no_square};
    Piece captured = no_piece;
    bool incheck = false;
};

// ─── Rootmove ───────────────────────────────────────────────────────────────

struct Rootmove {
    explicit Rootmove(const Move& m) : pv(1, m) {}
    std::vector<Move> pv;
    int selDepth = 0;
    int16_t score = score::kNegInf;
    int16_t prevScore = score::kNegInf;
    bool operator==(const Move& m) const { return pv[0] == m; }
    bool operator<(const Rootmove& m) const {
        return m.score != score ? m.score < score : m.prevScore < prevScore;
    }
};
using Rootmoves = std::vector<Rootmove>;

// ─── Piece data ─────────────────────────────────────────────────────────────

struct piece_data {
    std::array<U64, 2> bycolor{};
    std::array<Square, 2> king_sq{no_square, no_square};
    std::array<Color, squares> color_on;
    std::array<Piece, squares> piece_on;
    std::array<std::array<int, pieces>, 2> number_of{};
    std::array<std::array<U64, no_piece + 1>, colors> bitmap{};
    std::array<std::array<std::array<std::uint8_t, squares>, pieces>, 2> piece_idx{};
    std::array<std::array<std::array<Square, 11>, pieces>, 2> square_of;

    /// Change log for the most recent mutation. Not board state: see
    /// `feature_delta.hpp`. Written by the three primitives below, cleared by
    /// `position::do_move`/`undo_move`, read by an incremental evaluator.
    FeatureDelta delta;

    piece_data() {
        color_on.fill(no_color);
        piece_on.fill(no_piece);
        for (auto& v : square_of)
            for (auto& w : v)
                w.fill(no_square);
    }
    piece_data(const piece_data& pd) = default;
    piece_data& operator=(const piece_data& pd) = default;

    void clear();
    void set(const Color& c, const Piece& p, const Square& s, info& ifo);

    inline void do_quiet(const Color& c, const Piece& p, const Square& f, const Square& t,
                         info& ifo);
    template <Color c> inline void do_castle(bool kingside);
    template <Color c> inline void undo_castle(bool kingside);
    inline void do_cap(const Color& c, const Piece& p, const Square& f, const Square& t, info& ifo);
    inline void do_ep(const Color& c, const Square& f, const Square& t, info& ifo);
    inline void do_promotion(const Color& c, const Piece& p, const Square& f, const Square& t,
                             info& ifo);
    inline void do_promotion_cap(const Color& c, const Piece& p, const Square& f, const Square& t,
                                 info& ifo);
    inline void do_castle_ks(const Color& c, const Square& f, const Square& t, info& ifo);
    inline void do_castle_qs(const Color& c, const Square& f, const Square& t, info& ifo);
    inline void remove_piece(const Color& c, const Piece& p, const Square& s, info& ifo);
    inline void add_piece(const Color& c, const Piece& p, const Square& s, info& ifo);
};

// ─── Position class ─────────────────────────────────────────────────────────

class position {
    std::vector<info> history_;
    info ifo{};
    piece_data pcs;
    // Incremented only by the thread that owns this position, but summed by
    // every thread for the node limit and by the UCI loop for live nps.
    single_writer_counter nodes_searched;
    single_writer_counter qnodes_searched;
    /// Positions this thread resolved from a tablebase rather than by
    /// searching. Reported as UCI `tbhits`, which is the only way an operator
    /// can tell that SyzygyPath actually took effect.
    single_writer_counter tb_hits_;

  public:
    position() { history_.reserve(1024); }
    explicit position(std::istringstream& s);
    position(const position& p);
    position(position&&) = default;
    position& operator=(const position& p);
    position& operator=(position&&) = default;
    ~position() = default;

    Rootmoves root_moves;

    /// Deepest ply reached on the current iteration, quiescence included.
    ///
    /// This lives here because the engine gives each search thread its own
    /// `position`, so a field on it is per-thread by construction and costs
    /// nothing to reach: the search already holds this object in a register.
    /// It was previously a single `std::atomic<int>` on the engine shared by
    /// every thread, which made the reported `seldepth` describe no thread in
    /// particular once `Threads > 1` -- each thread reset it at the top of
    /// every aspiration window and each thread incremented it.
    ///
    /// Like `root_moves` above, this is per-search state rather than board
    /// state. Both belong in a per-thread search-state type; that refactor is
    /// worth doing when per-thread history tables arrive, not before.
    int sel_depth = 0;

    // setup / clear
    /// Returns false if the FEN does not describe a legal position, in which
    /// case the board is left cleared rather than half-parsed.
    bool setup(std::istringstream& fen);
    [[nodiscard]] std::string to_fen() const;
    void clear();
    void set_piece(char p, const Square& s);
    void print() const;
    void do_move(const Move& m);
    void undo_move(const Move& m);
    void do_null_move();
    void undo_null_move();
    [[nodiscard]] int see_move(const Move& m) const;
    [[nodiscard]] int see(const Move& m) const;

    /// Piece values used by static exchange evaluation.
    ///
    /// SEE decides capture ordering and prunes losing captures, so it is
    /// making claims about material that the evaluation also makes. These were
    /// a hard-coded table in position.cpp that happened to hold the same
    /// numbers as parameters::material_value; nothing linked them, so the first
    /// tuning run to move a piece value would have left SEE ordering and
    /// pruning captures by the old one. parameters::load() and the tuner both
    /// push the current values through here.
    static void set_see_values(const std::array<int, 5>& pawn_through_queen);
    [[nodiscard]] static std::array<int, 6> see_values();

    [[nodiscard]] bool is_attacked(const Square& s, const Color& us, const Color& them,
                                   U64 m = 0ULL) const;
    [[nodiscard]] U64 attackers_of2(const Square& s, const Color& c) const;
    [[nodiscard]] U64 attackers_of(const Square& s, const U64& bb) const;
    [[nodiscard]] U64 checkers() const { return ifo.checkers; }
    [[nodiscard]] bool in_check() const;
    [[nodiscard]] bool in_dangerous_check();
    [[nodiscard]] bool gives_check(const Move& m);
    [[nodiscard]] bool quiet_gives_dangerous_check(const Move& m);
    [[nodiscard]] bool is_legal(const Move& m);
    [[nodiscard]] U64 pinned(Color us);
    [[nodiscard]] bool is_draw();

    /// True when neither side can possibly deliver mate, so the position is
    /// dead drawn no matter how well either side plays.
    [[nodiscard]] bool is_material_draw() const;

    [[nodiscard]] inline bool can_castle_ks() const {
        return (ifo.cmask & (ifo.stm == white ? wks : bks)) == (ifo.stm == white ? wks : bks);
    }
    [[nodiscard]] inline bool can_castle_qs() const {
        return (ifo.cmask & (ifo.stm == white ? wqs : bqs)) == (ifo.stm == white ? wqs : bqs);
    }

    template <Color c> [[nodiscard]] inline bool can_castle_ks() const {
        return (ifo.cmask & (c == white ? wks : bks)) == (c == white ? wks : bks);
    }
    template <Color c> [[nodiscard]] inline bool can_castle_qs() const {
        return (ifo.cmask & (c == white ? wqs : bqs)) == (c == white ? wqs : bqs);
    }
    template <Color c> [[nodiscard]] inline bool can_castle() const {
        return can_castle_ks<c>() || can_castle_qs<c>();
    }
    template <Color c> [[nodiscard]] inline U64 pinned() const { return ifo.pinned[c]; }

    [[nodiscard]] inline bool pawns_near_promotion() const {
        return (get_pieces<white, pawn>() & bitboards::row[r7]) != 0ULL ||
               (get_pieces<black, pawn>() & bitboards::row[r2]) != 0ULL;
    }

    [[nodiscard]] inline bool pawns_on_7th() const {
        return ifo.stm == white ? (get_pieces<white, pawn>() & bitboards::row[r7]) != 0ULL
                                : (get_pieces<black, pawn>() & bitboards::row[r2]) != 0ULL;
    }

    template <Color c> [[nodiscard]] inline bool non_pawn_material() const {
        return (get_pieces<c, knight>() | get_pieces<c, bishop>() | get_pieces<c, rook>() |
                get_pieces<c, queen>()) != 0ULL;
    }

    [[nodiscard]] inline Square eps() const { return ifo.eps; }
    [[nodiscard]] inline Color to_move() const { return ifo.stm; }

    /// Halfmoves since the last capture or pawn move. Tablebase probes need
    /// this to be zero, since the tables encode no progress toward the
    /// fifty-move rule.
    [[nodiscard]] inline int rule50() const { return static_cast<int>(ifo.move50); }

    /// The raw castling-rights mask. Tablebase probes need this to be zero for
    /// the same reason.
    [[nodiscard]] inline U16 castle_mask() const { return ifo.cmask; }
    [[nodiscard]] inline U64 key() const { return ifo.key; }
    [[nodiscard]] inline U64 repkey() const { return ifo.repkey; }
    [[nodiscard]] inline U64 pawnkey() const { return ifo.pawnkey; }
    [[nodiscard]] inline U64 material_key() const { return ifo.mkey; }

    [[nodiscard]] inline U64 all_pieces() const { return pcs.bycolor[white] | pcs.bycolor[black]; }
    [[nodiscard]] inline unsigned number_of(const Color& c, const Piece& p) const {
        return pcs.number_of[c][p];
    }
    [[nodiscard]] inline Piece piece_on(const Square& s) const { return pcs.piece_on[s]; }
    [[nodiscard]] inline Square king_square(const Color& c) const { return ifo.ks[c]; }
    [[nodiscard]] inline Square king_square() const { return ifo.ks[ifo.stm]; }
    [[nodiscard]] inline Color color_on(const Square& s) const { return pcs.color_on[s]; }

    /// The features that changed on the last `do_move`. Valid only until the
    /// next board mutation; see `feature_delta.hpp`.
    [[nodiscard]] inline const FeatureDelta& delta() const { return pcs.delta; }

    [[nodiscard]] bool is_cap_promotion(const Movetype& mt);
    [[nodiscard]] bool is_promotion(const U8& mt);

    [[nodiscard]] inline U64 nodes() const { return nodes_searched.get(); }
    [[nodiscard]] inline U64 qnodes() const { return qnodes_searched.get(); }
    inline void set_nodes_searched(U64 n) { nodes_searched.set(n); }
    inline void set_qnodes_searched(U64 qn) { qnodes_searched.set(qn); }
    inline void adjust_nodes(const U64& dn) { nodes_searched.add(dn); }
    inline void adjust_qnodes(const U64& dn) { qnodes_searched.add(dn); }
    [[nodiscard]] inline U64 tb_hits() const { return tb_hits_.get(); }
    inline void set_tb_hits(U64 n) { tb_hits_.set(n); }
    inline void adjust_tb_hits(const U64& dn) { tb_hits_.add(dn); }

    template <Color c, Piece p> [[nodiscard]] inline U64 get_pieces() const {
        return pcs.bitmap[c][p];
    }
    template <Color c> [[nodiscard]] inline U64 get_pieces() const { return pcs.bycolor[c]; }

    template <Color c, Piece p> [[nodiscard]] inline Square* squares_of() const {
        return const_cast<Square*>(pcs.square_of[c][p].data() + 1);
    }
};

// ─── piece_data inline implementations ──────────────────────────────────────

inline void piece_data::clear() {
    std::fill(bycolor.begin(), bycolor.end(), 0);
    std::fill(king_sq.begin(), king_sq.end(), no_square);
    std::fill(color_on.begin(), color_on.end(), no_color);
    std::fill(piece_on.begin(), piece_on.end(), no_piece);

    for (auto& v : number_of)
        std::fill(v.begin(), v.end(), 0);
    for (auto& v : bitmap)
        std::fill(v.begin(), v.end(), 0ULL);
    for (auto& v : piece_idx)
        for (auto& w : v)
            std::fill(w.begin(), w.end(), 0);
    for (auto& v : square_of)
        for (auto& w : v)
            std::fill(w.begin(), w.end(), no_square);

    delta.clear();
}

inline void piece_data::do_quiet(const Color& c, const Piece& p, const Square& f, const Square& t,
                                 info& ifo) {
    U64 fto = bitboards::squares[f] | bitboards::squares[t];

    int idx = piece_idx[c][p][f];
    piece_idx[c][p][f] = 0;
    piece_idx[c][p][t] = static_cast<std::uint8_t>(idx);

    bycolor[c] ^= fto;
    bitmap[c][p] ^= fto;

    square_of[c][p][idx] = t;
    color_on[f] = no_color;
    color_on[t] = c;
    piece_on[t] = p;
    piece_on[f] = no_piece;

    ifo.key ^= zobrist::piece(f, c, p);
    ifo.key ^= zobrist::piece(t, c, p);
    ifo.repkey ^= zobrist::piece(f, c, p);
    ifo.repkey ^= zobrist::piece(t, c, p);

    if (p == pawn) {
        ifo.pawnkey ^= zobrist::piece(f, c, p);
        ifo.pawnkey ^= zobrist::piece(t, c, p);
    }

    delta.remove(c, p, f);
    delta.add(c, p, t);
}

inline void piece_data::do_cap(const Color& c, const Piece& p, const Square& f, const Square& t,
                               info& ifo) {
    Color them = Color(c ^ 1);
    Piece cap = piece_on[t];
    remove_piece(them, cap, t, ifo);
    do_quiet(c, p, f, t, ifo);
}

inline void piece_data::do_promotion(const Color& c, const Piece& p, const Square& f,
                                     const Square& t, info& ifo) {
    remove_piece(c, pawn, f, ifo);
    add_piece(c, p, t, ifo);
}

inline void piece_data::do_ep(const Color& c, const Square& f, const Square& t, info& ifo) {
    Color them = Color(c ^ 1);
    Square cs = Square(them == white ? t + 8 : t - 8);
    remove_piece(them, pawn, cs, ifo);
    do_quiet(c, pawn, f, t, ifo);
}

inline void piece_data::do_promotion_cap(const Color& c, const Piece& p, const Square& f,
                                         const Square& t, info& ifo) {
    Color them = Color(c ^ 1);
    Piece cap = piece_on[t];
    remove_piece(them, cap, t, ifo);
    remove_piece(c, pawn, f, ifo);
    add_piece(c, p, t, ifo);
}

inline void piece_data::do_castle_ks(const Color& c, const Square& f, const Square& t, info& ifo) {
    Square rf = (c == white ? H1 : H8);
    Square rt = (c == white ? F1 : F8);
    do_quiet(c, king, f, t, ifo);
    do_quiet(c, rook, rf, rt, ifo);
}

inline void piece_data::do_castle_qs(const Color& c, const Square& f, const Square& t, info& ifo) {
    Square rf = (c == white ? A1 : A8);
    Square rt = (c == white ? D1 : D8);
    do_quiet(c, king, f, t, ifo);
    do_quiet(c, rook, rf, rt, ifo);
}

/// Material key contribution for owning `count` pieces of type `p` and color
/// `c`. The material key must be a function of the piece *counts* and nothing
/// else, so it deliberately does not depend on where any piece stands.
///
/// The zobrist piece table is reused with the count standing in for a square,
/// which is safe because a side can hold at most ten pieces of one type and the
/// material key shares no bits with the position key.
inline U64 material_hash(const Color& c, const Piece& p, int count) {
    return zobrist::piece(Square(count), c, p);
}

inline void piece_data::remove_piece(const Color& c, const Piece& p, const Square& s, info& ifo) {
    U64 sq = bitboards::squares[s];
    bycolor[c] ^= sq;
    bitmap[c][p] ^= sq;

    int tmp_idx = piece_idx[c][p][s];
    int max_idx = number_of[c][p];
    Square tmp_sq = square_of[c][p][max_idx];
    square_of[c][p][tmp_idx] = square_of[c][p][max_idx];
    square_of[c][p][max_idx] = no_square;
    piece_idx[c][p][tmp_sq] = static_cast<std::uint8_t>(tmp_idx);
    number_of[c][p] -= 1;
    piece_idx[c][p][s] = 0;
    color_on[s] = no_color;
    piece_on[s] = no_piece;
    ifo.key ^= zobrist::piece(s, c, p);
    // number_of has already been decremented, so the count being removed is
    // one more than it now reads.
    ifo.mkey ^= material_hash(c, p, number_of[c][p] + 1);
    ifo.repkey ^= zobrist::piece(s, c, p);
    if (p == pawn)
        ifo.pawnkey ^= zobrist::piece(s, c, p);

    delta.remove(c, p, s);
}

inline void piece_data::add_piece(const Color& c, const Piece& p, const Square& s, info& ifo) {
    U64 sq = bitboards::squares[s];
    bycolor[c] |= sq;
    bitmap[c][p] |= sq;

    number_of[c][p] += 1;
    square_of[c][p][number_of[c][p]] = s;
    piece_on[s] = p;
    piece_idx[c][p][s] = static_cast<std::uint8_t>(number_of[c][p]);
    color_on[s] = c;
    ifo.key ^= zobrist::piece(s, c, p);
    ifo.mkey ^= material_hash(c, p, number_of[c][p]);
    ifo.repkey ^= zobrist::piece(s, c, p);
    if (p == pawn)
        ifo.pawnkey ^= zobrist::piece(s, c, p);

    delta.add(c, p, s);
}

inline void piece_data::set(const Color& c, const Piece& p, const Square& s, info& ifo) {
    bitmap[c][p] |= bitboards::squares[s];
    bycolor[c] |= bitboards::squares[s];
    color_on[s] = c;
    number_of[c][p] += 1;
    piece_idx[c][p][s] = static_cast<std::uint8_t>(number_of[c][p]);
    square_of[c][p][number_of[c][p]] = s;
    piece_on[s] = p;
    if (p == king)
        king_sq[c] = s;

    ifo.key ^= zobrist::piece(s, c, p);
    ifo.mkey ^= material_hash(c, p, number_of[c][p]);
    ifo.repkey ^= zobrist::piece(s, c, p);
    if (p == pawn)
        ifo.pawnkey ^= zobrist::piece(s, c, p);
}

} // namespace havoc
