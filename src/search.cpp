#include "havoc/search.hpp"

#include "havoc/bitboard.hpp"
#include "havoc/eval/hce.hpp"
#include "havoc/movegen.hpp"
#include "havoc/position.hpp"

#include <algorithm>
#include <cassert>
#include <chrono>
#include <cmath>
#include <iostream>
#include <string>
#include <thread>

namespace havoc {

namespace {
const std::vector<float> kMaterialVals{100.0f, 300.0f, 315.0f, 480.0f, 910.0f};

// Mate scores are relative to the root, but a transposition may be reached at a
// different distance from the root than where it was stored. Convert to a
// node-relative value on store and back to a root-relative value on probe.
// A position can be a fifty move draw and checkmate at the same time, and mate
// ends the game before the rule can be claimed, so that case has to confirm the
// side to move still has a move. Repetitions carry the guarantee for free -- the
// same position occurred earlier and the game continued -- but the test is cheap
// enough here to just run for both.
bool has_legal_move(position& p) {
    Movegen mvs(p);
    mvs.generate<pseudo_legal, pieces>();
    for (int i = 0; i < mvs.size(); ++i)
        if (p.is_legal(mvs[i]))
            return true;
    return false;
}

inline int16_t score_to_tt(int score, int ply) {
    if (score >= score::kMateMaxPly)
        return static_cast<int16_t>(score + ply);
    if (score <= score::kMatedMaxPly)
        return static_cast<int16_t>(score - ply);
    return static_cast<int16_t>(score);
}

inline int score_from_tt(int score, int ply) {
    if (score >= score::kMateMaxPly)
        return score - ply;
    if (score <= score::kMatedMaxPly)
        return score + ply;
    return score;
}

// UCI wants forced mates reported as "score mate N", where N counts moves (not
// plies) and is negative when the side to move is getting mated. The root node
// sits at ply 1 rather than 0, so a score of kMate - d is a mate d - 1 plies
// away.
inline std::string uci_score_string(int score) {
    if (score >= score::kMateMaxPly) {
        int plies = score::kMate - score - 1;
        return "mate " + std::to_string((plies + 1) / 2);
    }
    if (score <= score::kMatedMaxPly) {
        int plies = score - score::kMated - 1;
        return "mate " + std::to_string(-((plies + 1) / 2));
    }
    return "cp " + std::to_string(score);
}
} // namespace

// ─── Construction / configuration ───────────────────────────────────────────

SearchEngine::SearchEngine() : worker_(1) {
    search_threads_.init(1);
}

SearchEngine::~SearchEngine() {
    stop();
    wait();
}

void SearchEngine::set_threads(int n) {
    // The bounds the "uci" handshake advertises. They were advertised but never
    // enforced: "setoption name Threads value 99999" tried to start 99999
    // threads and wedged the engine.
    n = std::clamp(n, kMinThreads, kMaxThreads);

    // Threadpool::init() destroys and recreates every Searchthread, so every
    // piece of engine-owned per-thread state has to be put back afterwards.
    // History is carried because start() documents that it persists through a
    // game and resets only on ucinewgame, and a GUI may change Threads at any
    // point; the tuned parameters are carried because otherwise the freshly
    // constructed threads evaluate with the compiled-in defaults.
    const bool had_threads = search_threads_.size() > 0;
    Movehistory carried;
    if (had_threads)
        carried = search_threads_[0]->history;

    search_threads_.init(n);

    for (unsigned i = 0; i < search_threads_.size(); ++i)
        configure_thread(i, had_threads ? &carried : nullptr);
}

bool SearchEngine::set_hash_size(int mb) {
    // Refused, not clamped. Clamping an out-of-range request up to the
    // advertised maximum turns a typo into a 32 TB allocation attempt, which an
    // overcommitting kernel grants and then OOM-kills on first touch. A size the
    // engine cannot honour leaves the existing table alone instead.
    if (mb < kMinHashMb || mb > kMaxHashMb)
        return false;
    return tt_.resize(static_cast<size_t>(mb));
}

void SearchEngine::clear() {
    tt_.clear();
    for (unsigned i = 0; i < search_threads_.size(); ++i)
        search_threads_[i]->history.clear();
}

void SearchEngine::configure_thread(unsigned i, const Movehistory* carried) {
    Searchthread* t = search_threads_[i];

    t->params = params_;

    // The pawn and material tables cache scores computed under the old
    // parameters. They are keyed by pawn/material structure, not by the
    // parameter set, so without an explicit clear the new values would be
    // masked by stale hits for the rest of the session.
    t->pawn_tbl.clear();
    t->material_tbl.clear();
    t->evaluator = std::make_unique<HCEEvaluator>(t->pawn_tbl, t->material_tbl, t->params);

    if (carried)
        t->history = *carried;
}

void SearchEngine::load_params(const std::string& filename) {
    params_.load(filename);
    for (unsigned i = 0; i < search_threads_.size(); ++i)
        configure_thread(i, nullptr);
}

// ─── Pruning helpers ────────────────────────────────────────────────────────

unsigned SearchEngine::reduction(bool pv_node, bool improving, int d, int mc) {
    return bitboards::reductions[static_cast<int>(pv_node)][static_cast<int>(improving)]
                                [std::max(0, std::min(d, 63))][std::max(0, std::min(mc, 63))];
}

/// Magnitude of a history update at this depth. The table saturates at
/// +/-kMaxHistory, and `apply_history_bonus` decays by h*|bonus|/kMaxHistory,
/// so the bonus has to be a real fraction of that range for the mechanism to
/// behave as designed rather than degenerate into a slow accumulator.
int SearchEngine::history_bonus(int depth) const {
    return params_.history_bonus_scale * depth * depth;
}

int SearchEngine::futility_move_count(bool improving, U16 depth) {
    return (params_.futility_base + depth * depth) / (2 - static_cast<int>(improving));
}

/// Full static evaluation with lazy cutoffs disabled. Used where a score is
/// needed but the position cannot be searched any further.
int SearchEngine::static_eval(position& p, int thread_id) {
    auto* sthread = search_threads_[thread_id];
    return sthread->evaluator->evaluate(p, -1);
}

// ─── Start search ───────────────────────────────────────────────────────────

void SearchEngine::start(position& p, const SearchLimits& lims, bool silent) {
    // Copy the limits into the engine before anything asynchronous can see
    // them. start() only enqueues work and returns, so the caller's object is
    // gone by the time the timer thread reads it -- see limits_ in search.hpp.
    limits_ = lims;
    // Wait for any previous search to finish
    wait();

    positions_.clear();
    // History is deliberately not cleared here. It is indexed by colour and
    // from/to square, so nothing in it is tied to the position it was learned
    // from, and the cutoff statistics it accumulates are the whole point of the
    // heuristic. Clearing it on every "go" threw that away before every move
    // and made each search start ordering-blind. ucinewgame calls
    // SearchEngine::clear(), which is the right granularity: history persists
    // through a game and is reset between games.

    signals_.stop = false;
    for (unsigned i = 0; i < search_threads_.size(); ++i)
        search_threads_[i]->history.set_continuation_weights(params_.cont_hist1_pct,
                                                             params_.cont_hist2_pct);
    tt_.new_search();

    p.set_nodes_searched(0);
    p.set_qnodes_searched(0);

    // Load root moves
    Movegen mvs(p);
    mvs.generate<pseudo_legal, pieces>();
    p.root_moves.clear();
    for (int i = 0; i < mvs.size(); ++i) {
        if (!p.is_legal(mvs[i]))
            continue;
        p.root_moves.push_back(Rootmove(mvs[i]));
    }

    // Create per-thread copies
    for (unsigned i = 0; i < search_threads_.size(); ++i) {
        positions_.emplace_back(std::make_unique<position>(p));
    }
    completed_depth_.assign(search_threads_.size(), 0);

    // "go mate N" asks for a mate in N moves, so there is no point looking
    // beyond the 2N-1 plies one can occupy -- and without a bound the search
    // never returned at all, since a mate search carries no clock. An explicit
    // "depth" still wins, because it is the more specific request.
    U16 depth = static_cast<U16>(MAX_PLY);
    if (lims.depth > 0)
        depth = static_cast<U16>(lims.depth);
    else if (lims.mate > 0)
        depth = static_cast<U16>(std::min(2 * lims.mate - 1, static_cast<int>(MAX_PLY)));

    search_clock_.reset();
    stm_is_white_ = (p.to_move() == white);
    const TimeBudget budget = estimate_time_budget(limits_, stm_is_white_);
    // Origin before limits, as in ponder_hit(): readers pair a limit they have
    // acquired with the origin, so the origin must never be the newer of the two.
    time_origin_.store(0.0, std::memory_order_relaxed);
    soft_limit_.store(budget.soft, std::memory_order_release);
    hard_limit_.store(budget.hard, std::memory_order_release);
    node_limit_.store(lims.nodes > 0 ? static_cast<U64>(lims.nodes) : 0,
                      std::memory_order_relaxed);
    signals_.ponder.store(limits_.ponder);
    searching_ = true;

    // Launch the entire search on the worker thread so UCI loop stays responsive.
    // The worker thread manages the timer, search threads, and result collection.
    worker_.enqueue([this, &p, depth, silent]() {
        // Launch timer
        Threadpool<Workerthread> timer_thread(1);
        timer_thread.enqueue([this, &p]() { search_timer(p); });

        // Launch helper threads
        if (search_threads_.size() > 1) {
            for (unsigned i = 1; i < search_threads_.size(); ++i) {
                int tid = static_cast<int>(i);
                search_threads_.enqueue([this, tid, depth, silent]() {
                    iterative_deepening(*positions_[tid], depth, silent, tid);
                });
            }
        }

        // Launch main thread search
        search_threads_.enqueue(
            [this, depth, silent]() { iterative_deepening(*positions_[0], depth, silent, 0); });

        search_threads_.wait_finished();
        signals_.stop = true;

        // Collect results. Helper threads all search to different depths, so
        // their root scores are not directly comparable: a shallow thread can
        // easily return a higher score than a deeper, more reliable one. Prefer
        // the thread that finished the deepest iteration, breaking ties on
        // score, and prefer a proven shorter mate over any of that.
        Rootmoves bestRoots;
        int best_depth = -1;
        int best_score_val = score::kNegInf;
        for (unsigned i = 0; i < positions_.size(); ++i) {
            const auto& t = positions_[i];
            if (t->root_moves.empty())
                continue;
            const int d = (i < completed_depth_.size() ? completed_depth_[i] : 0);
            const int sc = t->root_moves[0].score;
            if (d <= 0)
                continue;

            const bool mate = sc >= score::kMateMaxPly;
            const bool best_mate = best_score_val >= score::kMateMaxPly;

            bool better;
            if (mate || best_mate)
                better = sc > best_score_val;
            else
                better = (d != best_depth ? d > best_depth : sc > best_score_val);

            if (bestRoots.empty() || better) {
                best_depth = d;
                best_score_val = sc;
                bestRoots = t->root_moves;
            }
        }

        if (bestRoots.empty())
            bestRoots = positions_[0]->root_moves;

        // Copy results back to the original position
        p.root_moves = bestRoots;

        if (!silent) {
            // Every "go" must be answered. Checkmate and stalemate leave no
            // root move, and saying nothing at all left the GUI waiting on a
            // reply that was never coming -- an unbounded hang on any terminal
            // position, which is exactly what an analysis GUI sends when it
            // reaches the end of a game. "(none)" is the established way to
            // say there is no move.
            if (bestRoots.empty() || bestRoots[0].pv.empty()) {
                std::cout << "bestmove (none)" << std::endl;
            } else {
                std::cout << "bestmove " << uci::move_to_string(bestRoots[0].pv[0]);
                if (bestRoots[0].pv.size() > 1)
                    std::cout << " ponder " << uci::move_to_string(bestRoots[0].pv[1]);
                std::cout << std::endl;
            }
        }

        searching_ = false;
    });
}

void SearchEngine::stop() {
    signals_.stop = true;
}

void SearchEngine::wait() {
    worker_.wait_finished();
}

// ─── Timer ──────────────────────────────────────────────────────────────────

void SearchEngine::search_timer(position&) {
    // Deliberately the clock start() reset, not a fresh one: a local clock here
    // starts only once this thread is scheduled, so the setup it misses is time
    // the engine has already spent and would then overspend by.
    const util::Clock& c = search_clock_;

    // The deadline is re-read every poll rather than captured once, because
    // ponderhit moves it: a search that began as a ponder has no deadline at
    // all until the GUI says the guess was right. The three near-identical
    // loops this replaces each captured their limit up front, so nothing the
    // GUI said afterwards could be heard.
    while (!signals_.stop.load() && searching_.load()) {
        const double limit = hard_limit_.load(std::memory_order_acquire);
        const double origin = time_origin_.load(std::memory_order_relaxed);
        if (limit > kNoTimeLimit && c.elapsed_ms() - origin > limit)
            break;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    signals_.stop = true;
}

void SearchEngine::ponder_hit() {
    // The GUI played the move we guessed, so the search continues -- but it is
    // now on the clock, and was not a moment ago. Without this the engine
    // pondered on until stdin closed: measured at 20+ seconds of thinking on a
    // 2 second clock, which is a loss on time in every game a GUI ponders.
    if (!signals_.ponder.exchange(false))
        return;

    limits_.ponder = false;
    const TimeBudget b = estimate_time_budget(limits_, stm_is_white_);

    // Time spent pondering was free -- the GUI's clock only started now -- so
    // the budget runs from this instant. Moving the origin rather than
    // resetting the clock keeps the clock immutable while search threads read
    // it, and keeps the budgets as durations, which is what the stability
    // factor in iterative_deepening multiplies.
    //
    // The three are a tuple, and readers must not see it half-updated. A fresh
    // limit paired with the old origin (0) would charge the whole ponder to
    // this move and stop the search on the spot, so the origin is published
    // first and the limits with release; readers acquire the limit and then
    // read the origin. The reverse pairing -- old limit, new origin -- is what
    // remains, and is harmless: the old limit is kNoTimeLimit, so the timer
    // simply has no deadline for the 1 ms until its next poll.
    time_origin_.store(search_clock_.elapsed_ms(), std::memory_order_relaxed);
    soft_limit_.store(b.soft, std::memory_order_release);
    hard_limit_.store(b.hard, std::memory_order_release);
}

TimeBudget estimate_time_budget(const SearchLimits& lims, bool white_to_move) {
    if (lims.infinite || lims.ponder || lims.depth > 0)
        return {kNoTimeLimit, kNoTimeLimit};
    if (lims.movetime != 0) {
        // The GUI asked for exactly this much, so there is nothing to ration
        // and no later move to bank the savings for. Leaving a soft limit here
        // would be actively wrong: the stability scaling would cut a settled
        // search off at 0.6 of the time the GUI asked it to spend.
        return {kNoTimeLimit, static_cast<double>(lims.movetime)};
    }

    double our_time = (white_to_move ? lims.wtime : lims.btime);
    double our_inc = (white_to_move ? lims.winc : lims.binc);

    // "go" with no clock at all means there is nothing to budget against, so
    // search until the GUI stops us.
    const bool has_clock = lims.wtime > 0 || lims.btime > 0 || lims.winc > 0 || lims.binc > 0 ||
                           lims.movestogo > 0;
    if (!has_clock)
        return {kNoTimeLimit, kNoTimeLimit};

    // Our clock is spent, or the GUI reported a negative value that the UCI
    // layer clamped to zero. Returning kNoTimeLimit here used to mean "no
    // limit", so the engine answered a flag-fall by thinking forever and
    // losing on time. Move as quickly as we can instead.
    if (our_time <= 0)
        return {kMinSearchTime, kMinSearchTime};

    // Sudden death: assume a fixed number of moves still to play.
    const double moves_left = (lims.movestogo > 0 ? static_cast<double>(lims.movestogo) : 25.0);

    const double base_time = our_time / moves_left + our_inc * 0.9;
    // Never commit more than a third of what is left to a single move.
    const double max_time = our_time * 0.33;

    const double hard =
        std::max(kMinSearchTime, std::min(base_time * kHardTimeFactor, max_time));
    const double soft = std::max(kMinSearchTime, std::min(base_time, hard));
    return {soft, hard};
}

double estimate_move_time(const SearchLimits& lims, bool white_to_move) {
    return estimate_time_budget(lims, white_to_move).hard;
}

double soft_time_target(double soft, int stable_iterations) {
    if (soft <= 0.0)
        return kNoTimeLimit;
    const double factor =
        kSoftTimeUnstable - kSoftTimeStep * std::min(stable_iterations, kSoftTimeMaxStable);
    // kMinSearchTime is a floor, so it has to survive the scaling. Without this
    // clamp a settled root turns the 50ms the engine is guaranteed on a spent
    // clock into 30ms, which is the one situation where the margin is being
    // relied on.
    return std::max(kMinSearchTime, soft * factor);
}

double SearchEngine::estimate_max_time(position& p) const {
    return estimate_time_budget(limits_, p.to_move() == white).hard;
}

// ─── Iterative deepening ────────────────────────────────────────────────────

void SearchEngine::iterative_deepening(position& p, U16 depth, bool silent, int thread_id) {
    int alpha = score::kNegInf;
    int beta = score::kInf;
    const int baseDelta = params_.aspiration_delta;
    int delta = baseDelta;
    int smallDelta = params_.aspiration_reseed_delta;
    int eval = score::kNegInf;
    // The score of the last *completed* iteration. The aspiration window has to
    // be anchored on this and not on `eval`, because between the two `eval`
    // holds the value a failed search returned, and that is a bound, not an
    // estimate of the score.
    int last_completed = score::kNegInf;

    if (params_.fixed_depth > 0)
        depth = static_cast<U16>(params_.fixed_depth);

    constexpr unsigned stack_size = MAX_PLY + 4;
    // search() and qsearch() bail out at ply >= MAX_PLY, and the deepest node
    // they can still enter before that check fires sits at index MAX_PLY + 1
    // (the root is placed at index 2 and holds ply 1). Anything smaller would
    // let the recursion write past the end of this array.
    static_assert(stack_size >= MAX_PLY + 2,
                  "search stack must hold every ply the MAX_PLY guard admits");
    SearchNode stack[stack_size];
    Move pv_line[MAX_PLY + 4];

    (stack + 2)->pv = pv_line;

    bool is_main = (thread_id == 0);

    // Best move of the previous completed iteration, and how many completed
    // iterations in a row have agreed on it. Only the main thread budgets time,
    // so only the main thread tracks this.
    Move prev_best{};
    int stable_iterations = 0;

    for (unsigned id = 1 + static_cast<unsigned>(thread_id); id <= depth; ++id) {
        if (signals_.stop.load())
            break;

        (stack + 0)->ply = (stack + 1)->ply = (stack + 2)->ply = 0;

        // Carry the last iteration's scores forward. Every root move that fails
        // low this iteration has its score reset to -inf, so without this they
        // would all compare equal and the ordering the previous iteration
        // established would survive only as an accident of stable_sort. The
        // comparator has always read prevScore; nothing ever wrote it.
        for (auto& rm : p.root_moves)
            rm.prevScore = rm.score;

        // Reset the widening step for every iteration. This was declared once
        // outside the loop, so a depth that needed several re-searches left
        // `delta` permanently enlarged for every depth that followed. Measured
        // over bench 12 (144 iterations): mean delta at iteration start 91.3
        // against a base of 65, peaking at 306.
        delta = baseDelta;

        // Only aspirate once a completed iteration has produced a score to
        // aspirate around. The old guard was `id >= 2`, which is true on the
        // *first* iteration of every helper thread, since thread n starts at
        // depth n + 1. Those threads then built their window out of the initial
        // eval of -inf and searched [-inf, -inf + 33], which fails high on
        // essentially any position.
        if (last_completed != score::kNegInf) {
            alpha = std::max(last_completed - smallDelta, static_cast<int>(score::kNegInf));
            beta = std::min(last_completed + smallDelta, static_cast<int>(score::kInf));
        } else {
            alpha = score::kNegInf;
            beta = score::kInf;
        }

        // Aspiration window search
        while (true) {
            p.sel_depth = 0;
            eval = search<root>(p, alpha, beta, static_cast<U16>(id), stack + 2, thread_id);

            // The sort deliberately runs before the stop check, so an
            // aborted iteration still reorders the root list and can change
            // the move that is played. That looks unsafe -- it compares this
            // iteration's scores against the previous iteration's for the
            // moves time ran out on -- but it is not reachable in practice.
            // Only root moves that raise alpha are given a real score; every
            // other move is set to kNegInf, so after a completed iteration
            // the list is one scored move followed by a run of kNegInf and a
            // partial iteration has nothing to promote. Measured over 59
            // aborted iterations (12 bench positions x 5 time budgets from
            // 150 ms to 900 ms): the abort changed root_moves[0] zero times.
            // Left alone; if root scoring ever changes so that several moves
            // keep real scores, this becomes live and needs the stop check
            // moved above the sort.
            std::stable_sort(p.root_moves.begin(), p.root_moves.end());

            if (signals_.stop.load())
                break;

            if (is_main && !silent && (eval <= alpha || eval >= beta))
                readout_pv(stack, p.root_moves, eval, alpha, beta, static_cast<U16>(id));

            // Widen only the bound that actually failed, and leave the other
            // one where it is. The old code recomputed *both* bounds from
            // `eval` on every re-search, which re-centred the window on the
            // score of the search that had just failed. On a fail low that
            // score is only an upper bound and under fail-soft it can sit far
            // below alpha, so beta was dragged down with it -- sometimes below
            // the alpha we had just failed under -- and the re-search then
            // failed high against a window that had moved past the score in
            // the opposite direction.
            if (eval <= alpha) {
                alpha = std::max(eval - delta, static_cast<int>(score::kNegInf));
                delta += delta / 4;
            } else if (eval >= beta) {
                beta = std::min(eval + delta, static_cast<int>(score::kInf));
                delta += delta / 4;
            } else {
                break;
            }
        }

        if (!signals_.stop.load()) {
            last_completed = eval;
            if (thread_id < static_cast<int>(completed_depth_.size()))
                completed_depth_[thread_id] = static_cast<int>(id);
        }

        // Print PV
        if (is_main && !signals_.stop.load()) {
            if (!silent)
                readout_pv(stack, p.root_moves, eval, alpha, beta, static_cast<U16>(id));

            if (id == depth) {
                signals_.stop = true;
                break;
            }

            // "go mate N" is answered as soon as a mate at most N moves away
            // is proven; deepening past it only re-proves the same thing.
            if (limits_.mate > 0 && eval >= score::kMateMaxPly &&
                (score::kMate - eval + 1) / 2 <= limits_.mate) {
                signals_.stop = true;
                break;
            }

            // Decide whether another iteration is affordable. The old loop
            // simply started one and let the timer cut it off partway, which
            // spends the whole budget on every move however obvious the move
            // is: an aborted iteration contributes nothing, so that time is
            // burnt rather than banked.
            //
            // The budget is scaled by how settled the root is. A best move that
            // has survived several iterations is unlikely to be overturned by
            // one more, so the search leaves early and the saved milliseconds
            // go back on the clock for a move that needs them; a root that is
            // still changing its mind buys extra time, bounded by the hard
            // limit the timer thread enforces.
            // `soft_limit_` is loaded with acquire and `time_origin_` after it,
            // so a fresh limit is never paired with a stale origin -- see
            // ponder_hit(), which publishes the two together.
            const double soft = soft_limit_.load(std::memory_order_acquire);
            if (soft > 0.0 && !p.root_moves.empty()) {
                const Move best = p.root_moves[0].pv[0];
                stable_iterations = (best == prev_best) ? stable_iterations + 1 : 0;
                prev_best = best;

                const double target = soft_time_target(soft, stable_iterations);
                const double origin = time_origin_.load(std::memory_order_relaxed);
                if (search_clock_.elapsed_ms() - origin >= target) {
                    signals_.stop = true;
                    break;
                }
            }
        }
    }
}

// ─── Main search ────────────────────────────────────────────────────────────

template <Nodetype type>
int SearchEngine::search(position& pos, int alpha, int beta, U16 depth, SearchNode* stack,
                         int thread_id) {
    if (signals_.stop.load())
        return score::kDraw;

    // "go nodes N" was parsed and then never enforced, so it searched forever.
    // The count is checked here rather than on the timer thread because a
    // node limit is meant to be reproducible: a deadline that depends on how
    // fast the machine happens to be would defeat the point of asking for one.
    // The 1023-node stride keeps the cross-thread sum off the hot path while
    // staying exact enough to be worth asking for.
    const U64 node_limit = node_limit_.load(std::memory_order_relaxed);
    if (node_limit > 0 && (pos.nodes() & 1023) == 0 && total_nodes() >= node_limit) {
        signals_.stop = true;
        return score::kDraw;
    }

    assert(alpha < beta);

    int bestScore = score::kNegInf;
    Move best_move{};
    best_move.type = static_cast<U8>(no_type);

    Move ttm{};
    ttm.type = static_cast<U8>(no_type);
    Move pv_line[MAX_PLY + 4];
    int ttvalue = score::kNegInf;
    U8 tt_depth = 0;
    U8 tt_bound = static_cast<U8>(no_bound);

    bool in_check = pos.in_check();
    std::vector<Move> quiets;
    stack->in_check = in_check;
    stack->ply = (stack - 1)->ply + 1;

    U16 root_dist = stack->ply;
    const bool root_node = (type == Nodetype::root && stack->ply == 1);
    // A node is a PV node only if it is actually searched with a window wider
    // than one, not merely because it was reached through the Nodetype::pv
    // template argument.
    //
    // The move loop hands Nodetype::pv to its first two moves (the PVS
    // full-window threshold below). At a genuine PV node that is right: the
    // window really is full. At a node that is itself being searched with a
    // null window, -beta and -alpha are still a null window, so the child gets
    // the PV type and a null window at the same time. Counting the bench tree:
    //
    //     PV type, full window     20,857
    //     PV type, null window    146,330
    //     non-PV                  343,243
    //
    // 87% of the nodes calling themselves PV nodes were null-window searches.
    // They were denied the TT cutoff below, which is gated on !pvNode, and were
    // given PV reductions by reduction(pvNode, ...). Neither is defensible for
    // a search that can only ever return a bound.
    const bool pvNode = (root_node || (type == Nodetype::pv && beta - alpha > 1));

    const Move excluded_move = stack->excluded_move;
    const bool singular_search = !excluded_move.is_null();

    // threat_move records a threat that *this* node's null move search found:
    // the opponent's refutation, used to order and extend replies to it. The
    // stack slot is shared by every node at this ply, and nothing ever cleared
    // it, so a node that ran no null move search read whatever some unrelated
    // sibling subtree happened to leave there. A singular verification search
    // is excluded because it re-searches this same position on this same stack
    // entry, and the threat found here is still the right one.
    if (!singular_search)
        stack->threat_move = Move{};

    // Every thread tracks its own selective depth on its own position, so
    // this is no longer gated on being the main thread: each one is now
    // reporting about itself rather than racing on one shared counter.
    if (pvNode && pos.sel_depth < stack->ply + 1)
        pos.sel_depth++;

    // The search stack is a fixed MAX_PLY + 4 array and every recursion steps
    // one node forward, so the tree has to be cut off before it can run off the
    // end. Nothing else bounds it: extensions can push the real ply past the
    // nominal depth, and qsearch recurses on captures and check evasions with no
    // depth counter at all. Return a static verdict rather than recursing.
    if (stack->ply >= MAX_PLY)
        return in_check ? score::kDraw : static_eval(pos, thread_id);

    // Repetition and the fifty move rule apply whether or not the side to move
    // is in check. This used to carry a !in_check guard, which hid every draw
    // that arrives while in check -- perpetual check above all, the single most
    // common way a lost game is saved. Over 40 self play games the guard
    // suppressed 48036 positions that is_draw() called drawn, roughly 1200 a
    // game; each was searched on as though the game continued.
    if (!root_node && pos.is_draw() && (!in_check || has_legal_move(pos)))
        return score::kDraw;

    { // mate distance pruning
        int mating_score = score::kMate - root_dist;
        beta = std::min(mating_score, beta);
        if (alpha >= mating_score)
            return mating_score;

        int mated_score = score::kMated + root_dist;
        alpha = std::max(mated_score, alpha);
        if (beta <= mated_score)
            return mated_score;
    }

    // The window this node is actually searched with, captured after mate
    // distance pruning has narrowed it. alpha is raised in the move loop as
    // the PV improves, so by the time the bound is classified it no longer
    // says what the node was asked to beat; that is what this is for.
    const int alpha_orig = alpha;

    // TT lookup. Skipped during a singular verification search: the stored
    // entry describes the position including the move we are excluding.
    bool hashHit = false;
    if (!singular_search) {
        hash_data e;
        hashHit = tt_.fetch(pos.key(), e);
        if (hashHit) {
            ttm = e.move;
            ttvalue = score_from_tt(e.score, root_dist);
            tt_depth = e.depth;
            tt_bound = e.bound;

            if (!pvNode && e.depth >= depth) {
                if ((e.bound == bound_exact) || (e.bound == bound_low && ttvalue >= beta) ||
                    (e.bound == bound_high && ttvalue <= alpha)) {
                    // Note: this deliberately also fires on an upper-bound
                    // entry, where every move failed low and e.move refuted
                    // nothing. That looks wrong and was tried as a fix;
                    // restricting the update to genuine fail-highs costs 40%
                    // more nodes at fixed depth (bench 770124 -> 1080883).
                    // e.move is still the best move known for this position,
                    // and in TT-heavy regions this is the only thing that
                    // populates killers and countermoves at all -- nodes that
                    // return here never reach a move loop. Leave it.
                    thread_history(thread_id).update(pos.to_move(), ttm, (stack - 1)->curr_move,
                                    history_bonus(depth), static_cast<int16_t>(ttvalue), quiets,
                                    stack->killers);
                    thread_history(thread_id).update_continuation(pos, stack, ttm, history_bonus(depth), quiets);
                    return ttvalue;
                }
            }
        }
    }

    // Static evaluation
    const bool anyPawnsOn7th = pos.pawns_near_promotion();
    const bool weHavePawnsOn7th = pos.pawns_on_7th();
    (void)weHavePawnsOn7th;

    int16_t static_eval_val;
    if (in_check) {
        static_eval_val = score::kNegInf;
    } else {
        auto* sthread = search_threads_[thread_id];
        static_eval_val = static_cast<int16_t>(std::lround(sthread->evaluator->evaluate(pos)));

        // Correction history adjusts the raw evaluation, before the TT gets a
        // say. The TT value has a real search behind it and needs no help; the
        // point of the correction is to improve the estimate we fall back on
        // when there is no such value, and to give the update below a corrected
        // baseline so the table converges instead of chasing its own output.
        static_eval_val = static_cast<int16_t>(std::clamp(
            static_eval_val + thread_history(thread_id).correction(pos.to_move(), pos.pawnkey()),
            static_cast<int>(-score::kMate + 100), static_cast<int>(score::kMate - 100)));
    }
    const int16_t corrected_static_eval = static_eval_val;

    // A TT score is a better estimate of the position than a static evaluation
    // -- it has a search behind it -- but only in the direction its bound
    // supports. bound_low says the truth is at least ttvalue, so it may raise
    // the estimate and must not lower it; bound_high says the truth is at most
    // ttvalue, so the reverse. Only bound_exact may do both.
    //
    // This used to be an unconditional `if (ttvalue != kNegInf) static_eval =
    // ttvalue`, which let an upper bound stand in for the evaluation and then
    // fed it to reverse futility pruning and null move pruning, both of which
    // cut on static_eval >= beta and neither of which can tell that the number
    // is only a ceiling.
    if (static_eval_val != score::kNegInf && ttvalue != score::kNegInf &&
        (tt_bound == bound_exact || (tt_bound == bound_low && ttvalue > static_eval_val) ||
         (tt_bound == bound_high && ttvalue < static_eval_val))) {
        static_eval_val = static_cast<int16_t>(ttvalue);
    }

    stack->static_eval = static_eval_val;
    bool hasStaticValue = static_eval_val != score::kNegInf;

    // IIR: If we have no hash move at a PV or cut node, reduce depth.
    if (!singular_search && depth >= params_.iir_min_depth &&
        ttm.type == static_cast<U8>(no_type) &&
        (pvNode || (!pvNode && static_eval_val + params_.iir_cut_margin >= beta))) {
        depth -= 1;
    }

    // Reverse futility pruning: if our static eval is so good that even after
    // subtracting a margin we still beat beta, just return the static eval.
    if (!in_check && !pvNode && depth <= params_.rfp_max_depth && !stack->null_search &&
        !singular_search && static_eval_val - params_.rfp_margin * depth >= beta &&
        static_eval_val < score::kMate - 100) {
        return static_eval_val;
    }

    // Forward pruning conditions. `null_search` already prevents two null moves
    // in a row, which is the only ordering restriction null-move pruning needs.
    const bool forward_prune = (!in_check && !pvNode && !stack->null_search && !singular_search &&
                                std::abs(alpha - beta) == 1 && hasStaticValue);

    // Null move pruning
    bool null_move_allowed =
        (pos.to_move() == white ? pos.non_pawn_material<white>() : pos.non_pawn_material<black>());

    if (forward_prune && null_move_allowed && depth >= params_.nmp_min_depth &&
        static_eval_val >= beta) {

        // Reduce more when the static eval is far above beta, since the null
        // move is then more likely to hold.
        int R = params_.nmp_base_r + static_cast<int>(depth) / params_.nmp_depth_div +
                std::min(params_.nmp_eval_max, (static_eval_val - beta) / params_.nmp_eval_div);
        int ndepth = std::max(0, static_cast<int>(depth) - R);

        (stack + 1)->null_search = true;
        // The child only writes best_move if it reaches its move loop. A
        // transposition cutoff, a reverse futility return, a draw or a null
        // cutoff all return earlier and leave whatever an unrelated subtree put
        // in the slot, so clear it: a threat should only be adopted when this
        // null search actually produced one.
        (stack + 1)->best_move = Move{};
        // A null move is not a move, but this slot is what the child reads as
        // "the move my parent just played". Leaving the real move that was
        // searched here earlier in this node's own move loop makes the child
        // key its counter-move bonus, and every history term derived from the
        // predecessor, off a move that was never played on the board it is
        // looking at. Record the null explicitly.
        stack->curr_move = Move{};
        stack->push_context(no_piece, 0);
        pos.do_null_move();
        int null_eval =
            (ndepth <= 0 ? -qsearch<non_pv>(pos, -beta, -beta + 1, 0, stack + 1, thread_id)
                         : -search<non_pv>(pos, -beta, -beta + 1, static_cast<U16>(ndepth),
                                           stack + 1, thread_id));
        pos.undo_null_move();
        (stack + 1)->null_search = false;

        if (null_eval >= beta) {
            // A null move never proves a mate, so do not propagate mate scores.
            return null_eval >= score::kMateMaxPly ? beta : null_eval;
        } else {
            Move tm = (stack + 1)->best_move;
            if (tm.type == static_cast<U8>(capture) && beta - null_eval >= 500)
                stack->threat_move = tm;
        }
    }

    // ProbCut. If a capture, searched shallowly, already beats beta by a wide
    // margin, it is very likely that a full-depth search would also fail high,
    // so the whole node can be cut. The margin is what makes this safe: a
    // shallow search that only just reaches beta proves nothing, but one that
    // clears beta + margin has room for the shallow search to be wrong by a
    // piece and still be right about the cutoff.
    //
    // Only captures are tried. Quiet moves rarely swing a score by a margin
    // this large in a few plies, so searching them here would cost nodes
    // without producing cutoffs.
    const int probcut_beta = beta + params_.probcut_margin;
    if (!pvNode && !in_check && !singular_search && hasStaticValue &&
        depth >= params_.probcut_min_depth && std::abs(beta) < score::kMateMaxPly &&
        probcut_beta < score::kMateMaxPly &&
        // If the table already knows, from a search deep enough to be worth
        // more than the verification search below, that this node does not
        // reach probcut_beta, the verification cannot be trusted over it.
        !(ttvalue != score::kNegInf && tt_depth >= depth - params_.probcut_depth_reduction &&
          ttvalue < probcut_beta)) {

        // A capture can only reach probcut_beta if the material it wins covers
        // the gap between the static eval and that bound.
        const int see_threshold = probcut_beta - static_eval_val;
        const int probcut_depth = static_cast<int>(depth) - params_.probcut_depth_reduction;

        Moveorder pc_mvs(pos, ttm, stack, &thread_history(thread_id));
        Move pc_move;
        Move pc_prev = (stack - 1)->curr_move;
        Move pc_prevprev = (stack - 2)->curr_move;

        while (pc_mvs.next_move(pos, pc_move, pc_prev, pc_prevprev, stack->threat_move,
                                /*skipQuiets=*/true, /*rootMvs=*/false)) {
            if (signals_.stop.load())
                return score::kDraw;
            if (pc_move.type == static_cast<U8>(no_type) || pc_move == excluded_move)
                continue;
            // skipQuiets still lets the hash move and the killers through, and
            // those are usually quiet, so the capture test cannot be skipped.
            const bool pc_is_capture = (pc_move.type == static_cast<U8>(capture)) ||
                                       (pc_move.type == static_cast<U8>(ep)) ||
                                       pos.is_cap_promotion(static_cast<Movetype>(pc_move.type));
            if (!pc_is_capture || !pos.is_legal(pc_move))
                continue;
            if (pos.see(pc_move) < see_threshold)
                continue;

            stack->push_context(pos.piece_on(static_cast<Square>(pc_move.f)), pc_move.t);
            pos.do_move(pc_move);
            stack->curr_move = pc_move;

            // Qsearch first: it is nearly free and rejects most candidates, so
            // the expensive verification only runs on moves that survive it.
            int pc_score =
                -qsearch<non_pv>(pos, -probcut_beta, -probcut_beta + 1, 0, stack + 1, thread_id);
            if (pc_score >= probcut_beta && probcut_depth > 0) {
                pc_score = -search<non_pv>(pos, -probcut_beta, -probcut_beta + 1,
                                           static_cast<U16>(probcut_depth), stack + 1, thread_id);
            }
            pos.undo_move(pc_move);

            if (signals_.stop.load())
                return score::kDraw;

            if (pc_score >= probcut_beta) {
                // The verification searched probcut_depth plies below this
                // node, plus the one this move consumed, so the entry is worth
                // that much depth -- not `depth`, which was never searched.
                tt_.save(pos.key(), static_cast<U8>(std::max(0, probcut_depth) + 1),
                         static_cast<U8>(bound_low), pc_move, score_to_tt(pc_score, stack->ply),
                         pvNode);
                return pc_score;
            }
        }
    }

    // Main search
    U16 moves_searched = 0;
    Moveorder mvs(pos, ttm, stack, &thread_history(thread_id));
    Move move;
    Move pre_move = (stack - 1)->curr_move;
    Move pre_pre_move = (stack - 2)->curr_move;
    // `improving` asks whether our position got better over our own last two
    // turns. kNegInf is the in-check sentinel, not a number: subtracting it
    // makes any node whose grandparent was in check look like a huge
    // improvement. Test for it rather than doing arithmetic on it. When there
    // is nothing to compare against, default to improving, which is the
    // cautious answer -- it prunes and reduces less.
    bool improving;
    if (in_check)
        improving = false;
    else if ((stack - 2)->static_eval != score::kNegInf)
        improving = stack->static_eval >= (stack - 2)->static_eval;
    else
        improving = true;
    auto to_mv = pos.to_move();
    int SEE = 0;
    bool skipQuiets = false;
    bool rootMoves = root_node && pos.root_moves.size() > 4 && pos.root_moves[0].pv.size() > 4;

    while (mvs.next_move(pos, move, pre_move, pre_pre_move, stack->threat_move, skipQuiets,
                         rootMoves)) {
        if (signals_.stop.load())
            return score::kDraw;

        if (move.type == static_cast<U8>(no_type) || !pos.is_legal(move))
            continue;

        if (move == excluded_move)
            continue;

        // Move classification
        auto hashOrKiller = (move == ttm) || (move == stack->killers[0]) ||
                            (move == stack->killers[1]) || (move == stack->killers[2]) ||
                            (move == stack->killers[3]);
        auto isPromotion = pos.is_promotion(move.type);
        auto isCapture = (move.type == static_cast<U8>(capture)) ||
                         (move.type == static_cast<U8>(ep)) ||
                         pos.is_cap_promotion(static_cast<Movetype>(move.type));
        auto isQuiet = move.type == static_cast<U8>(Movetype::quiet);
        auto isEvasion = in_check;
        auto advancedPawnPush =
            (pos.piece_on(static_cast<Square>(move.f)) == Piece::pawn) &&
            (to_mv == white ? util::row(move.f) >= Row::r6 : util::row(move.f) <= Row::r3);
        auto dangerousQuietCheck = isQuiet && pos.quiet_gives_dangerous_check(move);

        // Futility pruning of quiet moves. If the static eval is so far below
        // alpha that even a generous allowance for what a quiet move can be
        // worth leaves it short, searching the rest of the quiets is unlikely
        // to raise alpha.
        //
        // The engine had the two *conditional* forms of this rule -- history
        // pruning, which needs a move with bad history, and move-count
        // pruning, which needs a high move index -- but not the plain
        // eval-based one, so a node 400cp behind at depth 2 still searched
        // every quiet with neutral history in the first few slots.
        //
        // skipQuiets is set rather than just skipping this move: the margin
        // tightens monotonically as the move index rises -- a later quiet is
        // reduced further, so its reduced depth is smaller and the condition
        // only gets easier to satisfy -- so once the rule rejects one quiet it
        // rejects every quiet after it, and telling the picker stops it
        // generating and scoring the rest. Excluded are moves that are not
        // really quiet in the sense this rule assumes -- checks, advanced pawn
        // pushes -- along with the hash move and killers, and the whole rule
        // is off while in check,
        // where every legal move is an evasion.
        //
        // The margin scales with the *reduced* depth, not the raw one. This
        // move, if searched, is very likely to be reduced -- that is what LMR
        // does to a late quiet -- so the raw depth overstates how much work it
        // is about to cost and therefore how large a margin is justified for
        // skipping it. Charging the raw depth was measured: it puts a 640cp
        // margin at depth 6 on a subtree that may really be searched at depth
        // 2, and loosening the margins from there to prune a comparable share
        // of the tree measured -15.6 +/- 19.1 over 794 games.
        const int lmr_depth =
            std::max(0, static_cast<int>(depth) -
                            static_cast<int>(reduction(pvNode, improving, depth, moves_searched)));
        if (!pvNode && !in_check && hasStaticValue && isQuiet && !hashOrKiller &&
            !dangerousQuietCheck && !advancedPawnPush && lmr_depth <= params_.fp_max_depth &&
            bestScore > score::kMatedMaxPly &&
            static_eval_val + params_.fp_base + params_.fp_margin * lmr_depth <= alpha) {
            skipQuiets = true;
            continue;
        }

        // History pruning: skip quiet moves with terrible history at shallow depths
        if (!pvNode && !in_check && depth <= params_.history_prune_depth && !hashOrKiller &&
            isQuiet && bestScore > score::kMatedMaxPly) {
            int hist_score = thread_history(thread_id).score(move, pos.to_move());
            if (hist_score < -params_.history_prune_margin * depth) {
                continue;
            }
        }

        // Skip captures with negative SEE
        if (isCapture && !hashOrKiller && !pvNode && !isEvasion && !isPromotion &&
            bestScore < alpha && depth <= params_.see_prune_depth && moves_searched > 1 &&
            (SEE = pos.see(move)) < 0)
            continue;

        // Skip quiet moves that hang material. The rule above covers captures
        // only, so a quiet move stepping a knight onto a square defended by a
        // pawn was searched at full width no matter how obviously it loses a
        // piece -- history pruning would only catch it once the same move had
        // already failed often enough to sink its history score, which is a
        // slow way to learn something SEE answers immediately.
        //
        // see() handles quiet moves correctly: with no victim the sequence
        // starts at zero and plays out the recaptures on the destination
        // square, so the return is exactly what the mover loses by going
        // there. The threshold scales with depth because the deeper the node
        // the more likely the search would have found the refutation anyway.
        if (!pvNode && !in_check && isQuiet && !hashOrKiller && !dangerousQuietCheck &&
            !advancedPawnPush && moves_searched > 1 &&
            depth <= params_.see_quiet_prune_depth && bestScore > score::kMatedMaxPly &&
            pos.see(move) < -params_.see_quiet_margin * depth)
            continue;

        // Singular extension: if the hash move is significantly better than
        // all alternatives, extend it by 1 ply. The alternatives are measured
        // by re-searching this node with the hash move excluded.
        int singular_ext = 0;
        if (!root_node && !singular_search && depth >= params_.singular_min_depth && move == ttm &&
            !stack->null_search && ttvalue != score::kNegInf &&
            (tt_bound == bound_low || tt_bound == bound_exact) && tt_depth >= depth - 3) {
            int singular_beta = ttvalue - params_.singular_margin * depth;
            int singular_depth = (depth - 1) / 2;

            const int16_t saved_static_eval = stack->static_eval;
            stack->excluded_move = ttm;
            int singular_score = search<non_pv>(pos, singular_beta - 1, singular_beta,
                                                static_cast<U16>(singular_depth), stack, thread_id);
            stack->excluded_move = Move{};
            stack->static_eval = saved_static_eval;

            if (singular_score < singular_beta) {
                singular_ext = 1;
            } else if (singular_beta >= beta) {
                return singular_beta; // Multi-cut
            }
        }

        stack->push_context(pos.piece_on(static_cast<Square>(move.f)), move.t);
        pos.do_move(move);
        stack->curr_move = move;

        bool givesCheck = pos.in_check();
        int extensions = std::max(givesCheck ? 1 : 0, singular_ext);
        // Extra reductions beyond the one ply every move already consumes.
        int reductions_val = 0;

        // Reduce uninteresting quiet moves
        if (!pvNode && !improving && !hashOrKiller && !isCapture && !isEvasion && !givesCheck &&
            !isPromotion && !advancedPawnPush && depth <= params_.lmr_extra_max_depth &&
            bestScore <= alpha)
            reductions_val += 1;

        // Extend likely interesting quiet moves
        auto threatResponse = (stack->threat_move.type != static_cast<U8>(no_type) &&
                               stack->threat_move.f == move.t) &&
                              isCapture;
        if (!pvNode && !improving && !hashOrKiller && !isCapture &&
            depth <= params_.quiet_ext_max_depth &&
            bestScore < alpha && bestScore > score::kMatedMaxPly &&
            (dangerousQuietCheck || advancedPawnPush || threatResponse))
            extensions += 1;

        // Movecount (late move) pruning. The guards matter as much as the rule:
        //
        //   !root_node -- futility_move_count(improving = false, depth = 1) is
        //               (6 + 1) / 2 = 3, so the depth-1 iteration from the
        //               start position searched three of twenty legal moves and
        //               discarded the rest. Every early iteration of iterative
        //               deepening was choosing from a truncated move list, and
        //               at short time controls those are sometimes the only
        //               iterations that finish.
        //   !in_check -- when in check every legal move is an evasion. The
        //               quiescence move picker already knows this and only
        //               generates quiets when in check, but the main move
        //               picker has no such special case: its quiet stage is
        //               where evasions arrive, and skipping it can discard the
        //               only escape.
        //   bestScore > kMatedMaxPly -- do not start discarding moves while the
        //               best score so far is still a forced mate against us.
        //               There may be nothing else to find and everything to
        //               lose.
        //
        // Not guarded on pvNode, matching Stockfish, which guards move count
        // pruning on rootNode only. (An earlier version of this comment argued
        // that !pvNode was unusable because Nodetype::pv leaked across most of
        // the tree. That leak is fixed -- pvNode is now derived from the window
        // -- so the argument no longer applies, but the conclusion is unchanged
        // and !pvNode remains untested here.)
        if (!root_node && !in_check && bestScore > score::kMatedMaxPly)
            // Compared as int. The cast to U16 truncated: futility_move_count
            // returns (futility_base + depth^2) / (2 - improving), and any
            // value that is a multiple of 65536 -- reachable by setting the
            // registered futility_base parameter, which is exactly what a tuner
            // or a UCI client is invited to do -- wrapped to 0 and turned "skip
            // no quiets" into "skip every quiet".
            skipQuiets = moves_searched >= futility_move_count(improving, depth);

        // Depth for the child node: one ply is always consumed here.
        int newdepth = static_cast<int>(depth) - 1 + extensions - reductions_val;
        (stack + 1)->pv = nullptr;

        int score_val = score::kNegInf;
        // PVS full-window threshold: give the first N moves a full window
        // before switching to null-window searches. Textbook PVS uses < 1. This
        // was < 3 to absorb re-searches caused by weak move ordering; with the
        // SEE rewrite, the capture-ordering scale fix and the dropped-quiets fix
        // the first move is now good enough often enough that the extra
        // full-window searches cost more than the re-searches they avoid.
        // Measured over the bench suite at fixed depth: < 3 = 3,075,308 nodes,
        // < 2 = 2,639,419, < 1 = 2,597,543. Kept at < 2, which captures nearly
        // all of the gain while still hedging one move.
        if (moves_searched < 2) {
            (stack + 1)->pv = pv_line;
            (stack + 1)->pv[0].set(A1, A1, no_type);
            score_val = (newdepth <= 0
                             ? -qsearch<Nodetype::pv>(pos, -beta, -alpha, 0, stack + 1, thread_id)
                             : -search<Nodetype::pv>(pos, -beta, -alpha,
                                                     static_cast<U16>(newdepth), stack + 1,
                                                     thread_id));
        } else {
            int LMR = newdepth;
            auto captureFollowup =
                (stack - 1)->curr_move.type == static_cast<U8>(capture) && isCapture;
            // Late move reduction
            if (!threatResponse && !hashOrKiller && !dangerousQuietCheck && !captureFollowup &&
                !advancedPawnPush && !isPromotion && !isEvasion && !givesCheck && !anyPawnsOn7th &&
                depth >= params_.lmr_min_depth && bestScore <= alpha) {
                unsigned R = reduction(pvNode, improving, depth, moves_searched);

                // Reduce more for moves with bad history
                int hist = thread_history(thread_id).score(move, to_mv);
                R += (hist < -params_.lmr_hist_bad) ? 1 : 0;

                // NOTE: non-PV nodes are *not* reduced further here. The
                // reduction table already encodes the cut-node penalty:
                // bitboards::reductions[0][..] is built as
                // bitboards::reductions[1][..] + 1. Adding another +1 at this
                // site applied the same idea twice, reducing non-PV moves by 2
                // plies more than PV moves instead of 1.

                // Reduce less for moves with good history
                if (hist > params_.lmr_hist_good)
                    R = std::max(0u, R - 1);

                // Don't reduce into qsearch
                LMR = std::max(1, newdepth - static_cast<int>(R));
            }

            score_val =
                (LMR <= 0 ? -qsearch<non_pv>(pos, -alpha - 1, -alpha, 0, stack + 1, thread_id)
                          : -search<non_pv>(pos, -alpha - 1, -alpha, static_cast<U16>(LMR),
                                            stack + 1, thread_id));

            if (score_val > alpha && (pvNode || LMR < newdepth)) {
                (stack + 1)->pv = pv_line;
                (stack + 1)->pv[0].set(A1, A1, no_type);

                score_val = (newdepth <= 0 ? -qsearch<Nodetype::pv>(pos, -beta, -alpha, 0,
                                                                    stack + 1, thread_id)
                                           : -search<Nodetype::pv>(pos, -beta, -alpha,
                                                                   static_cast<U16>(newdepth),
                                                                   stack + 1, thread_id));
            }
        }
        ++moves_searched;

        if (move.type == static_cast<U8>(Movetype::quiet))
            quiets.emplace_back(move);

        pos.undo_move(move);

        if (signals_.stop.load())
            return score::kDraw;

        // Root move update
        if (root_node) {
            auto it = std::find(pos.root_moves.begin(), pos.root_moves.end(), move);
            if (it != pos.root_moves.end()) {
                auto& rm = *it;
                if (moves_searched == 1 || score_val > alpha) {
                    rm.score = static_cast<int16_t>(score_val);
                    rm.selDepth = pos.sel_depth;
                    rm.pv.resize(1);
                    for (Move* m = (stack + 1)->pv; m; ++m) {
                        if (m->f == m->t || m->type == static_cast<U8>(no_type))
                            break;
                        rm.pv.push_back(*m);
                    }
                } else {
                    rm.score = score::kNegInf;
                }
            }
        }

        if (score_val > bestScore) {
            bestScore = score_val;
            best_move = move;
            stack->best_move = move;

            if (score_val >= beta) {
                thread_history(thread_id).update(pos.to_move(), best_move, (stack - 1)->curr_move,
                                history_bonus(depth), static_cast<int16_t>(bestScore), quiets,
                                stack->killers);
                thread_history(thread_id).update_continuation(pos, stack, best_move, history_bonus(depth), quiets);
                break;
            }

            if (score_val > alpha) {
                if (pvNode)
                    alpha = score_val;
                if (pvNode && !root_node)
                    update_pv(stack->pv, move, (stack + 1)->pv);
            }
        }
    }

    // Every quiet tried at a node that failed low is evidence against that
    // move. Without this the table only learns from beta cutoffs, and since a
    // cutoff node has tried a mean of 0.34 quiets before it cuts, the negative
    // side of the table never develops -- see docs/revisit-after-tuning.md for
    // the measured distribution and why history_malus_pct is still 0.
    //
    // The braces matter: malus_continuation sat outside the condition and ran
    // at every node, including the ones that cut. It is only harmless today
    // because history_malus_pct is 0, which makes the malus itself 0 -- so the
    // first thing an SPSA fit does when it raises that parameter is corrupt the
    // continuation tables at every cutoff node in the search.
    if (bestScore <= alpha_orig && !quiets.empty()) {
        const int malus = history_bonus(depth) * params_.history_malus_pct / 100;
        thread_history(thread_id).malus(to_mv, malus, quiets);
        thread_history(thread_id).malus_continuation(pos, stack, malus, quiets);
    }

    // Best move bonus
    if (bestScore >= alpha && bestScore < beta && best_move.f != best_move.t) {
        auto bonus = params_.best_move_bonus * depth;
        apply_history_bonus(stack->best_move_history()[to_mv][best_move.f][best_move.t], bonus);
    }

    if (moves_searched == 0) {
        // During a singular search the excluded move was skipped, so "no moves"
        // means "no alternatives", not stalemate or mate.
        if (singular_search)
            return alpha;
        return (in_check ? score::kMated + root_dist : score::kDraw);
    }

    // A score is exact only if it sits strictly inside the window the node was
    // searched with. best_move is not evidence of that here: it is assigned on
    // score_val > bestScore, and bestScore starts at -inf, so the first legal
    // move always sets it whether or not it beat alpha. (Stockfish tests the
    // equivalent of this flag, but only because it assigns bestMove inside its
    // value > alpha branch.) Testing it at a PV node therefore marked every
    // fail-low as bound_exact, storing an upper bound as though it were the
    // true score and letting later probes cut on it.
    Bound bound = (bestScore >= beta            ? bound_low
                   : pvNode && bestScore > alpha_orig ? bound_exact
                                                     : bound_high);

    // Feed the gap between what the evaluation guessed and what the search
    // found back into correction history. Only when the search result is
    // admissible evidence about the true score in the direction it differs:
    // a fail-low bounds the score from above, so it may only teach the table
    // that the evaluation was too high, and a fail-high only the reverse. A
    // capture or a check makes the difference tactical rather than a property
    // of the structure, which is what this table is keyed on.
    const bool best_is_capture =
        (best_move.type == static_cast<U8>(capture)) ||
        (best_move.type == static_cast<U8>(ep)) ||
        pos.is_cap_promotion(static_cast<Movetype>(best_move.type));

    if (!in_check && !singular_search && corrected_static_eval != score::kNegInf &&
        !best_is_capture && std::abs(bestScore) < score::kMate - 100 &&
        (bound == bound_exact || (bound == bound_low && bestScore > corrected_static_eval) ||
         (bound == bound_high && bestScore < corrected_static_eval))) {
        thread_history(thread_id).update_correction(to_mv, pos.pawnkey(), depth,
                                   bestScore - corrected_static_eval);
    }

    if (!singular_search)
        tt_.save(pos.key(), static_cast<U8>(depth), static_cast<U8>(bound), best_move,
                 score_to_tt(bestScore, stack->ply), pvNode);

    return bestScore;
}

// ─── Quiescence search ──────────────────────────────────────────────────────

// The remaining depth is not consulted: quiescence bounds itself by the ply
// guard and by which moves it is willing to generate, and it stores at a fixed
// depth. The parameter stays for signature parity with search().
template <Nodetype type>
int SearchEngine::qsearch(position& p, int alpha, int beta, U16 /*depth*/, SearchNode* stack,
                          int thread_id) {
    if (signals_.stop.load())
        return score::kDraw;

    int best_score = score::kNegInf;
    Move best_move{};
    best_move.type = static_cast<U8>(no_type);

    // See search(): alpha is raised by the stand pat and again in the move
    // loop, so the bound classification needs the window we were called with.
    const int alpha_orig = alpha;

    Move ttm{};
    ttm.type = static_cast<U8>(no_type);
    int ttvalue = score::kNegInf;
    // As in search(): the PV type is inherited from the caller's first two
    // moves, but the window may still be null, and a null-window qsearch can
    // only return a bound. Gating the TT cutoff and the bound classification on
    // the type alone treats those nodes as if they had produced a true score.
    bool pv_type = (type == Nodetype::pv && beta - alpha > 1);

    stack->ply = (stack - 1)->ply + 1;
    if (pv_type && p.sel_depth < stack->ply + 1)
        p.sel_depth++;
    U16 root_dist = stack->ply;

    bool in_check = p.in_check();
    stack->in_check = in_check;

    // qsearch runs no null move search, so it has no threat to report and must
    // not inherit one left in this slot by a search() node at the same ply.
    stack->threat_move = Move{};

    // See the matching guard in search(): qsearch has no depth counter, so a
    // long chain of captures and check evasions is bounded only by the stack.
    if (stack->ply >= MAX_PLY)
        return in_check ? score::kDraw : static_eval(p, thread_id);

    hash_data e;
    e.depth = 0;
    {
        if (tt_.fetch(p.key(), e)) {
            ttm = e.move;
            ttvalue = score_from_tt(e.score, root_dist);

            if (!pv_type) {
                if ((e.bound == bound_exact) || (e.bound == bound_low && ttvalue >= beta) ||
                    (e.bound == bound_high && ttvalue <= alpha)) {
                    return ttvalue;
                }
            }
        }
    }

    U16 qsdepth = in_check ? 1 : 0;
    const bool anyPawnsOn7th = p.pawns_near_promotion();

    if (!in_check) {
        auto* sthread = search_threads_[thread_id];
        best_score = static_cast<int>(std::lround(sthread->evaluator->evaluate(p)));

        // As in search(): a TT score has a search behind it and is the better
        // estimate, but only in the direction its bound supports. This was an
        // unconditional `best_score = ttvalue`, which let an upper bound become
        // the stand pat and then fail high against beta.
        if (ttvalue != score::kNegInf &&
            (e.bound == bound_exact || (e.bound == bound_low && ttvalue > best_score) ||
             (e.bound == bound_high && ttvalue < best_score)))
            best_score = ttvalue;

        // Stand pat
        if (best_score >= beta)
            return best_score;

        // Delta pruning.
        //
        // This returned `alpha`, which is a fail-hard return inside a fail-soft
        // search. Two things are wrong with that. It throws away what the node
        // actually knows -- best_score is below alpha - deltaCut, so it is a
        // strictly tighter and equally valid upper bound than alpha is. And it
        // makes the value a function of the window the node happened to be
        // searched with rather than of the position, so the same position
        // reached with a different window, or with a different move ordering
        // ahead of it, returns a different number.
        //
        // That second part is measurable: with an exact evaluation underneath
        // it, QuiescenceIsMirrorSymmetricOverRandomPlay finds a position and its
        // mirror scoring -18 and -19 at depth 1. Returning best_score makes all
        // 189 sampled positions symmetric again. The asymmetry is only hidden
        // today because the lazy evaluation cutoff above usually returns before
        // the difference can show.
        int deltaCut = params_.qs_delta_margin;
        if (anyPawnsOn7th)
            deltaCut += params_.qs_delta_pawn7th;
        if (best_score < alpha - deltaCut)
            return best_score;

        if (pv_type && alpha < best_score)
            alpha = best_score;
    }

    // Counted so that a pruning rule can never make a position with legal
    // evasions look like checkmate. Quiescence keeps no separate "moves tried"
    // counter: it has no late-move reduction or move-count pruning to feed one,
    // and the one it used to declare was incremented and never read.
    U16 legal_moves = 0;
    QMoveorder mvs(p, ttm, stack, &thread_history(thread_id));
    Move move;
    Move pre_move = (stack - 1)->curr_move;
    Move pre_pre_move = (stack - 2)->curr_move;

    while (mvs.next_move(p, move, pre_move, pre_pre_move, stack->threat_move, false)) {
        if (signals_.stop.load())
            return score::kDraw;

        if (move.type == static_cast<U8>(no_type) || !p.is_legal(move))
            continue;

        ++legal_moves;

        // qsearch never recorded the move it was searching, so a qsearch node
        // whose parent was also a qsearch node read whatever move some unrelated
        // subtree left at this ply and used it to key the counter-move bonus.
        stack->curr_move = move;
        stack->push_context(p.piece_on(static_cast<Square>(move.f)), move.t);

        auto hashOrKiller = (move == ttm) || (move == stack->killers[0]) ||
                            (move == stack->killers[1]) || (move == stack->killers[2]) ||
                            (move == stack->killers[3]);
        auto isQuiet = move.type == static_cast<U8>(Movetype::quiet);

        // Delta pruning for captures
        if (!isQuiet && !in_check && !hashOrKiller) {
            int idx = static_cast<int>(p.piece_on(static_cast<Square>(move.t)));
            float capture_score = 0;
            if (idx >= 0 && idx < static_cast<int>(kMaterialVals.size())) {
                if (move.type == static_cast<U8>(Movetype::capture))
                    capture_score = kMaterialVals[idx];
                else if (move.type == static_cast<U8>(ep))
                    capture_score = kMaterialVals[0];
                else if (move.type == static_cast<U8>(capture_promotion_q))
                    capture_score = kMaterialVals[idx] + kMaterialVals[queen];
                else if (move.type == static_cast<U8>(capture_promotion_r))
                    capture_score = kMaterialVals[idx] + kMaterialVals[rook];
                else if (move.type == static_cast<U8>(capture_promotion_b))
                    capture_score = kMaterialVals[idx] + kMaterialVals[bishop];
                else if (move.type == static_cast<U8>(capture_promotion_n))
                    capture_score = kMaterialVals[idx] + kMaterialVals[knight];
            }
            const int margin = params_.qs_capture_margin;
            if (capture_score > 0 &&
                (best_score + static_cast<int>(capture_score) + margin < alpha))
                continue;
            if (capture_score > 0 && (best_score - static_cast<int>(capture_score) - margin > beta))
                continue;
        }

        // Losing captures are not worth searching, but an evasion has to be
        // played whatever it costs, so never prune one.
        if (!in_check && p.see(move) < 0)
            continue;

        p.do_move(move);
        p.adjust_qnodes(1);

        int score_val = -qsearch<type>(p, -beta, -alpha, 0, stack + 1, thread_id);

        p.undo_move(move);

        if (score_val > best_score) {
            best_score = score_val;
            best_move = move;
            stack->best_move = move;

            if (score_val >= beta)
                break;

            if (pv_type && score_val > alpha)
                alpha = score_val;
        }
    }

    if (legal_moves == 0 && in_check)
        return score::kMated + root_dist;

    Bound bound = (best_score >= beta              ? bound_low
                   : pv_type && best_score > alpha_orig ? bound_exact
                                                       : bound_high);
    tt_.save(p.key(), static_cast<U8>(qsdepth), static_cast<U8>(bound), best_move,
             score_to_tt(best_score, stack->ply), pv_type);

    return best_score;
}

// ─── PV update ──────────────────────────────────────────────────────────────

void SearchEngine::update_pv(Move* root_pv, const Move& move, Move* child) {
    for (*root_pv++ = move; child && root_pv && child->f != child->t;)
        *root_pv++ = *child++;
    root_pv->set(A1, A1, no_type);
}

// ─── PV readout ─────────────────────────────────────────────────────────────

void SearchEngine::readout_pv(SearchNode* /*stack*/, const Rootmoves& mRoots, int eval, int alpha,
                              int beta, U16 depth) {
    std::unique_lock<std::mutex> lock(output_mutex_);

    U64 nodes = 0;
    for (auto& t : positions_) {
        nodes += t->nodes();
        nodes += t->qnodes();
    }

    // Without `time` a GUI cannot show think time and cannot derive nps, so it
    // has nothing to display while the engine is working. `hashfull` was
    // implemented and unit-tested but never actually reported to anyone.
    const double elapsed = search_clock_.elapsed_ms();
    const U64 elapsed_ms = static_cast<U64>(elapsed);
    const U64 nps = static_cast<U64>(static_cast<double>(nodes) * 1000.0 / std::max(1.0, elapsed));

    int numLines = std::min(multi_pv_, static_cast<int>(mRoots.size()));
    for (int i = 0; i < numLines; ++i) {
        if (i >= static_cast<int>(mRoots.size()))
            break;

        std::string res;
        for (auto& m : mRoots[i].pv) {
            if (m.f == m.t || m.type == static_cast<U8>(no_type))
                break;
            res += std::string(uci::move_to_string(m)) + " ";
        }

        // `pv` has to stay last: it is the one field of variable length, and a
        // GUI reading it consumes tokens until end of line.
        std::cout << "info"
                  << " depth " << depth
                  << " seldepth " << mRoots[i].selDepth
                  << " multipv " << (i + 1)
                  << " score " << uci_score_string(eval)
                  << (eval >= beta    ? " lowerbound"
                      : eval <= alpha ? " upperbound"
                                      : "")
                  << " nodes " << nodes
                  << " nps " << nps
                  << " hashfull " << tt_.hashfull()
                  << " time " << elapsed_ms
                  << " pv " << res << std::endl;
    }
}

// ─── Node counting ──────────────────────────────────────────────────────────

U64 SearchEngine::total_nodes() const {
    U64 n = 0;
    for (const auto& t : positions_)
        n += t->nodes() + t->qnodes();
    return n;
}

// ─── Explicit template instantiations ───────────────────────────────────────

template int SearchEngine::search<root>(position&, int, int, U16, SearchNode*, int);
template int SearchEngine::search<pv>(position&, int, int, U16, SearchNode*, int);
template int SearchEngine::search<non_pv>(position&, int, int, U16, SearchNode*, int);

template int SearchEngine::qsearch<pv>(position&, int, int, U16, SearchNode*, int);
template int SearchEngine::qsearch<non_pv>(position&, int, int, U16, SearchNode*, int);

} // namespace havoc
