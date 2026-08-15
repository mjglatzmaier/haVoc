#include "havoc/move_order.hpp"

#include "havoc/position.hpp"

#include <cmath>
#include <cstring>

namespace havoc {

// ─── Movehistory ────────────────────────────────────────────────────────────

Movehistory::Movehistory() : continuation_(std::make_unique<ContinuationHistory>()) {
    clear();
}

Movehistory& Movehistory::operator=(const Movehistory& mh) {
    std::copy(std::begin(mh.history_), std::end(mh.history_), std::begin(history_));
    countermoves = mh.countermoves;
    *continuation_ = *mh.continuation_;
    cont_pct1_ = mh.cont_pct1_;
    cont_pct2_ = mh.cont_pct2_;
    return *this;
}

// ─── Continuation history ───────────────────────────────────────────────────
//
// The plain history table is keyed on (color, from, to) alone, so every quiet
// carries a single score averaged over every context it was ever tried in. A
// knight retreat that refutes one particular bishop sortie is indistinguishable
// from the same retreat played into an unrelated structure.
//
// Continuation history keys the same evidence on the move that preceded it:
// plane 0 on the opponent's reply we are answering, plane 1 on our own move two
// plies back. The two are kept apart because a predecessor played by the
// opponent and one played by us mean opposite things about the position.

const int* Movehistory::continuation_slot(int plane, const SearchNode* stack, Color c, Piece moved,
                                          const Move& m) const {
    Piece prev = plane == 0 ? stack->prev_piece : stack->prev2_piece;
    U8 to = plane == 0 ? stack->prev_to : stack->prev2_to;
    if (prev == no_piece || moved == no_piece)
        return nullptr;
    return &continuation_->table[plane][prev][to][c][moved][m.t];
}

int* Movehistory::continuation_slot(int plane, const SearchNode* stack, Color c, Piece moved,
                                    const Move& m) {
    return const_cast<int*>(
        static_cast<const Movehistory*>(this)->continuation_slot(plane, stack, c, moved, m));
}

int Movehistory::continuation_score(const position& p, const Move& m, const SearchNode* stack) const {
    if (m.type != static_cast<U8>(Movetype::quiet))
        return 0;
    Color c = p.to_move();
    Piece moved = p.piece_on(static_cast<Square>(m.f));
    int s = 0;
    if (const int* a = continuation_slot(0, stack, c, moved, m))
        s += *a * cont_pct1_ / 100;
    if (const int* b = continuation_slot(1, stack, c, moved, m))
        s += *b * cont_pct2_ / 100;
    return s;
}

void Movehistory::update_continuation(const position& p, const SearchNode* stack, const Move& best,
                                      int bonus, const std::vector<Move>& quiets) {
    if (best.type != static_cast<U8>(Movetype::quiet))
        return;
    Color c = p.to_move();
    for (int plane = 0; plane < 2; ++plane) {
        if (int* h = continuation_slot(plane, stack, c, p.piece_on(static_cast<Square>(best.f)),
                                       best))
            apply_history_bonus(*h, bonus);
        for (const auto& q : quiets) {
            if (q == best)
                continue;
            if (int* h =
                    continuation_slot(plane, stack, c, p.piece_on(static_cast<Square>(q.f)), q))
                apply_history_bonus(*h, -bonus);
        }
    }
}

void Movehistory::malus_continuation(const position& p, const SearchNode* stack, int bonus,
                                     const std::vector<Move>& quiets) {
    Color c = p.to_move();
    for (int plane = 0; plane < 2; ++plane)
        for (const auto& q : quiets)
            if (int* h =
                    continuation_slot(plane, stack, c, p.piece_on(static_cast<Square>(q.f)), q))
                apply_history_bonus(*h, -bonus);
}

void Movehistory::update(const Color& c, const Move& m, const Move& previous, int bonus,
                         int16_t eval, const std::vector<Move>& quiets, Move* killers) {
    if (m.type == static_cast<U8>(Movetype::quiet)) {
        apply_history_bonus(history_[c][m.f][m.t], bonus);
        if (previous.type != static_cast<U8>(no_type)) {
            int opp = 1 - static_cast<int>(c);
            countermoves[opp][previous.f][previous.t] = m;
        }
        if (eval < score::kMateMaxPly && m != killers[2] && m != killers[3] && m != killers[0]) {
            killers[1] = killers[0];
            killers[0] = m;
        }
        // Penalise the quiet moves that were tried and failed, skipping the one
        // that caused the cutoff. Comparing only the from-square let a single
        // piece's other destinations escape the penalty while penalising moves
        // that merely started on the same square.
        for (auto& q : quiets) {
            if (m == q)
                continue;
            apply_history_bonus(history_[c][q.f][q.t], -bonus);
        }
    }

    // mate killers
    if (eval >= score::kMateMaxPly && m != killers[0] && m != killers[1] && m != killers[2]) {
        killers[3] = killers[2];
        killers[2] = m;
    }
}

void Movehistory::malus(const Color& c, int bonus, const std::vector<Move>& quiets) {
    for (auto& q : quiets)
        apply_history_bonus(history_[c][q.f][q.t], -bonus);
}

void Movehistory::clear() {
    for (auto& v : history_)
        for (auto& w : v)
            std::fill(w.begin(), w.end(), 0);

    if (continuation_)
        std::memset(continuation_->table, 0, sizeof(continuation_->table));

    Move empty;
    empty.set(0, 0, no_type);
    for (auto& color : countermoves)
        for (auto& from : color)
            std::fill(from.begin(), from.end(), empty);

    for (auto& c : corrections_)
        std::fill(c.begin(), c.end(), 0);
}

void Movehistory::update_correction(Color c, U64 pawnkey, int depth, int diff) {
    int& e = corrections_[c][pawnkey & (kCorrSize - 1)];

    // A single search result is one noisy sample of a systematic bias, so it
    // is folded into a running average rather than written over it. The weight
    // rises with depth because a deeper search is a better measurement, and is
    // capped at 16/256 so that no one node can move the entry more than a
    // sixteenth of the way to its own value.
    const int w = std::min(depth + 1, 16);
    const int sample = std::clamp(diff * kCorrGrain, -kCorrMax, kCorrMax);
    e = (e * (256 - w) + sample * w) / 256;
    e = std::clamp(e, -kCorrMax, kCorrMax);
}

int Movehistory::score(const Move& m, const Color& c) const {
    return history_[c][m.f][m.t];
}

int Movehistory::score(const Move& m, const Color& c, const Move& previous, const Move& followup,
                       const Move& threat) const {
    int s = history_[c][m.f][m.t];
    int opp = 1 - static_cast<int>(c);
    if (previous.type != static_cast<U8>(no_type) && countermoves[opp][previous.f][previous.t] == m)
        s += kCounterMoveBonus;
    if (followup.type != static_cast<U8>(no_type) && followup.f == m.t && followup.t == m.f)
        s -= kCounterMoveBonus;
    // threat is the opponent capture that refuted our null move, so the piece
    // it lands on is ours and is the one under threat. Moving that piece is
    // the most likely refutation of the threat, so it is worth trying early.
    if (threat.type != static_cast<U8>(no_type) && m.f == threat.t)
        s += kCounterMoveBonus;
    return s;
}

// ─── Scoring lambdas ────────────────────────────────────────────────────────

// These three share one signature so they can be passed interchangeably as the
// scoring callback in generate_and_score(); each uses only the subset of the
// arguments its phase needs, so the rest are deliberately unnamed here.

int score_captures(const position& p, const Move& m, const Move& /*prev*/,
                   const Move& /*followup*/, const Move& /*threat*/, SearchNode* stack,
                   const Movehistory* /*hist*/) {
    return p.see(m) * kCaptureSeeScale + stack->best_move_history()[p.to_move()][m.f][m.t];
}

int score_qcaptures(const position& p, const Move& m, const Move& /*prev*/,
                    const Move& /*followup*/, const Move& /*threat*/, SearchNode* /*stack*/,
                    const Movehistory* /*hist*/) {
    return p.see(m);
}

int score_quiets(const position& p, const Move& m, const Move& prev, const Move& followup,
                 const Move& threat, SearchNode* stack, const Movehistory* hist) {
    auto tomove = p.to_move();
    int s = 0;
    if (hist)
        s = hist->score(m, tomove, prev, followup, threat) + hist->continuation_score(p, m, stack);
    return s + stack->best_move_history()[tomove][m.f][m.t];
}

// ─── ScoredMoves ────────────────────────────────────────────────────────────

void ScoredMoves::reset(const position& p, Movegen* m, const MoveFilters& filters,
                        const Move& previous, const Move& followup, const Move& threat,
                        SearchNode* stack, const Movehistory* hist, ScoreFunc score_lambda,
                        int cutoff) {
    clear();
    load_and_score(p, m, filters, previous, followup, threat, stack, hist, score_lambda);
    sort(cutoff);
}

void ScoredMoves::load_and_score(const position& p, Movegen* moves,
                                 const MoveFilters& filters, const Move& previous,
                                 const Move& followup, const Move& threat, SearchNode* stack,
                                 const Movehistory* hist, ScoreFunc score_lambda) {
    m_start = m_end = 0;
    m_size = 0;
    for (int i = 0; i < moves->size() && m_size < kMaxMoves; ++i) {
        auto m = (*moves)[i];

        // skip hash moves and killers
        if (m == filters[0] || m == filters[1] || m == filters[2] || m == filters[3] ||
            m == filters[4])
            continue;

        int sc = score_lambda(p, m, previous, followup, threat, stack, hist);
        // Placement construction rather than assignment: the storage is
        // deliberately left uninitialised, so this is the write that begins
        // the entry's lifetime.
        std::construct_at(&m_moves()[m_size++], m, sc);
    }
}

void ScoredMoves::sort(int cutoff) {
    const unsigned N = m_size;
    auto& moves = m_moves();
    ScoredMove key;
    int j;
    for (unsigned i = m_start + 1; i < N; ++i) {
        key = moves[i];
        j = static_cast<int>(i) - 1;
        while (j >= 0 && moves[j] < key) {
            moves[j + 1] = moves[j];
            --j;
        }
        moves[j + 1] = key;
    }
    create_chunk(cutoff);
}

void ScoredMoves::create_chunk(int cutoff) {
    m_start = m_end;
    for (unsigned i = m_start; i < m_size; ++i) {
        if (m_moves()[i].s >= cutoff)
            m_end++;
    }
}

// ─── Moveorder ──────────────────────────────────────────────────────────────

Moveorder::Moveorder(position& p, Move& hashmove, SearchNode* stack, const Movehistory* hist)
    : m_movegen(p), m_stack(stack), m_hist(hist) {
    m_incheck = p.in_check();
    m_isendgame = false;
    killer_moves_ = {hashmove, stack->killers[0], stack->killers[1], stack->killers[2],
                     stack->killers[3]};
}

bool Moveorder::next_move(position& pos, Move& m, const Move& previous, const Move& followup,
                          const Move& threat, bool skipQuiets, bool rootMvs) {
    m = {};

    if (rootMvs) {
        if (root_counter_ < static_cast<int>(pos.root_moves.size())) {
            m = pos.root_moves[root_counter_++].pv[0];
            return true;
        }
        return false;
    }

    switch (m_phase) {
    case HashMove:
        m = killer_moves_[0];
        break;
    case MateKiller1:
        if (!(killer_moves_[3] == killer_moves_[0]))
            m = killer_moves_[3];
        break;
    case MateKiller2:
        if (!(killer_moves_[4] == killer_moves_[0]))
            m = killer_moves_[4];
        break;
    case Killer1:
        if (!(killer_moves_[1] == killer_moves_[0]))
            m = killer_moves_[1];
        break;
    case Killer2:
        if (!(killer_moves_[2] == killer_moves_[0]))
            m = killer_moves_[2];
        break;
    case InitCaptures:
        m_movegen.reset();
        m_movegen.generate<capture, pieces>();
        m_captures.reset(pos, &m_movegen, killer_moves_, previous, followup, threat, m_stack,
                         m_hist, score_captures, score::kDraw);
        break;
    case GoodCaptures:
    case BadCaptures:
        if (!m_captures.end()) {
            m = m_captures.front().m;
            m_captures.operator++();
        }
        break;
    case InitQuiets:
        if (!skipQuiets) {
            m_movegen.reset();
            m_movegen.generate<quiet, pieces>();
            m_quiets.reset(pos, &m_movegen, killer_moves_, previous, followup, threat, m_stack,
                           m_hist, score_quiets, score::kDraw);
        } else {
            m_quiets.clear();
        }
        break;
    case GoodQuiets:
    case BadQuiets:
        if (skipQuiets) {
            m_quiets.skip_rest();
        } else if (!m_quiets.end()) {
            m = m_quiets.front().m;
            m_quiets.operator++();
        }
        break;
    case End:
        return false;
    }
    next_phase();
    return true;
}

void Moveorder::next_phase() {
    if (m_phase == HashMove || m_phase == MateKiller1 || m_phase == MateKiller2 ||
        m_phase == Killer1 || m_phase == Killer2 || m_phase == InitCaptures ||
        m_phase == InitQuiets) {
        m_phase = static_cast<Phase>(m_phase + 1);
    } else if ((m_phase == GoodCaptures || m_phase == BadCaptures) && m_captures.end()) {
        m_captures.create_chunk(kOrderAll);
        m_phase = static_cast<Phase>(m_phase + 1);
    } else if ((m_phase == GoodQuiets || m_phase == BadQuiets) && m_quiets.end()) {
        m_quiets.create_chunk(kOrderAll);
        m_phase = static_cast<Phase>(m_phase + 1);
    }
}

// ─── QMoveorder ─────────────────────────────────────────────────────────────

QMoveorder::QMoveorder(position& p, Move& hashmove, SearchNode* stack, const Movehistory* hist)
    : Moveorder(p, hashmove, stack, hist) {}

bool QMoveorder::next_move(position& pos, Move& m, const Move& previous, const Move& followup,
                           const Move& threat, bool skipQuiets, bool /*rootMoves*/) {
    m = {};
    switch (m_phase) {
    case HashMove:
        if (valid_qmove(killer_moves_[0]) && killer_moves_[0].type != static_cast<U8>(no_type))
            m = killer_moves_[0];
        break;
    case MateKiller1:
        if (valid_qmove(killer_moves_[3]) && !(killer_moves_[3] == killer_moves_[0]))
            m = killer_moves_[3];
        break;
    case MateKiller2:
        if (valid_qmove(killer_moves_[4]) && !(killer_moves_[4] == killer_moves_[0]) &&
            !(killer_moves_[4] == killer_moves_[3]))
            m = killer_moves_[4];
        break;
    case Killer1:
        if (valid_qmove(killer_moves_[1]) && !(killer_moves_[1] == killer_moves_[0]))
            m = killer_moves_[1];
        break;
    case Killer2:
        if (valid_qmove(killer_moves_[2]) && !(killer_moves_[2] == killer_moves_[0]) &&
            !(killer_moves_[2] == killer_moves_[1]))
            m = killer_moves_[2];
        break;
    case InitCaptures:
        m_movegen.reset();
        m_movegen.generate<capture, pieces>();
        m_captures.reset(pos, &m_movegen, killer_moves_, previous, followup, threat, m_stack,
                         m_hist, score_qcaptures, score::kDraw);
        break;
    case GoodCaptures:
    case BadCaptures:
        if (!m_captures.end()) {
            m = m_captures.front().m;
            m_captures.operator++();
        }
        break;
    case InitQuiets:
        if (m_incheck && !skipQuiets) {
            m_movegen.reset();
            m_movegen.generate<quiet, pieces>();
            m_quiets.reset(pos, &m_movegen, killer_moves_, previous, followup, threat, m_stack,
                           m_hist, score_quiets, score::kDraw);
        } else {
            m_quiets.clear();
        }
        break;
    case GoodQuiets:
    case BadQuiets:
        if (skipQuiets)
            break;
        if (!m_quiets.end()) {
            m = m_quiets.front().m;
            m_quiets.operator++();
        }
        break;
    case End:
        return false;
    }
    next_phase();
    return true;
}

void QMoveorder::next_phase() {
    if (m_phase == HashMove || m_phase == MateKiller1 || m_phase == MateKiller2 ||
        m_phase == Killer1 || m_phase == Killer2 || m_phase == InitCaptures ||
        m_phase == InitQuiets) {
        m_phase = static_cast<Phase>(m_phase + 1);
    } else if ((m_phase == GoodCaptures || m_phase == BadCaptures) && m_captures.end()) {
        m_captures.create_chunk(kOrderAll);
        m_phase = static_cast<Phase>(m_phase + 1);
    } else if ((m_phase == GoodQuiets || m_phase == BadQuiets) && m_quiets.end()) {
        m_quiets.create_chunk(kOrderAll);
        m_phase = static_cast<Phase>(m_phase + 1);
    }
}

bool QMoveorder::valid_qmove(const Move& m) const {
    return m_incheck || m.type == static_cast<U8>(capture) || m.type == static_cast<U8>(ep) ||
           m.type == static_cast<U8>(capture_promotion_q) ||
           m.type == static_cast<U8>(capture_promotion_r) ||
           m.type == static_cast<U8>(capture_promotion_b) ||
           m.type == static_cast<U8>(capture_promotion_n);
}

} // namespace havoc
