#pragma once

/// @file search.hpp
/// @brief Alpha-beta search engine for haVoc.

#include "havoc/move_order.hpp"
#include "havoc/thread_pool.hpp"
#include "havoc/tt.hpp"
#include "havoc/types.hpp"
#include "havoc/utils.hpp"

#include <atomic>
#include <mutex>
#include <string>
#include <vector>

namespace havoc {

/// Bounds for the spin options advertised by the "uci" handshake. Declared here
/// so the advertised range and the enforced range cannot drift apart: uci.cpp
/// prints these and set_threads() clamps to them. The Hash bounds live in
/// tt.hpp, next to the table they constrain; unlike Threads, an out-of-range
/// Hash is refused rather than clamped, because the maximum is far larger than
/// any real machine can allocate.
constexpr int kMinThreads = 1;
constexpr int kMaxThreads = 1024;

// ─── Search limits (from UCI go command) ────────────────────────────────────

/// All values are milliseconds or plain counts and are never negative: the UCI
/// layer clamps whatever the GUI sends. They are signed so that a negative
/// clock reported by a GUI cannot wrap into an enormous positive budget.
struct SearchLimits {
    int wtime = 0, btime = 0, winc = 0, binc = 0;
    int movestogo = 0, nodes = 0, movetime = 0, mate = 0, depth = 0;
    bool infinite = false, ponder = false;
};

/// Floor on any timed search: below this the move is effectively instant and
/// the overhead of starting a search dominates.
inline constexpr double kMinSearchTime = 50.0;

/// Returned when the search should run until the GUI stops it.
inline constexpr double kNoTimeLimit = -1.0;

/// How much longer than the soft budget a single move may run before the timer
/// thread cuts it off. The soft limit is checked only between iterations, so
/// the hard limit is what bounds an iteration that turns out to be expensive --
/// a fail low at the root being the case that matters. Too tight and the engine
/// cannot spend extra time where it needs to; too loose and one bad move eats
/// the clock. The 33%-of-remaining cap still applies on top.
inline constexpr double kHardTimeFactor = 2.5;

/// Scaling of the soft budget by how settled the root move is.
///
/// `stable` counts completed iterations in a row that agreed on the best move.
/// The multiplier runs from kSoftTimeUnstable when the root has just changed
/// its mind down to kSoftTimeUnstable - kSoftTimeStep * kSoftTimeMaxStable once
/// it has held for several iterations, so the average move costs about what it
/// did before and the *distribution* is what moves: cheap when the move is not
/// in doubt, generous when it is.
inline constexpr double kSoftTimeUnstable = 1.10;
inline constexpr double kSoftTimeStep = 0.10;
inline constexpr int kSoftTimeMaxStable = 5;

/// The two limits a timed search runs under.
///
/// `soft` is the budget the search aims at: on finishing an iteration it asks
/// whether another one is affordable, and stops if it is not. That is where the
/// engine can be cheap on a position whose best move is not in doubt and
/// generous on one where it keeps changing its mind -- see the stability factor
/// in iterative_deepening.
///
/// `hard` is the deadline the timer thread enforces regardless. Nothing but a
/// stop from the GUI gets past it.
struct TimeBudget {
    double soft = kNoTimeLimit;
    double hard = kNoTimeLimit;
};

/// Milliseconds to spend on one move given the limits the GUI reported.
/// Kept free of the position so the policy can be tested directly.
TimeBudget estimate_time_budget(const SearchLimits& lims, bool white_to_move);

/// The hard deadline alone, for callers that only need the ceiling.
double estimate_move_time(const SearchLimits& lims, bool white_to_move);

/// The elapsed time at which a search holding `soft` as its budget should
/// decline to start another iteration, given how many consecutive completed
/// iterations have agreed on the root best move.
///
/// Free-standing so the scaling can be tested without running a search.
/// Returns kNoTimeLimit when there is no soft budget, which is the signal to
/// leave the decision entirely to the hard deadline.
double soft_time_target(double soft, int stable_iterations);

// ─── Search signals ─────────────────────────────────────────────────────────

struct SearchSignals {
    std::atomic<bool> stop{false};
    /// True while the current search is a ponder, i.e. the GUI has not yet
    /// confirmed the move we are assuming. Cleared by ponder_hit(), which is
    /// what puts the search on a clock. Was previously an atomic named
    /// ponder_hit that nothing ever read or wrote.
    std::atomic<bool> ponder{false};
};

// ─── Forward declarations for move_to_string (used by readout) ──────────────

namespace uci {
std::string move_to_string(const Move& m);
}

// ─── Search engine ──────────────────────────────────────────────────────────

class SearchEngine {
  public:
    SearchEngine();
    ~SearchEngine();

    void start(position& p, const SearchLimits& lims, bool silent);
    /// The GUI confirms the move the current ponder search assumed. The search
    /// continues, but from now on it is on the clock.
    void ponder_hit();

    void stop();
    void wait();

