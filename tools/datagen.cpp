/// @file datagen.cpp
#include "havoc/build_info.hpp"
#include "havoc/eval/nnue_evaluator.hpp"
#include "havoc/kpk.hpp"
/// @brief Training data generator: parallel self-play games → quiet position EPD file.

#include "havoc/bitboard.hpp"
#include "havoc/magics.hpp"
#include "havoc/movegen.hpp"
#include "havoc/position.hpp"
#include "havoc/search.hpp"
#include "havoc/uci.hpp"
#include "havoc/version.hpp"
#include "havoc/zobrist.hpp"

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <mutex>
#include <random>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

using namespace havoc;

struct DatagenPosition {
    std::string fen;
    double result;
    /// Search score at this position, in centipawns from the point of view of
    /// the side to move -- which is what the EPD `ce` opcode is defined to
    /// mean, and is deliberately NOT the convention `result` uses. Recorded
    /// alongside the result because a network trained on the result alone has
    /// to learn every position of a won game as equally won; the score is what
    /// tells it which of them were actually close.
    int score_stm;
};

static std::vector<Move> legal_moves(position& pos) {
    Movegen mvs(pos);
    mvs.generate<pseudo_legal, pieces>();
    std::vector<Move> legals;
    for (int i = 0; i < mvs.size(); ++i) {
        if (pos.is_legal(mvs[i]))
            legals.push_back(mvs[i]);
    }
    return legals;
}

/// A game either finished with a result, or it did not finish at all. Those
/// are different things and only one of them may be written to the file: a
/// game abandoned because the search returned nothing is not a draw, and
/// labelling it 0.5 teaches the network that every position in it was
/// balanced.
struct GameOutcome {
    bool complete = false;
    double result = 0.5;
};

/// Consecutive plies a side must be shown winning by `kAdjudicateCp` before
/// the game is called. A single ply is one search's opinion; the point of
/// requiring a run is that a horizon effect which resolves next ply cannot
/// mislabel every position in the game.
constexpr int kAdjudicateRun = 4;
constexpr int kAdjudicateCp = 5000;

static GameOutcome play_game(SearchEngine& engine, int depth, int random_plies,
                             std::vector<DatagenPosition>& positions, std::mt19937& rng) {
    std::string start_fen(uci::START_FEN);
    std::istringstream fen_stream(start_fen);
    position pos(fen_stream);

    int ply = 0;
    int adjudicate_run = 0;
    int adjudicate_sign = 0;

    while (ply < 500) {
        if (pos.is_draw())
            return {true, 0.5};

        auto legals = legal_moves(pos);
        if (legals.empty()) {
            if (!pos.in_check())
                return {true, 0.5}; // stalemate
            return {true, pos.to_move() == white ? 0.0 : 1.0};
        }

        Move best{};
        int score = 0;

        if (ply < random_plies) {
            std::uniform_int_distribution<int> dist(0, static_cast<int>(legals.size()) - 1);
            best = legals[dist(rng)];
        } else {
            SearchLimits lims{};
            lims.depth = static_cast<unsigned>(depth);
            engine.start(pos, lims, true);
            engine.wait();

            // Terminal positions are already handled above, so an empty root
            // list or an unplayable first move means the search failed rather
            // than that the game ended. Abandon the game instead of calling it
            // a draw -- the caller throws the positions away.
            if (pos.root_moves.empty() || pos.root_moves[0].pv.empty())
                return {false, 0.5};

            best = pos.root_moves[0].pv[0];
            score = pos.root_moves[0].score;

            // Adjudication is decided in White's frame so that a run survives
            // the alternation of the side to move.
            const int score_white = (pos.to_move() == white) ? score : -score;
            const int sign = (score_white > 0) - (score_white < 0);

            if (std::abs(score) >= score::kMateMaxPly) {
                // A mate score is a proof, not an estimate. No run required.
                return {true, score_white > 0 ? 1.0 : 0.0};
            }

            if (std::abs(score_white) > kAdjudicateCp) {
                adjudicate_run = (sign == adjudicate_sign) ? adjudicate_run + 1 : 1;
                adjudicate_sign = sign;
                if (adjudicate_run >= kAdjudicateRun)
                    return {true, sign > 0 ? 1.0 : 0.0};
            } else {
                adjudicate_run = 0;
                adjudicate_sign = 0;
            }

            bool is_quiet_move = (best.type == static_cast<U8>(quiet));
            if (ply >= 16 && !pos.in_check() && is_quiet_move && std::abs(score) < 3000) {
                positions.push_back({pos.to_fen(), 0.0, score});
            }
        }

        pos.do_move(best);
        ++ply;
    }

    // Ran out of plies without a result. That is not a draw either.
    return {false, 0.5};
}

