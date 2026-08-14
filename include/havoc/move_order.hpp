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
    /// Continuation history context: the move that was played *into* this
    /// node, and the one played two plies before it. A node is handed these by
    /// its parent rather than reading stack - 1 and stack - 2 itself, because
    /// only the search guarantees those frames exist -- anything else holding a
    /// SearchNode* (a test, a tool) would be reading off the front of its own
    /// array. `no_piece` means "no predecessor at that distance": the frames
    /// above the root, and null moves, both say so honestly.
    Piece prev_piece = no_piece;
    U8 prev_to = 0;
    Piece prev2_piece = no_piece;
    U8 prev2_to = 0;

    /// Records `moved`, landing on `to`, as the move this node is playing, and
    /// hands the resulting context down to the child frame. Call it before
    /// do_move, while the from square still says what is standing on it.
    void push_context(Piece moved, U8 to) {
        (this + 1)->prev_piece = moved;
        (this + 1)->prev_to = to;
        (this + 1)->prev2_piece = prev_piece;
        (this + 1)->prev2_to = prev_to;
    }

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
    Movehistory();
    Movehistory& operator=(const Movehistory& mh);

    /// `bonus` is the magnitude applied to the cutoff move, and subtracted
    /// from every quiet that was tried and failed. The caller computes it so
    /// that its scale is tunable: it has to be a meaningful fraction of
    /// kMaxHistory or the table never leaves the neighbourhood of zero and
    /// every threshold read against it is dead.
    void update(const Color& c, const Move& m, const Move& previous, int bonus, int16_t eval,
                const std::vector<Move>& quiets, Move* killers);

    /// Penalty applied to every quiet move that was tried at a node which
    /// failed low. Without it the table only ever learns from beta cutoffs,
    /// where a mean of 0.34 quiets have been tried, so the negative side never
    /// develops and any threshold read against it is unreachable.
    void malus(const Color& c, int bonus, const std::vector<Move>& quiets);

    /// Continuation history counterparts. `stack` is the node whose move loop
    /// produced the cutoff (or fail-low); it carries the predecessor context.
    /// `p` must be the position at that node, with every searched move undone,
    /// so that piece_on(from) still identifies the piece each quiet moved.
    void update_continuation(const position& p, const SearchNode* stack, const Move& best,
                             int bonus, const std::vector<Move>& quiets);
    void malus_continuation(const position& p, const SearchNode* stack, int bonus,
                            const std::vector<Move>& quiets);

    void clear();

    int score(const Move& m, const Color& c, const Move& previous, const Move& followup,
              const Move& threat) const;
    int score(const Move& m, const Color& c) const;

    /// Sum of the two continuation tables for a candidate quiet `m`, scaled by
    /// the weights set through `set_continuation_weights`.
    int continuation_score(const position& p, const Move& m, const SearchNode* stack) const;

    /// ─── Correction history ─────────────────────────────────────────────
    ///
    /// The static evaluation is not a neutral estimator: it is wrong in ways
    /// that repeat. A pawn structure the evaluation systematically overrates
    /// will be overrated in every position that shares it, and the search
    /// discovers that afresh at every node, paying for it in nodes each time.
    /// Correction history remembers the gap between what the evaluation said
    /// and what the search came back with, keyed on the pawn structure, and
    /// applies it to the next static evaluation of a position with that same
    /// structure.
    ///
    /// Values are held at `kCorrGrain` times their centipawn value so that a
    /// running average can move by less than one centipawn per update, which
    /// matters because most updates are small and integer division would
    /// otherwise round them all to zero.
    static constexpr int kCorrGrain = 256;
    static constexpr int kCorrMax = kCorrGrain * 32;
    static constexpr size_t kCorrSize = 16384;

    /// Centipawn adjustment to add to a raw static evaluation.
    [[nodiscard]] int correction(Color c, U64 pawnkey) const {
        return corrections_[c][pawnkey & (kCorrSize - 1)] / kCorrGrain;
    }

    /// `diff` is the search result minus the corrected static evaluation, in
    /// centipawns. Deeper searches carry more weight, capped so that no single
    /// result can displace the accumulated average outright.
    void update_correction(Color c, U64 pawnkey, int depth, int diff);

    /// The scoring functions are plain function pointers with a fixed
    /// signature and no route to the parameter block, so the search hands the
    /// tunable continuation weights to the table itself.
    void set_continuation_weights(int pct1, int pct2) {
        cont_pct1_ = pct1;
        cont_pct2_ = pct2;
    }

  private:
    /// One continuation plane per predecessor distance: [0] is keyed on the
    /// move our opponent just played, [1] on our own move two plies back.
    /// Heap-allocated -- 2.4 MB will not fit on the stack, and SearchEngine is
    /// an ordinary local in main().
    struct ContinuationHistory {
        int table[2][pieces][squares][colors][pieces][squares] = {};
    };

    int* continuation_slot(int plane, const SearchNode* stack, Color c, Piece moved,
                           const Move& m);
    const int* continuation_slot(int plane, const SearchNode* stack, Color c, Piece moved,
                                 const Move& m) const;

    std::array<std::array<std::array<int, squares>, squares>, colors> history_;
    /// Indexed by [side to move][pawn key]. Two colours because the same pawn
    /// structure is a different proposition depending on whose move it is.
    std::array<std::array<int, kCorrSize>, colors> corrections_{};
    // Countermove table: indexed by [color_of_previous_move][from][to] -> best response
    std::array<std::array<std::array<Move, 64>, 64>, 2> countermoves{};
    std::unique_ptr<ContinuationHistory> continuation_;
    int cont_pct1_ = 100;
    int cont_pct2_ = 100;
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
    /// Uninitialised storage for the move list.
    ///
    /// `Move` carries default member initialisers, so `ScoredMove` is not
    /// trivially default-constructible and a plain `std::array` of them cannot
    /// be left alone: every construction writes all kMaxMoves entries. A
    /// Moveorder holds two of these and one is built at essentially every
    /// search node, which made this initialisation alone the single largest
    /// entry in the profile -- more self time than the whole evaluation.
    ///
    /// The union suppresses it. Entries are constructed on write in
    /// load_and_score(), and every read is bounded by m_start/m_end, which
    /// never exceed m_size -- the count actually written. ScoredMove is
    /// trivially destructible, so the entries need no cleanup.
    union Storage {
        Storage() {}
        ~Storage() {}
        std::array<ScoredMove, kMaxMoves> moves;
    };
    Storage m_store;
    unsigned m_size = 0;
    unsigned m_start = 0;
    unsigned m_end = 0;

    std::array<ScoredMove, kMaxMoves>& m_moves() { return m_store.moves; }
    const std::array<ScoredMove, kMaxMoves>& m_moves() const { return m_store.moves; }

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
    const ScoredMove& front() const { return m_moves()[m_start]; }
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
