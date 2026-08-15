/// @file datagen.cpp
#include "havoc/kpk.hpp"
/// @brief Training data generator: parallel self-play games → quiet position EPD file.

#include "havoc/bitboard.hpp"
#include "havoc/magics.hpp"
#include "havoc/movegen.hpp"
#include "havoc/position.hpp"
#include "havoc/search.hpp"
#include "havoc/uci.hpp"
#include "havoc/zobrist.hpp"

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <fstream>
#include <iostream>
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
    /// Search score at this position, in centipawns from White's point of
    /// view, so that it agrees in sign with `result`. Recorded alongside the
    /// game result because a network trained on the result alone has to learn
    /// every position of a won game as equally won; the search score is what
    /// tells it which of them were actually close.
    int score_white;
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

static double play_game(SearchEngine& engine, int depth, int random_plies,
                        std::vector<DatagenPosition>& positions, std::mt19937& rng) {
    std::string start_fen(uci::START_FEN);
    std::istringstream fen_stream(start_fen);
    position pos(fen_stream);

    int ply = 0;

    while (ply < 500) {
        if (pos.is_draw())
            return 0.5;

        auto legals = legal_moves(pos);
        if (legals.empty())
            return pos.in_check() ? (pos.to_move() == white ? 0.0 : 1.0) : 0.5;

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

            if (pos.root_moves.empty())
                return 0.5;

            // A root move with an empty PV carries no move to play. The search
            // itself guards this before printing "bestmove" -- checkmate and
            // stalemate leave the list either empty or unplayable -- and
            // indexing pv[0] here would read past the end of an empty vector.
            if (pos.root_moves[0].pv.empty())
                return 0.5;

            best = pos.root_moves[0].pv[0];
            score = pos.root_moves[0].score;

            if (std::abs(score) > 5000) {
                return score > 0 ? (pos.to_move() == white ? 1.0 : 0.0)
                                 : (pos.to_move() == white ? 0.0 : 1.0);
            }

            bool is_quiet_move = (best.type == static_cast<U8>(quiet));
            if (ply >= 16 && !pos.in_check() && is_quiet_move && std::abs(score) < 3000) {
                // The search reports from the side to move's point of view;
                // the game result is recorded from White's. Store both the
                // same way round or the two labels disagree on half the data.
                const int score_white = (pos.to_move() == white) ? score : -score;
                positions.push_back({pos.to_fen(), 0.0, score_white});
            }
        }

        pos.do_move(best);
        ++ply;
    }

    return 0.5;
}

/// Worker function: each thread plays its share of games, flushing to disk periodically.
static void worker(int thread_id, int games_per_thread, int depth, int random_plies,
                   unsigned seed, int hash_mb, const std::string& output_file,
                   std::mutex& file_mutex, std::atomic<int>& games_done,
                   std::atomic<uint64_t>& total_positions, int total_games) {
    // `seed + thread_id` makes two runs whose seeds differ by less than the
    // thread count generate overlapping streams -- with 8 threads, --seed 2
    // and --seed 5 share five of their eight -- which is exactly what sharding
    // a dataset across consecutive seeds would do. Mix the two instead so that
    // neighbouring seeds are uncorrelated, while a given seed still reproduces
    // its dataset exactly.
    std::seed_seq sequence{seed, static_cast<unsigned>(thread_id)};
    std::mt19937 rng(sequence);

    SearchEngine engine;
    if (!engine.set_hash_size(hash_mb)) {
        // Refused rather than clamped, so the engine is still holding its
        // default. Say so rather than report a size that is not in use.
        std::cerr << "datagen: hash size " << hash_mb << " MB refused, thread " << thread_id
                  << " keeps the engine default" << std::endl;
    }

    std::vector<DatagenPosition> buffer;
    constexpr int FLUSH_INTERVAL = 10; // flush every N games

    for (int g = 0; g < games_per_thread; ++g) {
        // Once per game, not once per move. Games have to be independent of
        // each other, but within one game the table describes the tree the
        // next search is about to walk -- reusing it is what an engine does
        // between moves, and discarding it bought nothing. Clearing per move
        // memset the whole table plus a 2.4 MB continuation history before
        // every search on every thread, which at the 128 MB default cost more
        // than the searches it was interleaved with.
        engine.clear();

        std::vector<DatagenPosition> game_positions;
        double result = play_game(engine, depth, random_plies, game_positions, rng);

        for (auto& p : game_positions) {
            p.result = result;
            buffer.push_back(p);
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
                // evaluation, and readers that do not want it ignore it.
                out << p.fen << " c9 \"" << p.result << "\"; ce " << p.score_white << ";\n";
            }
            total_positions += buffer.size();
            buffer.clear();

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
              << "  --append         Append to existing file (resume interrupted run)\n"
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
    bool append = false;
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
        else if (key == "--seed" && i + 1 < argc)
            seed = static_cast<unsigned>(std::stoul(argv[++i]));
        else if (key == "--help" || key == "-h") {
            print_usage(argv[0]);
            return 0;
        }
    }

    if (num_threads < 1) num_threads = 1;

    // Count existing positions if appending
    uint64_t existing_positions = 0;
    if (append) {
        std::ifstream check(output);
        if (check.is_open()) {
            std::string line;
            while (std::getline(check, line))
                if (!line.empty()) ++existing_positions;
            std::cout << "Appending to " << output << " (" << existing_positions
                      << " existing positions)" << std::endl;
        }
    } else {
        // Truncate file
        std::ofstream(output, std::ios::trunc).close();
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

    // Distribute games across threads
    std::vector<std::thread> threads;
    int base = num_games / num_threads;
    int remainder = num_games % num_threads;

    for (int t = 0; t < num_threads; ++t) {
        int count = base + (t < remainder ? 1 : 0);
        threads.emplace_back(worker, t, count, depth, random_plies, seed, hash_mb,
                             std::cref(output), std::ref(file_mutex),
                             std::ref(games_done), std::ref(total_positions), num_games);
    }

    for (auto& t : threads)
        t.join();

    auto elapsed = std::chrono::steady_clock::now() - t0;
    double secs = std::chrono::duration<double>(elapsed).count();
    double gps = num_games / secs;

    std::cout << "\nDone: " << total_positions.load() << " total positions in " << output
              << " (" << static_cast<int>(secs) << "s, " << static_cast<int>(gps)
              << " games/sec)" << std::endl;

    return 0;
}

