#pragma once

/// @file move_order.hpp
/// @brief Move ordering for alpha-beta search.

#include "havoc/movegen.hpp"
#include "havoc/parameters.hpp"
#include "havoc/types.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <functional>
#include <memory>
#include <vector>

namespace havoc {

// ─── Search node (per-ply stack frame) ──────────────────────────────────────

/// Best-move history table, heap-allocated to avoid stack overflow.
struct BestMoveHistory {
    int bm[2][64][64] = {};
};

struct SearchNode {
    SearchNode() : bmh(std::make_unique<BestMoveHistory>()) {}

    U16 ply = 0;
    bool in_check = false;
    bool null_search = false;
    Move curr_move, best_move, threat_move;
    /// Set while verifying whether `excluded_move` is singular: the node is
    /// re-searched with this move skipped, so its result is not a true value
    /// for the position and must not be probed from or stored in the TT.
    Move excluded_move;
    Move* pv = nullptr;
    int sel_depth = 0;
    std::unique_ptr<BestMoveHistory> bmh;
    Move killers[4];
    int16_t static_eval = score::kNegInf;

    int (&best_move_history())[2][64][64] { return bmh->bm; }
};

// ─── Move history heuristic ─────────────────────────────────────────────────

struct Movehistory {
    Movehistory() { clear(); }
    Movehistory& operator=(const Movehistory& mh);

    void update(const Color& c, const Move& m, const Move& previous, int depth, int16_t eval,
                const std::vector<Move>& quiets, Move* killers);

    void clear();

    int score(const Move& m, const Color& c, const Move& previous, const Move& followup,
              const Move& threat) const;
    int score(const Move& m, const Color& c) const;

  private:
    std::array<std::array<std::array<int, squares>, squares>, colors> history_;
    std::array<std::array<Move, squares>, squares> counters_;
    // Countermove table: indexed by [color_of_previous_move][from][to] -> best response
    std::array<std::array<std::array<Move, 64>, 64>, 2> countermoves{};
    float counter_move_bonus_ = 5.0f;
};

// ─── Scored move ────────────────────────────────────────────────────────────

struct ScoredMove {
    ScoredMove() = default;
    ScoredMove(const Move& mv, int16_t sc) : m(mv), s(sc) {}
    Move m;
    int16_t s = score::kNegInf;
    bool operator>(const ScoredMove& o) const { return s > o.s; }
    bool operator<(const ScoredMove& o) const { return s < o.s; }
};

// ─── Scoring function types ─────────────────────────────────────────────────

using ScoreFunc =
    std::function<int16_t(const position& p, const Move& m, const Move& prev, const Move& followup,
                          const Move& threat, SearchNode* stack, const Movehistory* hist)>;

// ─── Scored moves array ─────────────────────────────────────────────────────

class ScoredMoves {
    std::vector<ScoredMove> m_moves;
    unsigned m_start = 0;
    unsigned m_end = 0;

    void load_and_score(const position& p, Movegen* moves, const std::vector<Move>& filters,
                        const Move& previous, const Move& followup, const Move& threat,
                        SearchNode* stack, const Movehistory* hist, ScoreFunc score_lambda);
    void sort(int16_t cutoff);

  public:
    ScoredMoves() = default;
    ScoredMoves(const position& p, Movegen* m, const std::vector<Move>& filters,
                const Move& previous, const Move& followup, const Move& threat, SearchNode* stack,
                const Movehistory* hist, ScoreFunc score_lambda, int16_t cutoff);

    int operator++() { return m_start++; }
    ScoredMove front() const { return m_moves[m_start]; }
    bool end() const { return m_start >= m_end; }
    unsigned size() const { return m_end - m_start; }
    void skip_rest() { m_start = m_end; }
    void create_chunk(int16_t cutoff);
};

// ─── Scoring lambdas ────────────────────────────────────────────────────────

int16_t score_captures(const position& p, const Move& m, const Move& prev, const Move& followup,
                       const Move& threat, SearchNode* stack, const Movehistory* hist);

int16_t score_qcaptures(const position& p, const Move& m, const Move& prev, const Move& followup,
                        const Move& threat, SearchNode* stack, const Movehistory* hist);

int16_t score_quiets(const position& p, const Move& m, const Move& prev, const Move& followup,
                     const Move& threat, SearchNode* stack, const Movehistory* hist);

// ─── Move order classes ─────────────────────────────────────────────────────

class Moveorder {
  protected:
    std::unique_ptr<ScoredMoves> m_captures;
    std::unique_ptr<ScoredMoves> m_quiets;
    std::unique_ptr<Movegen> m_movegen;
    std::vector<Move> killer_moves_;
    SearchNode* m_stack = nullptr;
    const Movehistory* m_hist = nullptr;

    bool m_incheck = false;
    bool m_isendgame = false;
    int root_counter_ = 0;

    enum Phase {
        HashMove,
        MateKiller1,
        MateKiller2,
        InitCaptures,
        GoodCaptures,
        Killer1,
        Killer2,
        InitQuiets,
        GoodQuiets,
        BadCaptures,
        BadQuiets,
        End
    };
    Phase m_phase = HashMove;

    virtual void next_phase();

  public:
    Moveorder() = default;
    Moveorder(position& p, Move& hashmove, SearchNode* stack, const Movehistory* hist);
    Moveorder(const Moveorder&) = delete;
    Moveorder(Moveorder&&) = delete;
    Moveorder& operator=(const Moveorder&) = delete;
    Moveorder& operator=(Moveorder&&) = delete;
    virtual ~Moveorder() = default;

    virtual bool next_move(position& pos, Move& m, const Move& previous, const Move& followup,
                           const Move& threat, bool skipQuiets, bool rootMvs = false);
};

class QMoveorder : public Moveorder {
  protected:
    enum Phase {
        HashMove,
        MateKiller1,
        MateKiller2,
        InitCaptures,
        GoodCaptures,
        Killer1,
        Killer2,
        BadCaptures,
        InitQuiets,
        GoodQuiets,
        BadQuiets,
        End
    };
    Phase m_phase = HashMove;

    bool valid_qmove(const Move& m) const;
    void next_phase() override;

  public:
    QMoveorder() = default;
    QMoveorder(position& p, Move& hashmove, SearchNode* stack, const Movehistory* hist);
    QMoveorder(const QMoveorder&) = delete;
    QMoveorder(QMoveorder&&) = delete;
    QMoveorder& operator=(const QMoveorder&) = delete;
    QMoveorder& operator=(QMoveorder&&) = delete;
    ~QMoveorder() override = default;

    bool next_move(position& pos, Move& m, const Move& previous, const Move& followup,
                   const Move& threat, bool skipQuiets, bool rootMoves = false) override;
};

} // namespace havoc
