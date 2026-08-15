/// @file pgn2epd.cpp
#include "havoc/kpk.hpp"
/// @brief Convert PGN files to EPD training data for Texel tuning.
/// Plays through each game, extracts quiet position FENs with game results.

#include "havoc/bitboard.hpp"
#include "havoc/magics.hpp"
#include "havoc/movegen.hpp"
#include "havoc/position.hpp"
#include "havoc/uci.hpp"
#include "havoc/zobrist.hpp"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

using namespace havoc;

/// Parse game result string to numeric value.
static double parse_result(const std::string& r) {
    if (r == "1-0") return 1.0;
    if (r == "0-1") return 0.0;
    if (r == "1/2-1/2") return 0.5;
    return -1.0; // unknown
}

/// Convert SAN move (e.g., "Nf3", "exd5", "O-O") to a legal move and play it.
static bool play_san_move(position& pos, const std::string& san) {
    // Strip check/mate indicators
    std::string s = san;
    while (!s.empty() && (s.back() == '+' || s.back() == '#' || s.back() == '!' || s.back() == '?'))
        s.pop_back();
    if (s.empty()) return false;

    // Castling
    if (s == "O-O" || s == "0-0") {
        Movegen mvs(pos);
        mvs.generate<pseudo_legal, pieces>();
        for (int i = 0; i < mvs.size(); ++i) {
            if (!pos.is_legal(mvs[i])) continue;
            if (mvs[i].type == static_cast<U8>(castle_ks)) {
                pos.do_move(mvs[i]);
                return true;
            }
        }
        return false;
    }
    if (s == "O-O-O" || s == "0-0-0") {
        Movegen mvs(pos);
        mvs.generate<pseudo_legal, pieces>();
        for (int i = 0; i < mvs.size(); ++i) {
            if (!pos.is_legal(mvs[i])) continue;
            if (mvs[i].type == static_cast<U8>(castle_qs)) {
                pos.do_move(mvs[i]);
                return true;
            }
        }
        return false;
    }

    // Parse SAN components
    Piece piece = pawn;
    int from_col = -1, from_row = -1;
    int to_col = -1, to_row = -1;
    Piece promo_piece = no_piece;

    size_t idx = 0;

    // Piece type
    if (idx < s.size() && std::isupper(s[idx]) && s[idx] != 'P') {
        switch (s[idx]) {
        case 'N': piece = knight; break;
        case 'B': piece = bishop; break;
        case 'R': piece = rook; break;
        case 'Q': piece = queen; break;
        case 'K': piece = king; break;
        default: return false;
        }
        ++idx;
    }

    // Collect remaining characters
    std::string rest = s.substr(idx);

    // Find promotion
    auto eq = rest.find('=');
    if (eq != std::string::npos && eq + 1 < rest.size()) {
        switch (rest[eq + 1]) {
        case 'Q': promo_piece = queen; break;
        case 'R': promo_piece = rook; break;
        case 'B': promo_piece = bishop; break;
        case 'N': promo_piece = knight; break;
        default: break;
        }
        rest = rest.substr(0, eq);
    }

    // Remove 'x' for captures
    rest.erase(std::remove(rest.begin(), rest.end(), 'x'), rest.end());

    // Parse destination (last two chars should be file+rank)
    if (rest.size() >= 2) {
        char fc = rest[rest.size() - 2];
        char fr = rest[rest.size() - 1];
        if (fc >= 'a' && fc <= 'h' && fr >= '1' && fr <= '8') {
            to_col = fc - 'a';
            to_row = fr - '1';
        }
        rest = rest.substr(0, rest.size() - 2);
    }

    // Disambiguation (remaining chars)
    for (char c : rest) {
        if (c >= 'a' && c <= 'h') from_col = c - 'a';
        else if (c >= '1' && c <= '8') from_row = c - '1';
    }

    if (to_col < 0 || to_row < 0) return false;
    Square to_sq = static_cast<Square>(to_row * 8 + to_col);

    // Find matching legal move
    Movegen mvs(pos);
    mvs.generate<pseudo_legal, pieces>();
    for (int i = 0; i < mvs.size(); ++i) {
        if (!pos.is_legal(mvs[i])) continue;

        Square from = static_cast<Square>(mvs[i].f);
        Square to = static_cast<Square>(mvs[i].t);

        if (to != to_sq) continue;
        if (pos.piece_on(from) != piece) continue;
        if (from_col >= 0 && (from % 8) != from_col) continue;
        if (from_row >= 0 && (from / 8) != from_row) continue;

        // Check promotion match
        if (promo_piece != no_piece) {
            auto mt = static_cast<Movetype>(mvs[i].type);
            Piece mp = no_piece;
            if (mt == promotion_q || mt == capture_promotion_q) mp = queen;
            else if (mt == promotion_r || mt == capture_promotion_r) mp = rook;
            else if (mt == promotion_b || mt == capture_promotion_b) mp = bishop;
            else if (mt == promotion_n || mt == capture_promotion_n) mp = knight;
            if (mp != promo_piece) continue;
        }

        pos.do_move(mvs[i]);
        return true;
    }

    return false;
}