/// A crash-resume point for a datagen run.
///
/// A run is hours long and occupies the whole machine, so losing one to a
/// reboot costs more than the run itself. What has to be restored is the exact
/// set of games already written: the file is shared by every thread and their
/// writes interleave, so a line count says nothing about which thread got how
/// far.
///
/// Two things make that recoverable. Each game seeds its own RNG from the
/// global game index, so a game is reproducible without replaying the ones
/// before it -- resuming is a matter of skipping indices, not fast-forwarding
/// a stream. And the checkpoint is written under the same mutex that guards
/// the EPD write, immediately after it, recording the file size at that
/// instant. Truncating back to that size on resume drops the tail of any
/// write that was in flight when the process died, so the data file and the
/// per-thread counts always agree.
///
/// Games buffered but not yet flushed are lost, which bounds the loss at
/// FLUSH_INTERVAL games per thread.
struct RunCheckpoint {
    // Identity of the run. A resume onto a different configuration would
    // silently produce a corpus that is neither of the two runs it came from.
    int games = 0;
    int depth = 0;
    int threads = 0;
    int random_plies = 0;
    // Not obviously part of a run's identity, but it is: at a fixed depth the
    // transposition table changes which moves the search finds, so resuming a
    // run under a different hash size continues it with a different engine.
    int hash_mb = 0;
    unsigned seed = 0;
    std::string eval_file;

    std::uintmax_t file_size = 0;
    uint64_t positions = 0;
    int abandoned = 0;
    std::vector<int> games_done_per_thread;
};

static std::string checkpoint_path(const std::string& output) {
    return output + ".progress";
}

/// Written under `file_mutex`, immediately after the EPD write it describes.
/// Via a temporary plus rename so that a crash during the checkpoint write
/// leaves the previous checkpoint intact rather than a half-written one.
static bool write_checkpoint(const std::string& output, const RunCheckpoint& cp) {
    const std::string path = checkpoint_path(output);
    const std::string tmp = path + ".tmp";
    {
        std::ofstream out(tmp, std::ios::trunc);
        out << "games " << cp.games << "\n"
            << "depth " << cp.depth << "\n"
            << "threads " << cp.threads << "\n"
            << "random_plies " << cp.random_plies << "\n"
            << "hash_mb " << cp.hash_mb << "\n"
            << "seed " << cp.seed << "\n"
            << "eval_file " << (cp.eval_file.empty() ? "-" : cp.eval_file) << "\n"
            << "file_size " << cp.file_size << "\n"
            << "positions " << cp.positions << "\n"
            << "abandoned " << cp.abandoned << "\n";
        for (size_t t = 0; t < cp.games_done_per_thread.size(); ++t)
            out << "thread " << t << " " << cp.games_done_per_thread[t] << "\n";
        if (!out)
            return false;
    }
    std::error_code ec;
    std::filesystem::rename(tmp, path, ec);
    return !ec;
}

