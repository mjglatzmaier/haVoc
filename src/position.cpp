#include "havoc/position.hpp"

#include <algorithm>
#include <cmath>
#include <vector>

namespace havoc {

// ─── Constructors / assignment ──────────────────────────────────────────────

position::position(std::istringstream& fen) {
    history_.reserve(1024);
    setup(fen);
}

position::position(const position& p) {
    history_.reserve(1024);
    *this = p;
}

position& position::operator=(const position& p) {
    if (this == &p)
        return *this;
    history_ = p.history_;
    root_moves = p.root_moves;
    ifo = p.ifo;
    pcs = p.pcs;
    nodes_searched = p.nodes_searched;
    qnodes_searched = p.qnodes_searched;
    return *this;
}

// ─── Setup / clear ──────────────────────────────────────────────────────────

void position::setup(std::istringstream& fen) {
    clear();

    std::string token;
    fen >> token;
    Square s = A8;

    for (auto& c : token) {
        if (isdigit(c))
            s += int(c - '0');
        else if (c == '/')
            s -= 16;
        else {
            set_piece(c, s);
            ++s;
        }
    }

    // side to move
    fen >> token;
    ifo.stm = (token == "w" ? white : black);
    ifo.key ^= zobrist::stm(ifo.stm);
    ifo.repkey ^= zobrist::stm(ifo.stm);

    // castle rights
    fen >> token;
    ifo.cmask = U16(0);
    for (auto& c : token)
        ifo.cmask |= castle_right_from_char(c);
    // Hashed as a whole mask. Hashing each right under zobrist::castle(stm, cr)
    // made the term depend on whose turn it was, so the same rights hashed
    // differently for white and for black, and it could never be cancelled by
    // do_move, which cleared with masks under the owning colour instead.
    ifo.key ^= zobrist::castle_rights(ifo.cmask);
    ifo.repkey ^= zobrist::castle_rights(ifo.cmask);

    // ep square
    fen >> token;
    ifo.eps = no_square;
    Row row = no_row;
    Col col = no_col;
    for (auto& c : token) {
        if (c >= 'a' && c <= 'h')
            col = Col(c - 'a');
        if (c == '3' || c == '6')
            row = Row(c - '1');
    }
    ifo.eps = Square(8 * row + col);
    if (!util::on_board(ifo.eps))
        ifo.eps = no_square;
    if (ifo.eps != no_square) {
        ifo.key ^= zobrist::ep(util::col(ifo.eps));
        ifo.repkey ^= zobrist::ep(util::col(ifo.eps));
    }

    // half-moves since last pawn move/capture
    fen >> token;
    ifo.move50 = (token != "-" ? U8(std::stoi(token)) : 0);

    // move counter
    fen >> token;
    ifo.hmvs = (token != "-" ? U16(std::stoi(token)) : 0);

    // check info
    Color stm = to_move();
    ifo.ks[stm] = pcs.king_sq[stm];
    ifo.incheck = is_attacked(ifo.ks[stm], stm, Color(stm ^ 1));
    ifo.checkers = (in_check() ? attackers_of2(ifo.ks[stm], Color(stm ^ 1)) : 0ULL);
    ifo.pinned[stm] = pinned(stm);
    ifo.pinned[stm ^ 1] = pinned(Color(stm ^ 1));
}

std::string position::to_fen() const {
    std::string fen;
    for (int r = 7; r >= 0; --r) {
        int empties = 0;
        for (int c = 0; c < 8; ++c) {
            int s = r * 8 + c;
            if (piece_on(Square(s)) == no_piece) {
                ++empties;
                continue;
            }
            if (empties > 0)
                fen += std::to_string(empties);
            empties = 0;
            fen += kSanPiece[(color_on(Square(s)) == black ? piece_on(Square(s)) + 6
                                                           : piece_on(Square(s)))];
        }
        if (empties > 0)
            fen += std::to_string(empties);
        if (r > 0)
            fen += "/";
    }

    fen += (to_move() == white ? " w" : " b");

    // castle rights
    std::string c_str;
    if ((ifo.cmask & wks) == wks)
        c_str += "K";
    if ((ifo.cmask & wqs) == wqs)
        c_str += "Q";
    if ((ifo.cmask & bks) == bks)
        c_str += "k";
    if ((ifo.cmask & bqs) == bqs)
        c_str += "q";
    fen += (c_str.empty() ? " -" : " " + c_str);

    // ep-square
    std::string ep_sq;
    if (ifo.eps != no_square) {
        ep_sq += kSanCols[util::col(ifo.eps)];
        ep_sq += std::to_string(util::row(ifo.eps) + 1);
    }
    fen += (ep_sq.empty() ? " -" : " " + ep_sq);

    // move50
    fen += " " + std::to_string(ifo.move50);

    // half-mvs
    fen += " " + std::to_string(ifo.hmvs);

    return fen;
}

bool position::is_material_draw() const {
    // Any pawn, rook or queen leaves mate possible, and this is the overwhelming
    // majority of positions, so reject them with a single test before counting
    // anything else.
    if (number_of(white, pawn) || number_of(black, pawn) || number_of(white, rook) ||
        number_of(black, rook) || number_of(white, queen) || number_of(black, queen))
        return false;

    const unsigned wn = number_of(white, knight), wb = number_of(white, bishop);
    const unsigned bn = number_of(black, knight), bb = number_of(black, bishop);
    const unsigned w = wn + wb, b = bn + bb;

    // King against king, and king plus a single minor against a bare king, are
    // dead positions under the FIDE rules: no sequence of legal moves mates.
    if (w <= 1 && b <= 1)
        return true;

    // Two knights against a bare king cannot be forced, and the arbiter will
    // never award it, so treating it as anything but a draw only makes the
    // engine trade into it.
    if ((wn == 2 && wb == 0 && b == 0) || (bn == 2 && bb == 0 && w == 0))
        return true;

    return false;
}

bool position::is_draw() {
    if (ifo.move50 > 99)
        return true;

    // Trading down into king and a lone minor is a draw however winning the
    // material count looks. Without this the search happily walked into it: over
    // 158 gauntlet games haVoc drew 4 by insufficient mating material while its
    // own evaluation still read +3.6 to +3.9 on the very last move, because
    // nothing in the engine knew the position was dead.
    if (is_material_draw())
        return true;

    // A repetition cannot reach back past an irreversible move: a capture or a
    // pawn move changes the piece placement, and repkey covers piece placement,
    // so no position before one of them can equal this one. move50 counts
    // exactly the plies since the last such move, so it is the correct bound
    // and stopping there loses no match.
    //
    // This used to walk the whole game every time. is_draw() runs at nearly
    // every search node, so the cost grew with the move number: by move 60 each
    // node was reading about 60 history entries to prove something that could
    // not be true past the first few.
    U64 kcurrent = ifo.repkey;
    unsigned same_count = 0;
    const int size = static_cast<int>(history_.size());
    const int limit = std::max(0, size - static_cast<int>(ifo.move50));
    int idx = size - 2;
    while (same_count == 0 && idx >= limit) {
        same_count += (kcurrent == history_[idx].repkey);
        idx -= 2;
    }
    return same_count > 0;
}

// ─── do / undo move ─────────────────────────────────────────────────────────

void position::do_move(const Move& m) {
    history_.push_back(ifo);
    const Square from = Square(m.f);
    const Square to = Square(m.t);
    const Movetype t = Movetype(m.type);
    const Piece p = piece_on(from);
    const Color us = to_move();
    const U16 old_cmask = ifo.cmask;

    // king square update and castle rights update
    if (p == king) {
        pcs.king_sq[us] = to;
        ifo.ks[us] = to;
    }

    // Castling rights, hashed once from the finished mask below. Indexing by
    // both squares the move touches retires a right when the king leaves home,
    // when the rook leaves its corner, and when the rook is *captured* on its
    // corner -- the last of which nothing used to handle, so the engine went on
    // believing in a rook that was no longer on the board.
    ifo.cmask &= castle_rights_after(from) & castle_rights_after(to);

    ifo.captured = no_piece;

    if (t == quiet) {
        pcs.do_quiet(us, p, from, to, ifo);
    } else if (t == capture) {
        ifo.captured = piece_on(to);
        pcs.do_cap(us, p, from, to, ifo);
    } else if (t == ep) {
        ifo.captured = pawn;
        pcs.do_ep(us, from, to, ifo);
    } else if (t < capture_promotion_q) {
        pcs.do_promotion(us,
                         (t == promotion_q   ? queen
                          : t == promotion_r ? rook
                          : t == promotion_b ? bishop
                                             : knight),
                         from, to, ifo);
    } else if (t < castle_ks) {
        ifo.captured = piece_on(to);
        pcs.do_promotion_cap(us,
                             (t == capture_promotion_q   ? queen
                              : t == capture_promotion_r ? rook
                              : t == capture_promotion_b ? bishop
                                                         : knight),
                             from, to, ifo);
    } else if (t == castle_ks) {
        pcs.do_castle_ks(us, from, to, ifo);
        ifo.cmask &= (us == white ? clearw : clearb);
    } else if (t == castle_qs) {
        pcs.do_castle_qs(us, from, to, ifo);
        ifo.cmask &= (us == white ? clearw : clearb);
    }

    if (ifo.cmask != old_cmask) {
        ifo.key ^= zobrist::castle_rights(old_cmask) ^ zobrist::castle_rights(ifo.cmask);
        ifo.repkey ^= zobrist::castle_rights(old_cmask) ^ zobrist::castle_rights(ifo.cmask);
    }

    // En passant. The square the previous move offered has to be hashed back
    // out as it expires, exactly as do_null_move already did. Only XORing the
    // new one in left every square ever offered in the key, so the key recorded
    // the history of double pushes instead of the one square, if any, that can
    // actually be captured on now.
    if (ifo.eps != no_square) {
        ifo.key ^= zobrist::ep(util::col(ifo.eps));
        ifo.repkey ^= zobrist::ep(util::col(ifo.eps));
    }
    ifo.eps = no_square;
    if (p == pawn && std::abs(from - to) == 16) {
        ifo.eps = Square(from + (us == white ? 8 : -8));
        ifo.key ^= zobrist::ep(util::col(ifo.eps));
        ifo.repkey ^= zobrist::ep(util::col(ifo.eps));
    }

    // move50
    //
    // Neither move50 nor hmvs belongs in the transposition key: the key must
    // identify a position, and these describe how the game arrived at it. The
    // 50-move rule is enforced by is_draw() reading move50 directly, and
    // repetition by repkey, so nothing here depends on them being hashed.
    if (p == pawn || t == capture)
        ifo.move50 = 0;
    else
        ifo.move50++;

    // half-moves
    ifo.hmvs++;

    // Side to move. Both terms have to move: XOR the outgoing side back out
    // before XORing the incoming side in. Only adding the new one leaves the
    // old one in the key, so the side-to-move contribution accumulates over a
    // game instead of toggling, giving it a period of four plies rather than
    // two.
    ifo.key ^= zobrist::stm(ifo.stm);
    ifo.repkey ^= zobrist::stm(ifo.stm);
    ifo.stm = Color(ifo.stm ^ 1);
    ifo.key ^= zobrist::stm(ifo.stm);
    ifo.repkey ^= zobrist::stm(ifo.stm);

    ifo.incheck = is_attacked(king_square(), ifo.stm, us);
    ifo.checkers = (ifo.incheck ? attackers_of2(king_square(), Color(ifo.stm ^ 1)) : 0ULL);
    ifo.pinned[ifo.stm] = pinned(ifo.stm);
    ifo.pinned[ifo.stm ^ 1] = pinned(Color(ifo.stm ^ 1));
    ++nodes_searched;
}

void position::undo_move(const Move& m) {
    const Square from = Square(m.t);
    const Square to = Square(m.f);
    const Movetype t = Movetype(m.type);
    const Piece p = piece_on(from);
    const Color us = Color(to_move() ^ 1);
    Piece cp = ifo.captured;

    if (t == quiet) {
        pcs.do_quiet(us, p, from, to, ifo);
    } else if (t == capture) {
        pcs.do_quiet(us, p, from, to, ifo);
        pcs.add_piece(to_move(), cp, from, ifo);
    } else if (t == ep) {
        pcs.do_quiet(us, p, from, to, ifo);
        pcs.add_piece(to_move(), cp, Square(from + (us == white ? -8 : 8)), ifo);
    } else if (t < capture_promotion_q) {
        pcs.remove_piece(us, piece_on(from), from, ifo);
        pcs.add_piece(us, pawn, to, ifo);
    } else if (t < castle_ks) {
        pcs.remove_piece(us, piece_on(from), from, ifo);
        pcs.add_piece(to_move(), cp, from, ifo);
        pcs.add_piece(us, pawn, to, ifo);
    } else if (t == castle_ks) {
        Square rt = (us == white ? H1 : H8);
        Square rf = (us == white ? F1 : F8);
        pcs.do_quiet(us, king, from, to, ifo);
        pcs.do_quiet(us, rook, rf, rt, ifo);
    } else if (t == castle_qs) {
        Square rf = (us == white ? D1 : D8);
        Square rt = (us == white ? A1 : A8);
        pcs.do_quiet(us, king, from, to, ifo);
        pcs.do_quiet(us, rook, rf, rt, ifo);
    }
    ifo = history_.back();
    history_.pop_back();
}

// ─── null moves ─────────────────────────────────────────────────────────────

void position::do_null_move() {
    const Color us = to_move();
    const Color them = Color(us ^ 1);

    history_.push_back(ifo);

    if (ifo.eps != no_square) {
        ifo.key ^= zobrist::ep(util::col(ifo.eps));
        ifo.repkey ^= zobrist::ep(util::col(ifo.eps));
        ifo.eps = no_square;
    }

    // As in do_move: the outgoing side has to be XORed back out.
    ifo.key ^= zobrist::stm(ifo.stm);
    ifo.repkey ^= zobrist::stm(ifo.stm);
    ifo.stm = them;
    ifo.key ^= zobrist::stm(ifo.stm);
    ifo.repkey ^= zobrist::stm(ifo.stm);

    ifo.move50++;
    ifo.hmvs++;
}

void position::undo_null_move() {
    ifo = history_.back();
    history_.pop_back();
}

// ─── SEE ────────────────────────────────────────────────────────────────────

// Indexed by Piece: pawn, knight, bishop, rook, queen, king. The king entry is
// only ever a "this cannot be allowed to happen" sentinel and is not tunable.
static std::array<int, 6> mvals{100, 300, 315, 480, 910, 2000};

void position::set_see_values(const std::array<int, 5>& pawn_through_queen) {
    for (int i = 0; i < 5; ++i)
        mvals[static_cast<std::size_t>(i)] = pawn_through_queen[static_cast<std::size_t>(i)];
}

std::array<int, 6> position::see_values() { return mvals; }

int position::see(const Move& m) const {
    return see_move(m);
}

// Static exchange evaluation: play out the capture sequence on the destination
// square, always recapturing with the least valuable attacker, and return the
// material the mover ends up with assuming either side may stop when continuing
// would lose material.
//
// Pins are deliberately ignored. A pinned piece can still be a legal recapture
// when it stays on the pin ray, and treating every pinned piece as absent
// distorts the result far more often than including it does.
int position::see_move(const Move& m) const {
    const Square from = Square(m.f);
    const Square to = Square(m.t);
    const Movetype mt = Movetype(m.type);
    const Color us = to_move();

    Piece on_target = piece_on(from);
    if (on_target == no_piece)
        return 0;

    U64 occ = all_pieces();
    int gain = 0;

    if (mt == ep) {
        const Square capsq = Square(us == white ? to - 8 : to + 8);
        gain = mvals[pawn];
        occ ^= bitboards::squares[capsq];
    } else {
        const Piece victim = piece_on(to);
        if (victim == king)
            return mvals[king];
        if (victim != no_piece)
            gain = mvals[victim];
    }

    // A capture-promotion also trades the pawn for the promoted piece, and it is
    // the promoted piece that stands on the target for the rest of the sequence.
    if (mt >= capture_promotion_q && mt <= capture_promotion_n) {
        const Piece promoted = (mt == capture_promotion_q   ? queen
                                : mt == capture_promotion_r ? rook
                                : mt == capture_promotion_b ? bishop
                                                            : knight);
        gain += mvals[promoted] - mvals[pawn];
        on_target = promoted;
    }

    occ ^= bitboards::squares[from];

    const U64 diagonal = pcs.bitmap[white][bishop] | pcs.bitmap[black][bishop] |
                         pcs.bitmap[white][queen] | pcs.bitmap[black][queen];
    const U64 straight = pcs.bitmap[white][rook] | pcs.bitmap[black][rook] |
                         pcs.bitmap[white][queen] | pcs.bitmap[black][queen];

    int swap[32];
    swap[0] = gain;
    int d = 0;

    Color stm = Color(us ^ 1);
    U64 attackers = attackers_of(to, occ) & occ;

    while (d < 31) {
        const U64 stm_attackers = attackers & pcs.bycolor[stm];
        if (!stm_attackers)
            break;

        // Recapture with the least valuable attacker.
        Piece next = no_piece;
        U64 from_bb = 0ULL;
        for (int pt = pawn; pt <= king; ++pt) {
            const U64 b = stm_attackers & pcs.bitmap[stm][pt];
            if (b) {
                next = Piece(pt);
                from_bb = b & (~b + 1ULL);
                break;
            }
        }

        // Capturing with the king is only legal once the square is undefended.
        if (next == king && (attackers & pcs.bycolor[stm ^ 1]))
            break;

        ++d;
        swap[d] = mvals[on_target] - swap[d - 1];

        occ ^= from_bb;
        // Removing the recapturing piece can uncover a slider behind it. A pawn,
        // bishop or queen can only have been shielding a diagonal slider, and a
        // rook or queen an orthogonal one. A knight never blocks a ray to the
        // target square, and a king capture always ends the sequence.
        if (next == pawn || next == bishop || next == queen)
            attackers |= magics::attacks<bishop>(occ, to) & diagonal;
        if (next == rook || next == queen)
            attackers |= magics::attacks<rook>(occ, to) & straight;
        attackers &= occ;

        on_target = next;
        stm = Color(stm ^ 1);
    }

    // Walk the sequence back, letting either side decline to continue.
    while (d > 0) {
        swap[d - 1] = std::min(-swap[d], swap[d - 1]);
        --d;
    }
    return swap[0];
}

// ─── Promotions ─────────────────────────────────────────────────────────────

static inline bool is_promotion_type(const Movetype& mt) {
    return mt == promotion || mt == promotion_q || mt == promotion_r || mt == promotion_b ||
           mt == promotion_n;
}

bool position::is_cap_promotion(const Movetype& mt) {
    return mt == capture_promotion_q || mt == capture_promotion_r || mt == capture_promotion_b ||
           mt == capture_promotion_n;
}

bool position::is_promotion(const U8& mt) {
    return is_promotion_type(Movetype(mt)) || is_cap_promotion(Movetype(mt));
}

// ─── gives_check / quiet_gives_dangerous_check ──────────────────────────────

bool position::gives_check(const Move& m) {
    auto mask = 0ULL;
    auto c = to_move();
    auto them = Color(c ^ 1);
    auto isCapture = m.type == capture || m.type == capture_promotion ||
                     m.type == capture_promotion_b || m.type == capture_promotion_n ||
                     m.type == capture_promotion_q || m.type == capture_promotion_r;
    auto isPromotion = is_promotion(m.type);
    auto isEp = m.type == ep;
    auto isCastles = m.type == castles || m.type == castle_ks || m.type == castle_qs;
    auto from = m.f;
    auto to = m.t;
    auto capSq = no_square;
    auto promotePiece = no_piece;
    auto piece = piece_on(Square(from));
    auto capPiece = no_piece;
    if (piece == king && !isCastles)
        return false;

    if (isCastles) {
        if (c == white) {
            to = (m.type == castle_ks ? F1 : D1);
            from = (m.type == castle_ks ? H1 : A1);
        } else if (c == black) {
            to = (m.type == castle_ks ? F8 : D8);
            from = (m.type == castle_ks ? H8 : A8);
        }
        pcs.bitmap[c][rook] ^= (bitboards::squares[from] | bitboards::squares[to]);
    } else {
        pcs.bitmap[c][piece] ^= (bitboards::squares[from] | bitboards::squares[to]);
        if (isPromotion)
            pcs.bitmap[c][piece] ^= bitboards::squares[to];
    }

    if (isCapture || isEp) {
        if (isEp)
            capSq = Square(to + (c == white ? -8 : 8));
        else
            capSq = Square(to);
        capPiece = piece_on(Square(capSq));
        pcs.bitmap[them][capPiece] ^= bitboards::squares[capSq];
        mask ^= bitboards::squares[capSq];
    }

    if (isPromotion) {
        if (m.type == capture_promotion_n || m.type == promotion_n)
            promotePiece = knight;
        if (m.type == capture_promotion_b || m.type == promotion_b)
            promotePiece = bishop;
        if (m.type == capture_promotion_r || m.type == promotion_r)
            promotePiece = rook;
        if (m.type == capture_promotion_q || m.type == promotion_q)
            promotePiece = queen;
        pcs.bitmap[c][promotePiece] |= bitboards::squares[to];
    }

    mask = (all_pieces() ^ bitboards::squares[from]) | bitboards::squares[to];

    auto target = king_square(them);
    auto checks = is_attacked(target, them, c, mask);

    if (isCastles) {
        pcs.bitmap[c][rook] ^= (bitboards::squares[to] | bitboards::squares[from]);
    } else {
        pcs.bitmap[c][piece] ^= (bitboards::squares[to] | bitboards::squares[from]);
        if (isCapture || isEp)
            pcs.bitmap[them][capPiece] |= bitboards::squares[capSq];
        if (isPromotion)
            pcs.bitmap[c][promotePiece] ^= bitboards::squares[to];
    }

    return checks;
}

bool position::quiet_gives_dangerous_check(const Move& m) {
    if (m.type != quiet)
        return false;

    U64 msk = (all_pieces() ^ bitboards::squares[m.f]) | bitboards::squares[m.t];
    auto us = to_move();
    auto them = Color(us ^ 1);
    auto target = king_square(them);
    auto friends = (us == white ? get_pieces<white>() : get_pieces<black>());
    auto atk = attackers_of(target, msk) & friends;

    if (atk == 0ULL)
        return false;
    if (bits::more_than_one(atk))
        return true;

    auto s = bits::pop_lsb(atk);
    auto p = piece_on(Square(s));
    auto rd = util::row_dist(s, target);
    auto cd = util::col_dist(s, target);
    auto dangerous_checker = (p == pawn || p == rook || p == queen);
    return dangerous_checker && rd <= 1 && cd <= 1;
}

// ─── is_legal ───────────────────────────────────────────────────────────────

bool position::is_legal(const Move& m) {
    Square f = Square(m.f);
    Square t = Square(m.t);
    Piece p = piece_on(f);
    Movetype mt = Movetype(m.type);
    if (mt == no_type)
        return false;
    Square ks = king_square();
    Color us = to_move();
    Color them = Color(us ^ 1);
    Square eks = king_square(them);
    auto pc = pcs.bitmap[them];
    bool ispromotion = is_promotion_type(mt);
    bool iscappromotion = is_cap_promotion(mt);
    bool slider = (p == rook || p == bishop || p == queen);
    bool pawncapture = iscappromotion || mt == capture || mt == ep;

    // basic checks
    if (p == no_piece)
        return false;
    if (f == t)
        return false;
    if (t == eks)
        return false;
    if (color_on(t) == us)
        return false;
    if (color_on(f) != us)
        return false;
    if ((mt == ep || mt == quiet || ispromotion) && piece_on(t) != no_piece)
        return false;
    if ((ispromotion || iscappromotion) && p != pawn)
        return false;
    if ((mt == capture || iscappromotion) && color_on(t) != them)
        return false;
    if ((mt == capture || iscappromotion) && piece_on(t) == king)
        return false;
    if (mt == ep && t != ifo.eps)
        return false;
    if (mt == ep && p != pawn)
        return false;

    if (p == pawn) {
        if (util::row_dist(f, t) != 1 && util::row_dist(f, t) != 2)
            return false;
        if (us == black && util::row(f) < util::row(t))
            return false;
        if (us == white && util::row(f) > util::row(t))
            return false;
        if ((mt == quiet || ispromotion) && util::col_dist(f, t) != 0)
            return false;
        if ((!ispromotion && !iscappromotion) && (util::row(t) == r1 || util::row(t) == r8))
            return false;
        if (pawncapture && (util::row_dist(f, t) != 1 || util::col_dist(f, t) != 1))
            return false;
        if (mt == quiet && util::row_dist(f, t) == 2) {
            if (us == white && util::row(f) != 1)
                return false;
            if (us == black && util::row(f) != 6)
                return false;
            Square s = Square(f + (us == white ? 8 : -8));
            if (piece_on(s) != no_piece)
                return false;
        }
    }

    if (p == knight) {
        int rd = util::row_dist(f, t);
        int cd = util::col_dist(f, t);
        if (std::min(rd, cd) != 1 || std::max(rd, cd) != 2)
            return false;
    }

    if (p == rook) {
        if (!util::same_row(f, t) && !util::same_col(f, t))
            return false;
    }

    if (p == bishop) {
        if (!util::on_diagonal(f, t))
            return false;
    }

    if (p == queen) {
        if (!util::same_row(f, t) && !util::same_col(f, t) && !util::on_diagonal(f, t))
            return false;
    }

    if (p == king) {
        if (!util::same_row(f, t) && !util::same_col(f, t) && !util::on_diagonal(f, t))
            return false;
        if ((mt == quiet || mt == capture) && util::same_row(f, t) && util::col_dist(f, t) != 1)
            return false;
        if ((mt == quiet || mt == capture) && util::same_col(f, t) && util::row_dist(f, t) != 1)
            return false;
        if (util::on_diagonal(f, t) && (util::row_dist(f, t) != 1 || util::col_dist(f, t) != 1))
            return false;
    }

    // pinned
    if ((bitboards::squares[f] & ifo.pinned[ifo.stm]) && !util::aligned(ks, f, t))
        return false;

    // ep can uncover a discovered check
    if (mt == ep) {
        Square csq = Square(t + (them == white ? 8 : -8));
        U64 msk = (all_pieces() ^ bitboards::squares[f] ^ bitboards::squares[csq]) |
                  bitboards::squares[t];
        return ((magics::attacks<bishop>(msk, ks) & (pc[queen] | pc[bishop])) == 0ULL) &&
               ((magics::attacks<rook>(msk, ks) & (pc[queen] | pc[rook])) == 0ULL);
    }

    // castles
    if (mt == castle_ks || mt == castle_qs) {
        if (in_check())
            return false;
        if (p != king)
            return false;
        if (piece_on(us == white ? E1 : E8) != king)
            return false;
        if (us == white && color_on(E1) != white)
            return false;
        if (us == black && color_on(E8) != black)
            return false;

        Square s1 = no_square;
        Square s2 = no_square;
        if (mt == castle_ks) {
            s1 = (us == white ? F1 : F8);
            s2 = (us == white ? G1 : G8);
        } else {
            s1 = (us == white ? D1 : D8);
            s2 = (us == white ? C1 : C8);
        }

        if (mt == castle_ks && !can_castle_ks())
            return false;
        if (mt == castle_qs && !can_castle_qs())
            return false;

        if (mt == castle_ks) {
            if (piece_on(us == white ? F1 : F8) != no_piece)
                return false;
            if (piece_on(us == white ? G1 : G8) != no_piece)
                return false;
            if (piece_on(us == white ? H1 : H8) != rook)
                return false;
            if (us == white && color_on(H1) != white)
                return false;
            if (us == black && color_on(H8) != black)
                return false;
        }
        if (mt == castle_qs) {
            if (piece_on(us == white ? B1 : B8) != no_piece)
                return false;
            if (piece_on(us == white ? C1 : C8) != no_piece)
                return false;
            if (piece_on(us == white ? D1 : D8) != no_piece)
                return false;
            if (piece_on(us == white ? A1 : A8) != rook)
                return false;
            if (us == white && color_on(A1) != white)
                return false;
            if (us == black && color_on(A8) != black)
                return false;
        }

        if (is_attacked(s1, us, them) || is_attacked(s2, us, them))
            return false;

        return true;
    }

    // king move legality
    if (p == king) {
        U64 msk = all_pieces() ^ bitboards::squares[ks];
        if (is_attacked(t, us, them, msk))
            return false;
    }

    // in check: must capture or block
    if (in_check() && p != king) {
        U64 checks = checkers();
        if (bits::more_than_one(checks))
            return false;

        Square check_f = Square(bits::pop_lsb(checks));

        if ((mt == capture || iscappromotion) && t != check_f)
            return false;

        Piece checker = piece_on(check_f);

        if ((mt == quiet || ispromotion) && (checker == pawn || checker == knight))
            return false;

        if ((mt == quiet || ispromotion) &&
            (checker == bishop || checker == rook || checker == queen)) {
            U64 empty = ~all_pieces();
            U64 evasion_target = bitboards::between[check_f][king_square()] & empty;
            U64 block_bb = evasion_target & bitboards::squares[t];
            if (block_bb == 0ULL)
                return false;
        }
    }

    if (slider) {
        U64 bb = bitboards::between[f][t];
        bb ^= bitboards::squares[f];
        bb ^= bitboards::squares[t];
        bb &= all_pieces();
        if (bb != 0ULL)
            return false;
    }

    return true;
}

// ─── Pinned / check ─────────────────────────────────────────────────────────

U64 position::pinned(const Color us) {
    const Color them = Color(us ^ 1);
    const Square ks = king_square(us);
    U64 pinned = 0ULL;
    U64 bs = pcs.bitmap[them][bishop] | pcs.bitmap[them][queen];
    U64 rs = pcs.bitmap[them][rook] | pcs.bitmap[them][queen];

    U64 sliders = (bs & bitboards::battks[ks]) | (rs & bitboards::rattks[ks]);

    if (sliders == 0ULL)
        return pinned;

    do {
        int sq = bits::pop_lsb(sliders);
        if (!util::aligned(sq, static_cast<int>(ks)))
            continue;

        U64 tmp = (bitboards::between[sq][ks] & all_pieces()) ^
                  (bitboards::squares[ks] | bitboards::squares[sq]);

        if (!bits::more_than_one(tmp))
            pinned |= tmp;
    } while (sliders);

    return pinned & pcs.bycolor[us];
}

bool position::in_check() const {
    return ifo.incheck;
}

bool position::in_dangerous_check() {
    if (!in_check())
        return false;
    if (ifo.checkers == 0ULL)
        return false;
    if (bits::more_than_one(ifo.checkers))
        return true;

    U64 chk = ifo.checkers;
    auto c = bits::pop_lsb(chk);
    auto p = piece_on(Square(c));
    auto rd = util::row_dist(c, static_cast<int>(king_square()));
    auto cd = util::col_dist(c, static_cast<int>(king_square()));
    auto us = to_move();
    auto them = Color(us ^ 1);
    auto dangerous_checker = (p == pawn || p == rook || p == queen);
    return dangerous_checker && rd <= 1 && cd <= 1 && is_attacked(Square(c), us, them);
}

// ─── Attack detection ───────────────────────────────────────────────────────

bool position::is_attacked(const Square& s, const Color& us, const Color& them, U64 m) const {
    auto p = pcs.bitmap[them];
    U64 stepper_attacks = (bitboards::pattks[us][s] & p[pawn]) | (bitboards::nmask[s] & p[knight]) |
                          (bitboards::kmask[s] & p[king]);

    if (stepper_attacks != 0ULL)
        return true;

    if (m == 0ULL)
        m = all_pieces();

    return (magics::attacks<bishop>(m, s) & (p[queen] | p[bishop])) ||
           (magics::attacks<rook>(m, s) & (p[queen] | p[rook]));
}

U64 position::attackers_of(const Square& s, const U64& m) const {
    auto p = [this](const Color& c, const Piece& pc) {
        return pcs.bitmap[c][pc];
    };
    U64 battck = magics::attacks<bishop>(m, s);
    U64 rattck = magics::attacks<rook>(m, s);
    U64 qattck = battck | rattck;

    return (bitboards::pattks[black][s] & p(white, pawn)) |
           (bitboards::pattks[white][s] & p(black, pawn)) |
           (bitboards::nmask[s] & (p(black, knight) | p(white, knight))) |
           (bitboards::kmask[s] & (p(black, king) | p(white, king))) |
           (battck & (p(white, bishop) | p(black, bishop))) |
           (rattck & (p(white, rook) | p(black, rook))) |
           (qattck & (p(white, queen) | p(black, queen)));
}

U64 position::attackers_of2(const Square& s, const Color& c) const {
    U64 m = all_pieces();
    auto p = pcs.bitmap[c];
    U64 battck = magics::attacks<bishop>(m, s);
    U64 rattck = magics::attacks<rook>(m, s);
    U64 qattck = battck | rattck;

    return (bitboards::pattks[c ^ 1][s] & p[pawn]) | (bitboards::nmask[s] & p[knight]) |
           (bitboards::kmask[s] & p[king]) | (battck & p[bishop]) | (rattck & p[rook]) |
           (qattck & p[queen]);
}

// ─── set_piece / clear / print ──────────────────────────────────────────────

void position::set_piece(char p, const Square& s) {
    auto it = std::find(kSanPiece.begin(), kSanPiece.end(), p);
    if (it == kSanPiece.end())
        return;
    auto idx = std::distance(kSanPiece.begin(), it);

    Color color = (idx < 6 ? white : black);
    Piece piece = Piece(idx < 6 ? idx : idx - 6);
    pcs.set(color, piece, s, ifo);
    if (piece == king)
        ifo.ks[color] = s;
}

void position::clear() {
    pcs.clear();
    history_.clear();
    nodes_searched = 0;
    qnodes_searched = 0;
    ifo = {};
}

void position::print() const {
    std::cout << "   +---+---+---+---+---+---+---+---+" << std::endl;
    for (Row r = r8; r >= r1; --r) {
        std::cout << " " << r + 1 << " ";
        for (Col c = A; c <= H; ++c) {
            Square s = Square(8 * r + c);
            if (pcs.piece_on[s] != no_piece) {
                Piece p = pcs.piece_on[s];
                std::cout << "| " << (pcs.color_on[s] == white ? kSanPiece[p] : kSanPiece[p + 6])
                          << " ";
            } else {
                std::cout << "|   ";
            }
        }
        std::cout << "|" << std::endl;
        std::cout << "   +---+---+---+---+---+---+---+---+" << std::endl;
    }
    std::cout << "     a   b   c   d   e   f   g   h  " << std::endl;
}

} // namespace havoc
