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
    n = std::max(n, 1);
    search_threads_.init(n);
}

void SearchEngine::set_hash_size(int mb) {
    tt_.resize(static_cast<size_t>(mb));
}

void SearchEngine::clear() {
    tt_.clear();
    history_.clear();
}

void SearchEngine::load_params(const std::string& filename) {
    params_.load(filename);
    for (unsigned i = 0; i < search_threads_.size(); ++i) {
        search_threads_[i]->params = params_;
        // The pawn and material tables cache scores computed under the old
        // parameters. They are keyed by pawn/material structure, not by the
        // parameter set, so without an explicit clear the new values would be
        // masked by stale hits for the rest of the session.
        search_threads_[i]->pawn_tbl.clear();
        search_threads_[i]->material_tbl.clear();
        search_threads_[i]->evaluator = std::make_unique<HCEEvaluator>(
            search_threads_[i]->pawn_tbl, search_threads_[i]->material_tbl,
            search_threads_[i]->params);
    }
}

// ─── Pruning helpers ────────────────────────────────────────────────────────

unsigned SearchEngine::reduction(bool pv_node, bool improving, int d, int mc) {
    return bitboards::reductions[static_cast<int>(pv_node)][static_cast<int>(improving)]
                                [std::max(0, std::min(d, 63))][std::max(0, std::min(mc, 63))];
}

int SearchEngine::futility_move_count(bool improving, U16 depth) {
    return (6 + depth * depth) / (2 - static_cast<int>(improving));
}

/// Full static evaluation with lazy cutoffs disabled. Used where a score is
/// needed but the position cannot be searched any further.
int SearchEngine::static_eval(position& p, int thread_id) {
    auto* sthread = search_threads_[thread_id];
    return static_cast<int>(std::lround(sthread->evaluator->evaluate(p, -1.0f)));
}

float SearchEngine::lazy_eval_margin_search(int depth, bool advanced_pawn) {
    return advanced_pawn ? -1.0f : 225.0f * (1.0f - std::exp((depth - 64.0f) / 20.0f));
}