struct PGNGame {
    std::string result;
    std::vector<std::string> moves;
    int white_elo = 0;
    int black_elo = 0;
};

/// Parse PGN file into a vector of games.
static std::vector<PGNGame> parse_pgn(const std::string& filename) {
    std::vector<PGNGame> games;
    std::ifstream in(filename);
    if (!in.is_open()) {
        std::cerr << "Cannot open " << filename << std::endl;
        return games;
    }

    PGNGame current;
    std::string moves_text;
    bool in_headers = true;
    std::string line;

    auto finish_game = [&]() {
        if (moves_text.empty()) return;

        // Tokenize moves
        std::istringstream ss(moves_text);
        std::string token;
        while (ss >> token) {
            // Skip move numbers, results, and comments
            if (token.find('.') != std::string::npos && std::isdigit(token[0])) {
                // Could be "1." or "1..." — extract move after dots
                auto dot = token.rfind('.');
                if (dot + 1 < token.size()) {
                    current.moves.push_back(token.substr(dot + 1));
                }
                continue;
            }
            if (token == "1-0" || token == "0-1" || token == "1/2-1/2" || token == "*")
                continue;
            if (token[0] == '{') {
                // Skip comment blocks
                while (ss >> token && token.back() != '}') {}
                continue;
            }
            if (token[0] == '(') continue; // skip variations
            if (token[0] == '$') continue; // skip NAGs

            current.moves.push_back(token);
        }

        if (!current.moves.empty())
            games.push_back(current);
        current = PGNGame{};
        moves_text.clear();
    };

    while (std::getline(in, line)) {
        // Trim
        while (!line.empty() && (line.back() == '\r' || line.back() == '\n'))
            line.pop_back();

        if (line.empty()) {
            if (!in_headers) {
                finish_game();
                in_headers = true;
            }
            continue;
        }

        if (line[0] == '[') {
            if (!in_headers) {
                finish_game();
                in_headers = true;
            }
            // Parse header
            auto q1 = line.find('"');
            auto q2 = line.rfind('"');
            if (q1 != std::string::npos && q2 > q1) {
                std::string key = line.substr(1, line.find(' ') - 1);
                std::string val = line.substr(q1 + 1, q2 - q1 - 1);
                if (key == "Result") current.result = val;
                else if (key == "WhiteElo") current.white_elo = std::atoi(val.c_str());
                else if (key == "BlackElo") current.black_elo = std::atoi(val.c_str());
            }
        } else {
            in_headers = false;
            moves_text += " " + line;
        }
    }
    finish_game();

    return games;
}

