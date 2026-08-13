#include "havoc/move_order.hpp"

#include "havoc/position.hpp"

#include <cmath>

namespace havoc {

// ─── Movehistory ────────────────────────────────────────────────────────────

Movehistory& Movehistory::operator=(const Movehistory& mh) {
    std::copy(std::begin(mh.history_), std::end(mh.history_), std::begin(history_));
    countermoves = mh.countermoves;
    return *this;
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

    Move empty;
    empty.set(0, 0, no_type);
    for (auto& color : countermoves)
        for (auto& from : color)
            std::fill(from.begin(), from.end(), empty);
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

int score_captures(const position& p, const Move& m, const Move& prev, const Move& followup,
                   const Move& threat, SearchNode* stack, const Movehistory* hist) {
    return p.see(m) * kCaptureSeeScale + stack->best_move_history()[p.to_move()][m.f][m.t];
}

int score_qcaptures(const position& p, const Move& m, const Move& prev, const Move& followup,
                    const Move& threat, SearchNode* stack, const Movehistory* hist) {
    return p.see(m);
}

int score_quiets(const position& p, const Move& m, const Move& prev, const Move& followup,
                 const Move& threat, SearchNode* stack, const Movehistory* hist) {
    auto tomove = p.to_move();
    int s = 0;
    if (hist)
        s = hist->score(m, tomove, prev, followup, threat);
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
        m_moves[m_size++] = ScoredMove(m, sc);
    }
}

void ScoredMoves::sort(int cutoff) {
    const unsigned N = m_size;
    ScoredMove key;
    int j;
    for (unsigned i = m_start + 1; i < N; ++i) {
        key = m_moves[i];
        j = static_cast<int>(i) - 1;
        while (j >= 0 && m_moves[j] < key) {
            m_moves[j + 1] = m_moves[j];
            --j;
        }
        m_moves[j + 1] = key;
    }
    create_chunk(cutoff);
}

void ScoredMoves::create_chunk(int cutoff) {
    m_start = m_end;
    for (unsigned i = m_start; i < m_size; ++i) {
        if (m_moves[i].s >= cutoff)
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
                           const Move& threat, bool skipQuiets, bool rootMoves) {
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