float SearchEngine::lazy_eval_margin(int depth, bool advanced_pawn) {
    return advanced_pawn ? -1.0f : 225.0f * (1.0f - std::exp((depth - 64.0f) / 20.0f));
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

    U16 depth = (lims.depth > 0 ? static_cast<U16>(lims.depth) : static_cast<U16>(MAX_PLY));
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

        if (!silent && !bestRoots.empty()) {
            std::cout << "bestmove " << uci::move_to_string(bestRoots[0].pv[0]);
            if (bestRoots[0].pv.size() > 1)
                std::cout << " ponder " << uci::move_to_string(bestRoots[0].pv[1]);
            std::cout << std::endl;
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

void SearchEngine::search_timer(position& p) {
    util::Clock c;
    bool fixed_time = limits_.movetime > 0;
    int delay = 1;
    double time_limit = estimate_max_time(p);
    auto sleep = [delay]() {
        std::this_thread::sleep_for(std::chrono::milliseconds(delay));
    };

    double elapsed = 0;
    if (fixed_time) {
        do {
            elapsed = c.elapsed_ms();
            sleep();
        } while (!signals_.stop.load() && searching_.load() &&
                 elapsed <= static_cast<double>(limits_.movetime));
    } else if (time_limit > -1) {
        do {
            elapsed = c.elapsed_ms();
            sleep();
        } while (!signals_.stop.load() && searching_.load() && elapsed <= time_limit);
    } else {
        do {
            elapsed = c.elapsed_ms();
            sleep();
        } while (!signals_.stop.load() && searching_.load());
    }
    signals_.stop = true;
}

double estimate_move_time(const SearchLimits& lims, bool white_to_move) {
    if (lims.infinite || lims.ponder || lims.depth > 0)
        return kNoTimeLimit;
    if (lims.movetime != 0)
        return static_cast<double>(lims.movetime);

    double our_time = (white_to_move ? lims.wtime : lims.btime);
    double our_inc = (white_to_move ? lims.winc : lims.binc);

    // "go" with no clock at all means there is nothing to budget against, so
    // search until the GUI stops us.
    const bool has_clock = lims.wtime > 0 || lims.btime > 0 || lims.winc > 0 || lims.binc > 0 ||
                           lims.movestogo > 0;
    if (!has_clock)
        return kNoTimeLimit;

    // Our clock is spent, or the GUI reported a negative value that the UCI
    // layer clamped to zero. Returning kNoTimeLimit here used to mean "no
    // limit", so the engine answered a flag-fall by thinking forever and
    // losing on time. Move as quickly as we can instead.
    if (our_time <= 0)
        return kMinSearchTime;

    // Sudden death: assume a fixed number of moves still to play.
    const double moves_left = (lims.movestogo > 0 ? static_cast<double>(lims.movestogo) : 25.0);

    const double base_time = our_time / moves_left + our_inc * 0.9;
    // Never commit more than a third of what is left to a single move.
    const double max_time = our_time * 0.33;

    return std::max(kMinSearchTime, std::min(base_time, max_time));
}

double SearchEngine::estimate_max_time(position& p) const {
    return estimate_move_time(limits_, p.to_move() == white);
}

// ─── Iterative deepening ────────────────────────────────────────────────────

void SearchEngine::iterative_deepening(position& p, U16 depth, bool silent, int thread_id) {
    int alpha = score::kNegInf;
    int beta = score::kInf;
    int delta = 65;
    int smallDelta = 33;
    int eval = score::kNegInf;

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

        bool failLow = false;
        bool failHigh = false;

        // Aspiration window search
        while (true) {
            if (id >= 2) {
                alpha = std::max(eval - smallDelta, static_cast<int>(score::kNegInf));
                beta = std::min(eval + smallDelta, static_cast<int>(score::kInf));
                if (failLow) {
                    beta = std::min(beta + delta, static_cast<int>(score::kInf));
                    failLow = false;
                }
                if (failHigh) {
                    alpha = std::max(alpha - delta, static_cast<int>(score::kNegInf));
                    failHigh = false;
                }
            }

            sel_depth_ = 0;
            eval = search<root>(p, alpha, beta, static_cast<U16>(id), stack + 2, thread_id);

            std::stable_sort(p.root_moves.begin(), p.root_moves.end());

            if (signals_.stop.load())
                break;

            if (is_main && !silent && (eval <= alpha || eval >= beta))
                readout_pv(stack, p.root_moves, eval, alpha, beta, static_cast<U16>(id));

            if (eval <= alpha) {
                delta += delta / 4;
                failHigh = true;
            } else if (eval >= beta) {
                delta += delta / 4;
                failLow = true;
            } else {
                break;
            }
        }

        if (!signals_.stop.load() && thread_id < static_cast<int>(completed_depth_.size()))
            completed_depth_[thread_id] = static_cast<int>(id);

        // Print PV
        if (is_main && !signals_.stop.load()) {
            if (!silent)
                readout_pv(stack, p.root_moves, eval, alpha, beta, static_cast<U16>(id));

            if (id == depth) {
                signals_.stop = true;
                break;
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
    bool is_main = (thread_id == 0);

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

    if (pvNode && sel_depth_.load() < stack->ply + 1 && is_main)
        sel_depth_++;

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
                    history_.update(pos.to_move(), ttm, (stack - 1)->curr_move, depth,
                                    static_cast<int16_t>(ttvalue), quiets, stack->killers);
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
        float lm = lazy_eval_margin_search(depth, anyPawnsOn7th);
        static_eval_val = static_cast<int16_t>(std::lround(sthread->evaluator->evaluate(pos, lm)));
    }

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
    if (!singular_search && depth >= 4 && ttm.type == static_cast<U8>(no_type) &&
        (pvNode || (!pvNode && static_eval_val + 200 >= beta))) {
        depth -= 1;
    }

    // Reverse futility pruning: if our static eval is so good that even after
    // subtracting a margin we still beat beta, just return the static eval.
    if (!in_check && !pvNode && depth <= 6 && !stack->null_search && !singular_search &&
        static_eval_val - 80 * depth >= beta && static_eval_val < score::kMate - 100) {
        return static_eval_val;
    }

    // Forward pruning conditions. `null_search` already prevents two null moves
    // in a row, which is the only ordering restriction null-move pruning needs.
    const bool forward_prune = (!in_check && !pvNode && !stack->null_search && !singular_search &&
                                std::abs(alpha - beta) == 1 && hasStaticValue);

    // Null move pruning
    bool null_move_allowed =
        (pos.to_move() == white ? pos.non_pawn_material<white>() : pos.non_pawn_material<black>());

    if (forward_prune && null_move_allowed && depth >= 3 && static_eval_val >= beta) {

        // Reduce more when the static eval is far above beta, since the null
        // move is then more likely to hold.
        int R = 3 + static_cast<int>(depth) / 6 + std::min(3, (static_eval_val - beta) / 200);
        int ndepth = std::max(0, static_cast<int>(depth) - R);

        (stack + 1)->null_search = true;
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

    // Main search
    U16 moves_searched = 0;
    Moveorder mvs(pos, ttm, stack, &history_);
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

        // History pruning: skip quiet moves with terrible history at shallow depths
        if (!pvNode && !in_check && depth <= 3 && !hashOrKiller && isQuiet &&
            bestScore > score::kMatedMaxPly) {
            int hist_score = history_.score(move, pos.to_move());
            if (hist_score < -4096 * depth) {
                continue;
            }
        }

        // Skip captures with negative SEE
        if (isCapture && !hashOrKiller && !pvNode && !isEvasion && !isPromotion &&
            bestScore < alpha && depth <= 1 && moves_searched > 1 && (SEE = pos.see(move)) < 0)
            continue;

        // Singular extension: if the hash move is significantly better than
        // all alternatives, extend it by 1 ply. The alternatives are measured
        // by re-searching this node with the hash move excluded.
        int singular_ext = 0;
        if (!root_node && !singular_search && depth >= 8 && move == ttm && !stack->null_search &&
            ttvalue != score::kNegInf && (tt_bound == bound_low || tt_bound == bound_exact) &&
            tt_depth >= depth - 3) {
            int singular_beta = ttvalue - 2 * depth;
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

        pos.do_move(move);
        stack->curr_move = move;

        bool givesCheck = pos.in_check();
        int extensions = std::max(givesCheck ? 1 : 0, singular_ext);
        // Extra reductions beyond the one ply every move already consumes.
        int reductions_val = 0;

        // Reduce uninteresting quiet moves
        if (!pvNode && !improving && !hashOrKiller && !isCapture && !isEvasion && !givesCheck &&
            !isPromotion && !advancedPawnPush && depth <= 2 && bestScore <= alpha)
            reductions_val += 1;

        // Extend likely interesting quiet moves
        auto threatResponse = (stack->threat_move.type != static_cast<U8>(no_type) &&
                               stack->threat_move.f == move.t) &&
                              isCapture;
        if (!pvNode && !improving && !hashOrKiller && !isCapture && depth <= 2 &&
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
            skipQuiets = moves_searched >= static_cast<U16>(futility_move_count(improving, depth));

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
                depth >= 3 && bestScore <= alpha) {
                unsigned R = reduction(pvNode, improving, depth, moves_searched);

                // Reduce more for moves with bad history
                int hist = history_.score(move, to_mv);
                R += (hist < -2000) ? 1 : 0;

                // NOTE: non-PV nodes are *not* reduced further here. The
                // reduction table already encodes the cut-node penalty:
                // bitboards::reductions[0][..] is built as
                // bitboards::reductions[1][..] + 1. Adding another +1 at this
                // site applied the same idea twice, reducing non-PV moves by 2
                // plies more than PV moves instead of 1.

                // Reduce less for moves with good history
                if (hist > 4000)
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
                    rm.selDepth = sel_depth_.load();
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
                history_.update(pos.to_move(), best_move, (stack - 1)->curr_move, depth,
                                static_cast<int16_t>(bestScore), quiets, stack->killers);
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

    // Best move bonus
    if (bestScore >= alpha && bestScore < beta && best_move.f != best_move.t) {
        auto bonus = 2 * depth;
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
    if (!singular_search)
        tt_.save(pos.key(), static_cast<U8>(depth), static_cast<U8>(bound), best_move,
                 score_to_tt(bestScore, stack->ply), pvNode);

    return bestScore;
}

// ─── Quiescence search ──────────────────────────────────────────────────────

template <Nodetype type>
int SearchEngine::qsearch(position& p, int alpha, int beta, U16 depth, SearchNode* stack,
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
    if (pv_type && sel_depth_.load() < stack->ply + 1)
        sel_depth_++;
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
        float lm = lazy_eval_margin(qsdepth, anyPawnsOn7th);
        best_score = static_cast<int>(std::lround(sthread->evaluator->evaluate(p, lm)));

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

        // Delta pruning
        int deltaCut = 910;
        if (anyPawnsOn7th)
            deltaCut += 775;
        if (best_score < alpha - deltaCut)
            return alpha;

        if (pv_type && alpha < best_score)
            alpha = best_score;
    }

    U16 moves_searched = 0;
    // Counted separately from moves_searched so that a pruning rule can never
    // make a position with legal evasions look like checkmate.
    U16 legal_moves = 0;
    QMoveorder mvs(p, ttm, stack, &history_);
    Move move;
    Move pre_move = (stack - 1)->curr_move;
    Move pre_pre_move = (stack - 2)->curr_move;
    Color to_mv = p.to_move();

    while (mvs.next_move(p, move, pre_move, pre_pre_move, stack->threat_move, false)) {
        if (signals_.stop.load())
            return score::kDraw;

        if (move.type == static_cast<U8>(no_type) || !p.is_legal(move))
            continue;

        ++legal_moves;

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
            int margin = 200;
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

        ++moves_searched;
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
    tt_.save(p.key(), qsdepth, static_cast<U8>(bound), best_move,
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

void SearchEngine::readout_pv(SearchNode* stack, const Rootmoves& mRoots, int eval, int alpha,
                              int beta, U16 depth) {
    std::unique_lock<std::mutex> lock(output_mutex_);

    U64 nodes = 0;
    for (auto& t : positions_) {
        nodes += t->nodes();
        nodes += t->qnodes();
    }

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

        std::cout << "info"
                  << " depth " << depth
                  << " seldepth " << mRoots[i].selDepth
                  << " multipv " << (i + 1)
                  << " score " << uci_score_string(eval)
                  << (eval >= beta    ? " lowerbound"
                      : eval <= alpha ? " upperbound"
                                      : "")
                  << " nodes " << nodes << " pv " << res << std::endl;
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