int main(int argc, char* argv[]) {
    std::string output = "training_data.epd";
    int skip_moves = 8;
    int min_elo = 0;
    int max_elo = 99999;
    int max_games = 0;
    bool append = false;
    std::vector<std::string> pgn_files;

    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if ((arg == "-o" || arg == "--output") && i + 1 < argc)
            output = argv[++i];
        else if (arg == "--skip-moves" && i + 1 < argc)
            skip_moves = std::stoi(argv[++i]);
        else if (arg == "--min-elo" && i + 1 < argc)
            min_elo = std::stoi(argv[++i]);
        else if (arg == "--max-elo" && i + 1 < argc)
            max_elo = std::stoi(argv[++i]);
        else if (arg == "--max-games" && i + 1 < argc)
            max_games = std::stoi(argv[++i]);
        else if (arg == "--append" || arg == "-a")
            append = true;
        else if (arg == "--help" || arg == "-h") {
            std::cerr << "Usage: " << argv[0] << " [options] file1.pgn [file2.pgn ...]\n"
                      << "  -o, --output FILE   Output EPD file (default: training_data.epd)\n"
                      << "  --skip-moves N      Skip first N moves (default: 8)\n"
                      << "  --min-elo N         Minimum player Elo (default: 0)\n"
                      << "  --max-elo N         Maximum player Elo (default: 99999)\n"
                      << "  --max-games N       Max games to process (default: 0=all)\n"
                      << "  -a, --append        Append to existing file\n";
            return 0;
        } else {
            pgn_files.push_back(arg);
        }
    }

    if (pgn_files.empty()) {
        std::cerr << "No PGN files specified. Use --help for usage.\n";
        return 1;
    }

    bitboards::init();
    magics::init();
    zobrist::init();
    kpk::init();

    std::ofstream out(output, append ? std::ios::app : std::ios::trunc);
    if (!out.is_open()) {
        std::cerr << "Cannot open " << output << " for writing\n";
        return 1;
    }

    uint64_t total_games = 0;
    uint64_t total_positions = 0;
    uint64_t skipped = 0;

    for (const auto& pgn_file : pgn_files) {
        std::cout << "Parsing " << pgn_file << "..." << std::flush;
        auto games = parse_pgn(pgn_file);
        std::cout << " " << games.size() << " games" << std::endl;

        for (const auto& game : games) {
            double result = parse_result(game.result);
            if (result < 0) { ++skipped; continue; }

            if ((min_elo > 0 || max_elo < 99999) &&
                (game.white_elo < min_elo || game.black_elo < min_elo ||
                 game.white_elo > max_elo || game.black_elo > max_elo)) {
                ++skipped;
                continue;
            }

            // Play through the game
            std::string start(uci::START_FEN);
            std::istringstream fen_stream(start);
            position pos(fen_stream);
            int move_num = 0;
            bool ok = true;

            for (const auto& san : game.moves) {
                if (!play_san_move(pos, san)) {
                    // Try UCI format as fallback
                    std::string start2(uci::START_FEN);
                    std::istringstream fen2(start2);
                    position pos2(fen2);
                    // Give up on this game
                    ok = false;
                    break;
                }

                ++move_num;

                // Extract quiet positions after skip_moves
                if (move_num > skip_moves && !pos.in_check()) {
                    out << pos.to_fen() << " c9 \"" << result << "\";\n";
                    ++total_positions;
                }
            }

            if (ok) ++total_games;
            else ++skipped;

            if (max_games > 0 && total_games >= static_cast<uint64_t>(max_games))
                break;

            if (total_games % 1000 == 0 && total_games > 0) {
                std::cout << "  " << total_games << " games, "
                          << total_positions << " positions\r" << std::flush;
            }
        }

        if (max_games > 0 && total_games >= static_cast<uint64_t>(max_games))
            break;
    }

    out.close();
    std::cout << "\nDone: " << total_games << " games → " << total_positions
              << " positions (" << skipped << " skipped)" << std::endl;
    std::cout << "Output: " << output << std::endl;

    return 0;
}