static bool read_checkpoint(const std::string& output, RunCheckpoint& cp, std::string& err) {
    std::ifstream in(checkpoint_path(output));
    if (!in.is_open()) {
        err = "no checkpoint at " + checkpoint_path(output);
        return false;
    }
    std::map<int, int> per_thread;
    std::string key;
    while (in >> key) {
        if (key == "games") in >> cp.games;
        else if (key == "depth") in >> cp.depth;
        else if (key == "threads") in >> cp.threads;
        else if (key == "random_plies") in >> cp.random_plies;
        else if (key == "hash_mb") in >> cp.hash_mb;
        else if (key == "seed") in >> cp.seed;
        else if (key == "eval_file") { in >> cp.eval_file; if (cp.eval_file == "-") cp.eval_file.clear(); }
        else if (key == "file_size") in >> cp.file_size;
        else if (key == "positions") in >> cp.positions;
        else if (key == "abandoned") in >> cp.abandoned;
        else if (key == "thread") { int t = 0, n = 0; in >> t >> n; per_thread[t] = n; }
        else { err = "unrecognised key '" + key + "' in " + checkpoint_path(output); return false; }
        if (!in) { err = "truncated checkpoint " + checkpoint_path(output); return false; }
    }
    if (cp.threads <= 0 || static_cast<int>(per_thread.size()) != cp.threads) {
        err = "checkpoint records " + std::to_string(per_thread.size()) + " thread entries for "
              + std::to_string(cp.threads) + " threads";
        return false;
    }
    cp.games_done_per_thread.assign(static_cast<size_t>(cp.threads), 0);
    for (const auto& [t, n] : per_thread) {
        if (t < 0 || t >= cp.threads) { err = "checkpoint thread index out of range"; return false; }
        cp.games_done_per_thread[static_cast<size_t>(t)] = n;
    }
    return true;
}

/// Worker function: each thread plays its share of games, flushing to disk periodically.
static void worker(int thread_id, int first_game, int games_per_thread, int games_already_done,
                   int depth, int random_plies,
                   unsigned seed, int hash_mb, std::shared_ptr<const nnue::Network> net,
                   const std::string& output_file,
                   std::mutex& file_mutex, std::atomic<int>& games_done,
                   std::atomic<uint64_t>& total_positions, int total_games,
                   std::atomic<int>& games_abandoned, std::atomic<bool>& write_failed,
                   RunCheckpoint& checkpoint, std::atomic<bool>& checkpoint_failed) {
    SearchEngine engine;
    // Weights are immutable and shared; only the accumulators are per-thread,
    // so one loaded network serves every worker.
    if (net)
        engine.set_evaluator_factory(
            [net](Searchthread&) { return std::make_unique<NNUEEvaluator>(net); });
    if (!engine.set_hash_size(hash_mb)) {
        // Refused rather than clamped, so the engine is still holding its
        // default. Say so rather than report a size that is not in use.
        std::cerr << "datagen: hash size " << hash_mb << " MB refused, thread " << thread_id
                  << " keeps the engine default" << std::endl;
    }

    std::vector<DatagenPosition> buffer;
    constexpr int FLUSH_INTERVAL = 10; // flush every N games

    for (int g = games_already_done; g < games_per_thread; ++g) {
        // Seeded per game from the game's global index rather than once per
        // thread. Mixing the index in keeps neighbouring seeds uncorrelated --
        // the reason the old code mixed the thread id -- while making each
        // game reproducible on its own, which is what lets a resumed run skip
        // finished games instead of replaying them to advance the stream.
        std::seed_seq sequence{seed, static_cast<unsigned>(first_game + g)};
        std::mt19937 rng(sequence);

        // Once per game, not once per move. Games have to be independent of
        // each other, but within one game the table describes the tree the
        // next search is about to walk -- reusing it is what an engine does
        // between moves, and discarding it bought nothing. Clearing per move
        // memset the whole table plus a 2.4 MB continuation history before
        // every search on every thread, which at the 128 MB default cost more
        // than the searches it was interleaved with.
        engine.clear();

        std::vector<DatagenPosition> game_positions;
        GameOutcome outcome = play_game(engine, depth, random_plies, game_positions, rng);

        if (outcome.complete) {
            for (auto& p : game_positions) {
                p.result = outcome.result;
                buffer.push_back(p);
            }
        } else {
            ++games_abandoned;
        }

        int done = ++games_done;

        // Flush buffer to disk periodically
        if (g % FLUSH_INTERVAL == (FLUSH_INTERVAL - 1) || g == games_per_thread - 1) {
            std::lock_guard<std::mutex> lock(file_mutex);
            std::ofstream out(output_file, std::ios::app);
            for (const auto& p : buffer) {
                // `c9` stays first and keeps its exact spelling: readers split
                // the FEN off at " c9 ", so anything inserted ahead of it
                // lands inside the FEN. `ce` is the EPD opcode for a centipawn
                // evaluation and is defined from the point of view of the side
                // to move, which is the opposite convention to `c9` for half
                // the positions. That is the standard and readers expect it.
                out << p.fen << " c9 \"" << p.result << "\"; ce " << p.score_stm << ";\n";
            }
            out.flush();

            // A run that generates a hundred million positions will meet a
            // full disk. Counting the buffer as written and dropping it means
            // the tool reports a total it never stored, and the loss is
            // silent -- the one failure mode that cannot be noticed later.
            if (!out) {
                write_failed = true;
                std::cerr << "datagen: write to " << output_file << " failed, thread "
                          << thread_id << " stopping" << std::endl;
                return;
            }

            total_positions += buffer.size();
            buffer.clear();

            // Under the same lock and after the write it describes, so the
            // recorded size and the per-thread counts always describe the same
            // instant. A failure here is not fatal to the data -- it only
            // costs the ability to resume -- so it warns once and carries on.
            checkpoint.games_done_per_thread[static_cast<size_t>(thread_id)] = g + 1;
            checkpoint.positions = total_positions.load();
            checkpoint.abandoned = games_abandoned.load();
            std::error_code ec;
            checkpoint.file_size = std::filesystem::file_size(output_file, ec);
            if (!ec && !write_checkpoint(output_file, checkpoint)) {
                if (!checkpoint_failed.exchange(true))
                    std::cerr << "datagen: could not write " << checkpoint_path(output_file)
                              << "; this run will not be resumable" << std::endl;
            }

            if (done % 50 == 0 || done == total_games) {
                std::cout << "Progress: " << done << "/" << total_games
                          << "  total positions: " << total_positions.load()
                          << std::endl;
            }
        }
    }
}

