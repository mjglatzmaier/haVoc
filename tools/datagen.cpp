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

            best = pos.root_moves[0].pv[0];
            score = pos.root_moves[0].score;

            if (std::abs(score) > 5000) {
                return score > 0 ? (pos.to_move() == white ? 1.0 : 0.0)
                                 : (pos.to_move() == white ? 0.0 : 1.0);
            }

            bool is_quiet_move = (best.type == static_cast<U8>(quiet));
            if (ply >= 16 && !pos.in_check() && is_quiet_move && std::abs(score) < 3000) {
                positions.push_back({pos.to_fen(), 0.0});
            }
        }

        pos.do_move(best);
        ++ply;
        engine.clear();
    }

    return 0.5;
}

/// Worker function: each thread plays its share of games, flushing to disk periodically.
static void worker(int thread_id, int games_per_thread, int depth, int random_plies,
                   unsigned seed, const std::string& output_file,
                   std::mutex& file_mutex, std::atomic<int>& games_done,
                   std::atomic<uint64_t>& total_positions, int total_games) {
    std::mt19937 rng(seed + thread_id);
    SearchEngine engine;
    std::vector<DatagenPosition> buffer;
    constexpr int FLUSH_INTERVAL = 10; // flush every N games

    for (int g = 0; g < games_per_thread; ++g) {
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
                out << p.fen << " c9 \"" << p.result << "\";\n";
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
              << "  --output FILE    Output EPD file (default: training_data.epd)\n"
              << "  --append         Append to existing file (resume interrupted run)\n"
              << "  --seed N         Random seed (default: time-based)\n";
}

int main(int argc, char* argv[]) {
    int num_games = 1000;
    int depth = 4;
    int num_threads = static_cast<int>(std::thread::hardware_concurrency());
    int random_plies = 6;
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
              << num_threads << " threads, " << random_plies << " random plies" << std::endl;

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
        threads.emplace_back(worker, t, count, depth, random_plies, seed,
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