    /// True while a search is running. stop() only raises a flag, so this is
    /// how a caller tells that the search threads have actually finished with
    /// the position and the table; it also lets a test tell a ponder that is
    /// correctly waiting from one that has already given up.
    [[nodiscard]] bool searching() const { return searching_.load(); }

    SearchSignals& signals() { return signals_; }
    hash_table& tt() { return tt_; }
    U64 total_nodes() const;

    void set_threads(int n);
    /// Number of search threads currently running. Exposed so the clamp on
    /// set_threads() is testable.
    [[nodiscard]] unsigned threads() const { return search_threads_.size(); }
    /// Returns false if the table could not be resized, in which case the
    /// previous table is still in place.
    bool set_hash_size(int mb);
    void clear();
    void load_params(const std::string& filename);

    /// Direct access to the search's own parameter block. Search constants
    /// (reductions, margins, pruning depths) have no Texel gradient and are
    /// tuned by SPSA against game results, so tests and tuners need to be able
    /// to perturb them in-process.
    parameters& params() { return params_; }

    /// Read-only view of the first search thread's history heuristic. Exposed
    /// so that tests can assert which moves the search rewarded, which is
    /// otherwise only observable as a change in move ordering several plies
    /// away. Each thread now keeps its own tables, so this is the whole story
    /// only for a single-threaded search -- which is what the tests run.
    const Movehistory& history() const { return search_threads_[0]->history; }

  private:
    hash_table tt_;
    Threadpool<Searchthread> search_threads_;
    Threadpool<Workerthread> worker_;
    SearchSignals signals_;
    parameters params_;
    std::atomic<bool> searching_{false};
    std::mutex output_mutex_;
    int multi_pv_ = 1;

    // Per-search state
    std::vector<std::unique_ptr<position>> positions_;
    /// Deepest iteration each helper thread finished without being aborted.
    /// Used to pick which thread's result to play; a thread's raw score is not
    /// comparable across different depths.
    std::vector<int> completed_depth_;

    /// Move-ordering statistics for one search thread. One accessor rather than
    /// 26 direct reaches into the thread pool, so that where this state lives is
    /// decided in exactly one place -- moving it into a per-thread search
    /// context later is then a change to this function and nothing else.
    Movehistory& thread_history(int thread_id) { return search_threads_[thread_id]->history; }

    // Search methods
    void iterative_deepening(position& p, U16 depth, bool silent, int thread_id);

    template <Nodetype type>
    int search(position& pos, int alpha, int beta, U16 depth, SearchNode* stack, int thread_id);

    template <Nodetype type>
    int qsearch(position& pos, int alpha, int beta, U16 depth, SearchNode* stack, int thread_id);

    /// The limits the current search is running under.
    ///
    /// start() hands its work to a worker thread and returns immediately, so
    /// the SearchLimits the UCI layer built is a block-scoped local that is
    /// destroyed while the search is still running. Capturing it by reference
    /// left the timer thread reading a dead stack frame and budgeting the move
    /// from whatever the next command wrote there. Owned here instead.
    SearchLimits limits_{};

    /// Started once in start(), before any thread is spawned, and read by both
    /// the timer thread and the main search thread. Shared so that the two
    /// agree on when the move began: a clock created inside the timer thread
    /// starts late by however long the thread took to be scheduled, and the
    /// search would then overspend by exactly that much.
    util::Clock search_clock_{};

    /// The two budgets, as *durations* in milliseconds, or kNoTimeLimit when
    /// the search is not on a clock. Atomic because ponderhit rewrites them
    /// from the UCI thread while the search is running.
    std::atomic<double> soft_limit_{kNoTimeLimit};
    std::atomic<double> hard_limit_{kNoTimeLimit};

    /// Milliseconds on search_clock_ at which the budgets start counting.
    /// Zero for an ordinary search; on ponderhit it moves to "now", because the
    /// time spent pondering was free -- the GUI's clock only started then.
    /// Moving an origin rather than resetting the clock leaves the clock
    /// immutable while the search threads are reading it.
    std::atomic<double> time_origin_{0.0};

    /// Node ceiling from "go nodes N", or zero when unlimited. Checked inside
    /// search() rather than on the timer thread: a node limit is asked for
    /// precisely when a reproducible amount of work is wanted, so it must not
    /// depend on how fast the machine is.
    std::atomic<U64> node_limit_{0};

    /// Side to move for the search in progress, captured at start(), so that
    /// ponderhit can re-budget without the position.
    bool stm_is_white_ = true;

    void search_timer(position& p);
    double estimate_max_time(position& p) const;
    static void update_pv(Move* root_pv, const Move& move, Move* child);
    void readout_pv(SearchNode* stack, const Rootmoves& roots, int eval, int alpha, int beta,
                    U16 depth);

    // Pruning helpers
    static unsigned reduction(bool pv_node, bool improving, int d, int mc);
    int futility_move_count(bool improving, U16 depth);
    int history_bonus(int depth) const;
    int static_eval(position& p, int thread_id);
};

} // namespace havoc