static void print_usage(const char* prog) {
    std::cerr << "Usage: " << prog << " [options]\n"
              << "  --games N        Number of self-play games (default: 1000)\n"
              << "  --depth N        Search depth per move (default: 4)\n"
              << "  --threads N      Parallel game threads (default: CPU count)\n"
              << "  --random-plies N Random opening plies (default: 6)\n"
              << "  --hash N         Transposition table MB per thread (default: 16)\n"
              << "  --output FILE    Output EPD file (default: training_data.epd)\n"
              << "  --eval-file FILE Label with this NNUE network instead of the\n"
              << "                   handcrafted evaluation. A stronger evaluator makes a\n"
              << "                   depth-N label more accurate, which is what makes a\n"
              << "                   second training iteration worth running.\n"
              << "  --append         Append to an existing file. This adds games, it does\n"
              << "                   not resume a run: no game is skipped, so reusing the\n"
              << "                   same --seed regenerates the same games. Give each\n"
              << "                   shard its own seed.\n"
              << "  --resume         Continue an interrupted run from its checkpoint. The\n"
              << "                   other options must match the run being resumed; the\n"
              << "                   EPD is truncated back to the last checkpointed byte,\n"
              << "                   losing at most a handful of games per thread.\n"
              << "  --seed N         Random seed (default: time-based)\n";
}

