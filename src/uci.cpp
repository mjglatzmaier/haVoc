#include "havoc/uci.hpp"

#include "havoc/book.hpp"
#include "havoc/movegen.hpp"
#include "havoc/position.hpp"
#include "havoc/tablebase.hpp"
#include "havoc/version.hpp"

#include <algorithm>
#include <chrono>
#include <iostream>
#include <sstream>
#include <string>

namespace havoc {
namespace uci {

void loop(SearchEngine& engine) {
    std::string fen_str{START_FEN};
    std::istringstream fen_stream(fen_str);
    position uci_pos(fen_stream);

    std::string input;
    while (std::getline(std::cin, input)) {
        if (!parse_command(input, engine, uci_pos))
            break;
    }
}

bool parse_command(const std::string& input, SearchEngine& engine, position& uci_pos) {
    std::istringstream instream(input);
    std::string cmd;
    bool running = true;

    while (instream >> std::skipws >> cmd) {
        std::transform(cmd.begin(), cmd.end(), cmd.begin(), ::tolower);

        if (cmd == "position" && instream >> cmd) {
            std::string tmp;
            if (cmd == "startpos") {
                std::getline(instream, tmp);
                std::string fen_str{START_FEN};
                std::istringstream fen(fen_str);
                uci_pos.setup(fen);
                load_position(tmp, uci_pos);
            } else {
                std::string sfen;
                while ((instream >> cmd) && cmd != "moves")
                    sfen += cmd + " ";
                std::getline(instream, tmp);
                tmp = "moves " + tmp;
                std::istringstream fen(sfen);
                uci_pos.setup(fen);
                load_position(tmp, uci_pos);
            }
        } else if (cmd == "setoption" && instream >> cmd && instream >> cmd) {
            std::transform(cmd.begin(), cmd.end(), cmd.begin(), ::tolower);
            if (cmd == "hash" && instream >> cmd && instream >> cmd) {
                auto sz = std::stoi(cmd);
                engine.set_hash_size(sz);
                break;
            }
            if (cmd == "clear" && instream >> cmd) {
                if (cmd == "hash")
                    engine.tt().clear();
            }
            if (cmd == "threads" && instream >> cmd && instream >> cmd) {
                engine.set_threads(std::stoi(cmd));
                break;
            }
            if (cmd == "syzygypath" && instream >> cmd && instream >> cmd) {
                havoc::tablebase::init(cmd);
                break;
            }
            if (cmd == "bookfile" && instream >> cmd && instream >> cmd) {
                havoc::book::load(cmd);
                break;
            }
            if (cmd == "paramfile" && instream >> cmd && instream >> cmd) {
                engine.load_params(cmd);
                std::cout << "info string Loaded parameters from " << cmd << std::endl;
                break;
            }
        } else if (cmd == "d") {
            uci_pos.print();
            std::cout << "position hash key: " << uci_pos.key() << std::endl;
            std::cout << "fen: " << uci_pos.to_fen() << std::endl;
        } else if (cmd == "isready") {
            std::cout << "readyok" << std::endl;
        } else if (cmd == "go") {
            engine.wait(); // ensure any prior search is done
            SearchLimits lims{};
            // GUIs are not always well behaved: they can report a negative
            // clock when an engine has overstepped, and a malformed token
            // would otherwise escape as an uncaught std::stoi exception and
            // take the process down. Clamp to zero and swallow bad input.
            auto parse_limit = [](const std::string& s) -> int {
                try {
                    return std::max(0, std::stoi(s));
                } catch (const std::exception&) {
                    return 0;
                }
            };
            while (instream >> cmd) {
                if (cmd == "wtime" && instream >> cmd)
                    lims.wtime = parse_limit(cmd);
                else if (cmd == "btime" && instream >> cmd)
                    lims.btime = parse_limit(cmd);
                else if (cmd == "winc" && instream >> cmd)
                    lims.winc = parse_limit(cmd);
                else if (cmd == "binc" && instream >> cmd)
                    lims.binc = parse_limit(cmd);
                else if (cmd == "movestogo" && instream >> cmd)
                    lims.movestogo = parse_limit(cmd);
                else if (cmd == "nodes" && instream >> cmd)
                    lims.nodes = parse_limit(cmd);
                else if (cmd == "movetime" && instream >> cmd)
                    lims.movetime = parse_limit(cmd);
                else if (cmd == "mate" && instream >> cmd)
                    lims.mate = parse_limit(cmd);
                else if (cmd == "depth" && instream >> cmd)
                    lims.depth = parse_limit(cmd);
                else if (cmd == "infinite")
                    lims.infinite = true;
                else if (cmd == "ponder")
                    lims.ponder = true;
            }

            bool silent = false;
            engine.start(uci_pos, lims, silent);
        } else if (cmd == "stop") {
            engine.stop();
        } else if (cmd == "moves") {
            Movegen mvs(uci_pos);
            mvs.generate<pseudo_legal, pieces>();
            for (int i = 0; i < mvs.size(); ++i) {
                if (!uci_pos.is_legal(mvs[i]))
                    continue;
                std::cout << move_to_string(mvs[i]) << " ";
            }
            std::cout << std::endl;
        } else if (cmd == "ucinewgame") {
            engine.clear();
            uci_pos.clear();
        } else if (cmd == "uci") {
            engine.clear();
            uci_pos.clear();
            std::cout << "id name " << ENGINE_NAME << " " << VERSION_STRING << std::endl;
            std::cout << "id author " << ENGINE_AUTHOR << std::endl;
            std::cout << "option name Threads type spin default 1 min 1 max 1024" << std::endl;
            std::cout << "option name Hash type spin default 1024 min 1 max 33554432" << std::endl;
            std::cout << "option name SyzygyPath type string default <empty>" << std::endl;
            std::cout << "option name BookFile type string default <empty>" << std::endl;
            std::cout << "option name ParamFile type string default <empty>" << std::endl;
            std::cout << "uciok" << std::endl;
        } else if (cmd == "bench") {
            static const std::vector<std::string> bench_fens = {
                "rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1",
                "r3k2r/p1ppqpb1/bn2pnp1/3PN3/1p2P3/2N2Q1p/PPPBBPPP/R3K2R w KQkq - 0 1",
                "8/2p5/3p4/KP5r/1R3p1k/8/4P1P1/8 w - - 0 1",
                "r3k2r/Pppp1ppp/1b3nbN/nP6/BBP1P3/q4N2/Pp1P2PP/R2Q1RK1 w kq - 0 1",
                "rnbq1k1r/pp1Pbppp/2p5/8/2B5/8/PPP1NnPP/RNBQK2R w KQ - 1 8",
                "r4rk1/1pp1qppp/p1np1n2/2b1p1B1/2B1P1b1/3P1N1P/PPP1NPP1/R2Q1RK1 w - - 0 1",
                "r1bqkb1r/pppppppp/2n2n2/8/3PP3/8/PPP2PPP/RNBQKBNR w KQkq - 2 3",
                "r1bqk2r/ppppbppp/2n2n2/4p3/2B1P3/5N2/PPPP1PPP/RNBQK2R w KQkq - 4 4",
                "rnbqkbnr/pppp1ppp/8/4p3/4P3/5N2/PPPP1PPP/RNBQKB1R b KQkq - 1 2",
                "r1bqkbnr/pppppppp/2n5/8/4P3/8/PPPP1PPP/RNBQKBNR w KQkq - 1 2",
                "r2q1rk1/ppp2ppp/2n1bn2/2b1p3/3pP3/3P1N1P/PPP1BPP1/RNBQR1K1 w - - 0 8",
                "2rr2k1/pp3ppp/2n1bn2/2q1p3/8/1NP2N1P/PP3PP1/R1BQR1K1 w - - 5 14",
            };

            int bench_depth = 10;
            if (instream >> cmd)
                bench_depth = std::stoi(cmd);

            U64 total_nodes = 0;
            auto start = std::chrono::steady_clock::now();

            for (const auto& fen_str : bench_fens) {
                std::istringstream fen(fen_str);
                position pos(fen);
                SearchLimits lims{};
                lims.depth = bench_depth;
                engine.start(pos, lims, true);
                engine.wait();
                total_nodes += engine.total_nodes();
            }

            auto end_time = std::chrono::steady_clock::now();
            double ms = std::chrono::duration<double, std::milli>(end_time - start).count();

            std::cout << "Bench: " << total_nodes << " nodes, " << static_cast<int>(ms) << " ms, "
                      << static_cast<U64>(total_nodes * 1000.0 / ms) << " nps" << std::endl;
        } else if (cmd == "exit" || cmd == "quit") {
            engine.stop();
            engine.wait();
            running = false;
            break;
        }
    }
    return running;
}

void load_position(const std::string& pos, position& uci_pos) {
    std::string token;
    std::istringstream ss(pos);

    ss >> token; // eat "moves" token
    while (ss >> token) {
        Movegen mvs(uci_pos);
        mvs.generate<pseudo_legal, pieces>();
        for (int j = 0; j < mvs.size(); ++j) {
            if (!uci_pos.is_legal(mvs[j]))
                continue;
            if (move_to_string(mvs[j]) == token) {
                uci_pos.do_move(mvs[j]);
                break;
            }
        }
    }
}

std::string move_to_string(const Move& m) {
    std::string fromto;
    fromto += kSanSquares[m.f];
    fromto += kSanSquares[m.t];
    auto t = static_cast<Movetype>(m.type);

    auto ps = (t == capture_promotion_q   ? "q"
               : t == capture_promotion_r ? "r"
               : t == capture_promotion_b ? "b"
               : t == capture_promotion_n ? "n"
               : t == promotion_q         ? "q"
               : t == promotion_r         ? "r"
               : t == promotion_b         ? "b"
               : t == promotion_n         ? "n"
                                          : "");

    return fromto + ps;
}

} // namespace uci
} // namespace havoc
