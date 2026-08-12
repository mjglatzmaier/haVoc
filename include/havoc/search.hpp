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

/// Milliseconds to spend on one move given the limits the GUI reported.
/// Kept free of the position so the policy can be tested directly.
double estimate_move_time(const SearchLimits& lims, bool white_to_move);

// ─── Search signals ─────────────────────────────────────────────────────────

struct SearchSignals {
    std::atomic<bool> stop{false};
    std::atomic<bool> ponder_hit{false};
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
    void stop();
    void wait();

    SearchSignals& signals() { return signals_; }
    hash_table& tt() { return tt_; }
    U64 total_nodes() const;

    void set_threads(int n);
    void set_hash_size(int mb);
    void clear();
    void load_params(const std::string& filename);

    /// Direct access to the search's own parameter block. Search constants
    /// (reductions, margins, pruning depths) have no Texel gradient and are
    /// tuned by SPSA against game results, so tests and tuners need to be able
    /// to perturb them in-process.
    parameters& params() { return params_; }

    /// Read-only view of the history heuristic. Exposed so that tests can
    /// assert which moves the search rewarded, which is otherwise only
    /// observable as a change in move ordering several plies away.
    const Movehistory& history() const { return history_; }

  private:
    hash_table tt_;
    Threadpool<Searchthread> search_threads_;
    Threadpool<Workerthread> worker_;
    SearchSignals signals_;
    parameters params_;
    std::atomic<bool> searching_{false};
    std::atomic<int> sel_depth_{0};
    std::mutex output_mutex_;
    int multi_pv_ = 1;

    // Per-search state
    std::vector<std::unique_ptr<position>> positions_;
    /// Deepest iteration each helper thread finished without being aborted.
    /// Used to pick which thread's result to play; a thread's raw score is not
    /// comparable across different depths.
    std::vector<int> completed_depth_;
    Movehistory history_;

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
    float lazy_eval_margin_search(int depth, bool advanced_pawn);
    float lazy_eval_margin(int depth, bool advanced_pawn);
};

} // namespace havoc