int main(int argc, char* argv[]) {
    int num_games = 1000;
    int depth = 4;
    int num_threads = static_cast<int>(std::thread::hardware_concurrency());
    int random_plies = 6;
    // Per thread, so this is multiplied by --threads. The engine default of
    // 128 MB would reserve 3 GB across 24 threads to hold a depth 8 search
    // that never comes close to filling it.
    int hash_mb = 16;
    std::string output = "training_data.epd";
    std::string eval_file;
    bool append = false;
    bool resume = false;
    unsigned seed =
        static_cast<unsigned>(std::chrono::steady_clock::now().time_since_epoch().count());

    for (int i = 1; i < argc; ++i) {
        std::string key = argv[i];
        if ((key == "--games" || key == "-g") && i + 1 < argc)
            num_games = std::stoi(argv[++i]);
        else if ((key == "--depth" || key == "-d") && i + 1 < argc)
            depth = std::stoi(argv[++i]);
        else if ((key == "--threads" || key == "-t") && i + 1 < argc)
            num_threads = std::stoi(argv[++i]);
        else if (key == "--random-plies" && i + 1 < argc)
            random_plies = std::stoi(argv[++i]);
        else if (key == "--hash" && i + 1 < argc)
            hash_mb = std::stoi(argv[++i]);
        else if ((key == "--output" || key == "-o") && i + 1 < argc)
            output = argv[++i];
        else if (key == "--append" || key == "-a")
            append = true;
        else if (key == "--resume")
            resume = true;
        else if (key == "--eval-file" && i + 1 < argc)
            eval_file = argv[++i];
        else if (key == "--seed" && i + 1 < argc)
            seed = static_cast<unsigned>(std::stoul(argv[++i]));
        else if (key == "--help" || key == "-h") {
            print_usage(argv[0]);
            return 0;
        }
    }

    if (num_threads < 1) num_threads = 1;

    if (resume && append) {
        std::cerr << "datagen: --resume and --append do the opposite of each other. "
                     "--append adds a new shard, --resume continues an interrupted one."
                  << std::endl;
        return 1;
    }

    // A resumed run has to be the same run. Rather than trust the caller to
    // retype nine options identically, adopt them from the checkpoint and
    // refuse only when an explicitly supplied value contradicts it -- a silent
    // mismatch would produce a corpus that is neither of the two runs.
    RunCheckpoint resumed;
    if (resume) {
        std::string err;
        if (!read_checkpoint(output, resumed, err)) {
            std::cerr << "datagen: cannot resume: " << err << std::endl;
            return 1;
        }
        auto supplied = [&](const char* name) {
            for (int i = 1; i < argc; ++i)
                if (std::string(argv[i]) == name)
                    return true;
            return false;
        };
        struct Mismatch {
            const char* flag;
            const char* alias;
            std::string want;
            std::string got;
            bool differs;
        };
        const Mismatch checks[] = {
            {"--games", "-g", std::to_string(resumed.games), std::to_string(num_games),
             num_games != resumed.games},
            {"--depth", "-d", std::to_string(resumed.depth), std::to_string(depth),
             depth != resumed.depth},
            {"--threads", "-t", std::to_string(resumed.threads), std::to_string(num_threads),
             num_threads != resumed.threads},
            {"--random-plies", "", std::to_string(resumed.random_plies),
             std::to_string(random_plies), random_plies != resumed.random_plies},
            {"--hash", "", std::to_string(resumed.hash_mb), std::to_string(hash_mb),
             hash_mb != resumed.hash_mb},
            {"--seed", "", std::to_string(resumed.seed), std::to_string(seed),
             seed != resumed.seed},
            {"--eval-file", "", resumed.eval_file, eval_file, eval_file != resumed.eval_file},
        };
        for (const auto& c : checks) {
            if (c.differs && (supplied(c.flag) || (*c.alias && supplied(c.alias)))) {
                std::cerr << "datagen: cannot resume: checkpoint has " << c.flag << " "
                          << (c.want.empty() ? "(none)" : c.want) << ", command line says "
                          << (c.got.empty() ? "(none)" : c.got) << std::endl;
                return 1;
            }
        }
        num_games = resumed.games;
        depth = resumed.depth;
        num_threads = resumed.threads;
        random_plies = resumed.random_plies;
        hash_mb = resumed.hash_mb;
        seed = resumed.seed;
        eval_file = resumed.eval_file;
    }

    // Loaded once, before any game is played. A network that cannot be loaded
    // stops the run rather than quietly relabelling the whole corpus with the
    // handcrafted evaluation: the labels are the product, and which evaluator
    // produced them is not something a later reader can recover from the file.
    std::shared_ptr<const nnue::Network> net;
    if (!eval_file.empty()) {
        std::string err;
        net = load_network_file(eval_file, err);
        if (!net) {
            std::cerr << "datagen: " << err << std::endl;
            return 1;
        }
        std::cout << "Labelling with network " << eval_file << std::endl;
    }

    // Count existing positions if appending
    uint64_t existing_positions = 0;
    if (resume) {
        // Truncate back to the checkpointed size. Anything past it belongs to
        // a write that was in flight when the run died, and the per-thread
        // counts do not account for it -- keeping it would duplicate games the
        // resumed run is about to replay.
        std::error_code ec;
        const std::uintmax_t actual = std::filesystem::file_size(output, ec);
        if (ec) {
            std::cerr << "datagen: cannot resume: " << output << " is not readable"
                      << std::endl;
            return 1;
        }
        if (actual < resumed.file_size) {
            std::cerr << "datagen: cannot resume: " << output << " is " << actual
                      << " bytes but the checkpoint records " << resumed.file_size
                      << ". The data file has been truncated or replaced." << std::endl;
            return 1;
        }
        if (actual > resumed.file_size) {
            std::filesystem::resize_file(output, resumed.file_size, ec);
            if (ec) {
                std::cerr << "datagen: cannot resume: could not truncate " << output
                          << " to " << resumed.file_size << " bytes" << std::endl;
                return 1;
            }
            std::cout << "Discarded " << (actual - resumed.file_size)
                      << " unaccounted bytes past the checkpoint" << std::endl;
        }
        existing_positions = resumed.positions;
        int done = 0;
        for (int n : resumed.games_done_per_thread)
            done += n;
        std::cout << "Resuming " << output << ": " << done << "/" << num_games
                  << " games done, " << existing_positions << " positions kept" << std::endl;
    } else if (append) {
        std::ifstream check(output);
        if (check.is_open()) {
            std::string line;
            while (std::getline(check, line))
                if (!line.empty())
                    ++existing_positions;
            std::cout << "Appending to " << output << " (" << existing_positions
                      << " existing positions)" << std::endl;

            // A run killed mid-write leaves a partial final line. Appending
            // straight onto it splices two records into one unparseable row.
            check.clear();
            check.seekg(0, std::ios::end);
            if (check.tellg() > 0) {
                check.seekg(-1, std::ios::end);
                char last = 0;
                check.get(last);
                if (last != '\n') {
                    std::cerr << "datagen: " << output
                              << " does not end in a newline, so its last line is "
                                 "truncated. Refusing to append onto it." << std::endl;
                    return 1;
                }
            }
        }
    } else {
        // Truncate file
        std::ofstream(output, std::ios::trunc).close();
    }

    if (!resume) {
        // A checkpoint left by an earlier run describes byte offsets into a
        // file that no longer exists in that form. Remove it now rather than
        // let a later --resume trust it.
        std::error_code ec;
        std::filesystem::remove(checkpoint_path(output), ec);
    }

    std::cout << "haVoc datagen: " << num_games << " games, depth " << depth << ", "
              << num_threads << " threads, " << random_plies << " random plies, " << hash_mb
              << " MB hash/thread" << std::endl;

    bitboards::init();
    magics::init();
    zobrist::init();
    kpk::init();

    auto t0 = std::chrono::steady_clock::now();

    std::mutex file_mutex;
    std::atomic<int> games_done{0};
    std::atomic<uint64_t> total_positions{existing_positions};
    std::atomic<int> games_abandoned{0};
    std::atomic<bool> write_failed{false};
    std::atomic<bool> checkpoint_failed{false};

    RunCheckpoint checkpoint;
    checkpoint.games = num_games;
    checkpoint.depth = depth;
    checkpoint.threads = num_threads;
    checkpoint.random_plies = random_plies;
    checkpoint.hash_mb = hash_mb;
    checkpoint.seed = seed;
    checkpoint.eval_file = eval_file;
    checkpoint.games_done_per_thread.assign(static_cast<size_t>(num_threads), 0);
    if (resume) {
        checkpoint.games_done_per_thread = resumed.games_done_per_thread;
        games_done = 0;
        for (int n : resumed.games_done_per_thread)
            games_done += n;
        games_abandoned = resumed.abandoned;
    }

    // Distribute games across threads
    std::vector<std::thread> threads;
    int base = num_games / num_threads;
    int remainder = num_games % num_threads;

    int first_game = 0;
    for (int t = 0; t < num_threads; ++t) {
        int count = base + (t < remainder ? 1 : 0);
        int already = checkpoint.games_done_per_thread[static_cast<size_t>(t)];
        threads.emplace_back(worker, t, first_game, count, already, depth, random_plies, seed,
                             hash_mb, net, std::cref(output), std::ref(file_mutex),
                             std::ref(games_done), std::ref(total_positions), num_games,
                             std::ref(games_abandoned), std::ref(write_failed),
                             std::ref(checkpoint), std::ref(checkpoint_failed));
        first_game += count;
    }

    for (auto& t : threads)
        t.join();

    auto elapsed = std::chrono::steady_clock::now() - t0;
    double secs = std::chrono::duration<double>(elapsed).count();
    int games_this_run = num_games;
    if (resume)
        for (int n : resumed.games_done_per_thread)
            games_this_run -= n;
    double gps = games_this_run / secs;

    if (write_failed.load()) {
        std::cerr << "\ndatagen: FAILED. " << output
                  << " is incomplete -- some positions were generated but not stored."
                  << std::endl;
        if (!checkpoint_failed.load())
            std::cerr << "datagen: the checkpoint is intact; free space and rerun with "
                         "--resume to continue from it." << std::endl;
        return 1;
    }

    std::cout << "\nDone: " << total_positions.load() << " total positions in " << output
              << " (" << static_cast<int>(secs) << "s, " << static_cast<int>(gps)
              << " games/sec)" << std::endl;

    // A corpus that cannot be traced back to the evaluation that labelled it
    // is very hard to reason about later, and the binary is the only thing
    // that knows which commit it came from. Write it down next to the data.
    {
        std::ofstream meta(output + ".meta.json");
        meta << "{\n"
             << "  \"file\": \"" << output << "\",\n"
             << "  \"engine_commit\": \"" << havoc::GIT_COMMIT << "\",\n"
             << "  \"engine_tree_dirty\": " << (havoc::GIT_DIRTY ? "true" : "false") << ",\n"
             << "  \"engine_version\": \"" << havoc::VERSION_STRING << "\",\n"
             << "  \"provenance_grade\": \"self-reported\",\n"
             << "  \"label\": \"search d" << depth << "\",\n"
             << "  \"depth\": " << depth << ",\n"
             << "  \"games_requested\": " << num_games << ",\n"
             << "  \"games_abandoned\": " << games_abandoned.load() << ",\n"
             << "  \"positions\": " << total_positions.load() << ",\n"
             << "  \"threads\": " << num_threads << ",\n"
             << "  \"random_plies\": " << random_plies << ",\n"
             << "  \"seed\": " << seed << ",\n"
             << "  \"resumed\": " << (resume ? "true" : "false") << ",\n"
             << "  \"elapsed_seconds\": " << static_cast<int>(secs) << "\n"
             << "}\n";
        if (!meta)
            std::cerr << "datagen: could not write " << output << ".meta.json" << std::endl;
    }

    const int abandoned = games_abandoned.load();
    if (abandoned > 0) {
        // Not fatal, but it means the search returned nothing playable or a
        // game hit the ply cap. Those games are discarded rather than written
        // as draws, so say how many so that a systematic failure is visible.
        std::cout << "Abandoned " << abandoned << " of " << num_games
                  << " games with no result (positions discarded)" << std::endl;
    }

    // Only once the run has finished and the metadata is written. Until then
    // the checkpoint is the thing that makes an interrupted run recoverable,
    // so it outlives every failure path above.
    {
        std::error_code ec;
        std::filesystem::remove(checkpoint_path(output), ec);
    }

    return 0;
}

