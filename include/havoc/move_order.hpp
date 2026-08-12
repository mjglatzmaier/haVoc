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
#include <limits>
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

/// History scores saturate at +/-kMaxHistory so that they cannot overflow the
/// move ordering pipeline or grow without bound over a long search.
constexpr int kMaxHistory = 16384;

/// Capture scores combine the exchange value with a history term. The two live
/// on very different scales -- SEE spans roughly +/-910 while history saturates
/// at +/-kMaxHistory -- so the exchange value is scaled up before they are
/// added. This keeps the sign of the capture score equal to the sign of the SEE
/// value, which is what separates the good-capture phase from the bad-capture
/// phase, and leaves history to break ties between captures of equal value.
/// 4096 is the smallest power of two for which the narrowest gap between two
/// distinct exchange values (15, a bishop for a knight) outweighs the full
/// history range.
constexpr int kCaptureSeeScale = 4096;

/// Sentinel meaning "no cutoff" when splitting a scored move list into chunks.
/// It has to sit below every score a scoring function can produce, so it cannot
/// be one of the score:: constants -- those are search scores on a completely
/// different scale to ordering scores.
constexpr int kOrderAll = std::numeric_limits<int>::min();

/// Applies `bonus` to `h` with a decay proportional to the value already there,
/// which keeps |h| <= kMaxHistory while still letting recent evidence dominate.
/// Bonus applied to the move that previously refuted this exact predecessor
/// move, and penalty applied to a move that simply undoes the predecessor.
/// Quiet ordering is dominated by history, which saturates at +/-kMaxHistory,
/// so the counter-move term has to live on the same scale to have any effect:
/// it is set to an eighth of the history range, enough to lift a plausible
/// refutation above quiets with no track record while still letting strong
/// history evidence outrank it.
constexpr int kCounterMoveBonus = kMaxHistory / 8;

inline void apply_history_bonus(int& h, int bonus) {
    bonus = std::clamp(bonus, -kMaxHistory, kMaxHistory);
    h += bonus - h * std::abs(bonus) / kMaxHistory;
}

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
    // Countermove table: indexed by [color_of_previous_move][from][to] -> best response
    std::array<std::array<std::array<Move, 64>, 64>, 2> countermoves{};
};

// ─── Scored move ────────────────────────────────────────────────────────────

struct ScoredMove {
    ScoredMove() = default;
    ScoredMove(const Move& mv, int sc) : m(mv), s(sc) {}
    Move m;
    int s = score::kNegInf;
    bool operator>(const ScoredMove& o) const { return s > o.s; }
    bool operator<(const ScoredMove& o) const { return s < o.s; }
};

// ─── Scoring function types ─────────────────────────────────────────────────

/// A plain function pointer rather than std::function: every scorer has this
/// exact signature and captures nothing, and this sits on the hot path.
using ScoreFunc = int (*)(const position& p, const Move& m, const Move& prev,
                          const Move& followup, const Move& threat, SearchNode* stack,
                          const Movehistory* hist);

/// Upper bound on the number of moves in a position (the true maximum is 218).
constexpr unsigned kMaxMoves = 256;

/// Number of moves filtered out before scoring: hash move plus four killers.
constexpr unsigned kNumFilters = 5;

using MoveFilters = std::array<Move, kNumFilters>;

// ─── Scored moves array ─────────────────────────────────────────────────────

class ScoredMoves {
    std::array<ScoredMove, kMaxMoves> m_moves;
    unsigned m_size = 0;
    unsigned m_start = 0;
    unsigned m_end = 0;

    void load_and_score(const position& p, Movegen* moves, const MoveFilters& filters,
                        const Move& previous, const Move& followup, const Move& threat,
                        SearchNode* stack, const Movehistory* hist, ScoreFunc score_lambda);
    void sort(int cutoff);

  public:
    ScoredMoves() = default;

    /// Replaces the contents in place. Moveorder keeps its lists by value and
    /// refills them as the phases advance, so nothing here allocates per node.
    void reset(const position& p, Movegen* m, const MoveFilters& filters, const Move& previous,
               const Move& followup, const Move& threat, SearchNode* stack,
               const Movehistory* hist, ScoreFunc score_lambda, int cutoff);

    void clear() { m_size = m_start = m_end = 0; }

    int operator++() { return m_start++; }
    const ScoredMove& front() const { return m_moves[m_start]; }
    bool end() const { return m_start >= m_end; }
    unsigned size() const { return m_end - m_start; }
    void skip_rest() { m_start = m_end; }
    void create_chunk(int cutoff);
};

// ─── Scoring lambdas ────────────────────────────────────────────────────────

int score_captures(const position& p, const Move& m, const Move& prev, const Move& followup,
                   const Move& threat, SearchNode* stack, const Movehistory* hist);

int score_qcaptures(const position& p, const Move& m, const Move& prev, const Move& followup,
                    const Move& threat, SearchNode* stack, const Movehistory* hist);

int score_quiets(const position& p, const Move& m, const Move& prev, const Move& followup,
                 const Move& threat, SearchNode* stack, const Movehistory* hist);

// ─── Move order classes ─────────────────────────────────────────────────────

class Moveorder {
  protected:
    ScoredMoves m_captures;
    ScoredMoves m_quiets;
    Movegen m_movegen;
    MoveFilters killer_moves_;
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
