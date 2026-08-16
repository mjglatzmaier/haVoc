#include "havoc/uci.hpp"

#include "havoc/book.hpp"
#include "havoc/eval/nnue_evaluator.hpp"
#include "havoc/movegen.hpp"
#include "havoc/position.hpp"
#include "havoc/tablebase.hpp"
#include "havoc/tt.hpp"
#include "havoc/version.hpp"

#include <algorithm>
#include <charconv>
#include <chrono>
#include <iostream>
#include <sstream>
#include <string>

namespace havoc {
namespace uci {

namespace {

/// Parses a spin option value. std::stoi terminated the process on anything
/// non-numeric, and a GUI is free to send whatever it likes.
bool parse_int(const std::string& tok, int& out) {
    const char* first = tok.data();
    const char* last = first + tok.size();
    const auto [ptr, ec] = std::from_chars(first, last, out);
    return ec == std::errc() && ptr == last;
}

}  // namespace

void loop(SearchEngine& engine) {
    std::string fen_str{START_FEN};
    std::istringstream fen_stream(fen_str);
    position uci_pos(fen_stream);

    std::string input;
    while (std::getline(std::cin, input)) {
        if (!parse_command(input, engine, uci_pos))
            break;
    }

    // getline fails on EOF as well as on "quit", and a GUI that dies or simply
    // closes the pipe never sends "quit". The search runs on worker threads
    // that hold a reference to uci_pos, so returning here while one is still
    // in flight destroys the position underneath them. The quit branch of
    // parse_command already does this; do it on every exit from the loop.
    engine.stop();
    engine.wait();
}

bool parse_command(const std::string& input, SearchEngine& engine, position& uci_pos) {
    std::istringstream instream(input);
    std::string cmd;
    bool running = true;

    // Anything that rewrites state the search is reading has to wait for the
    // search to be over first. "stop" only raises a flag and returns, so a GUI
    // that sends "stop" and then sets up the next position -- which is what a
    // GUI does -- was racing the search threads on their way out.
    //
    // Two of these were hard crashes rather than theory. "setoption name Hash"
    // reallocates the table the search threads are reading, and "setoption name
    // Threads" reallocates the pool the running threads live in; both segfault
    // reproducibly in a plain release build. ThreadSanitizer also reports
    // "position" and "ucinewgame" tearing the position and the table out from
    // under the search.
    //
    // Waiting rather than refusing: a GUI that has moved on to the next
    // position has no use for the answer to the old one, so there is nothing to
    // preserve by declining.
    auto settle = [&engine]() {
        engine.stop();
        engine.wait();
    };

    while (instream >> std::skipws >> cmd) {
        std::transform(cmd.begin(), cmd.end(), cmd.begin(), ::tolower);

        if (cmd == "position" && instream >> cmd) {
            settle();
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
                if (!uci_pos.setup(fen)) {
                    // Keep the engine on a legal board. A kingless FEN used to
                    // be accepted and then indexed the attack tables with
                    // no_square.
                    std::cout << "info string ignoring illegal fen: " << sfen << std::endl;
                    std::string start_str{START_FEN};
                    std::istringstream start(start_str);
                    uci_pos.setup(start);
                } else {
                    load_position(tmp, uci_pos);
                }
            }
        } else if (cmd == "setoption" && instream >> cmd && instream >> cmd) {
            settle();
            std::transform(cmd.begin(), cmd.end(), cmd.begin(), ::tolower);
            if (cmd == "hash" && instream >> cmd && instream >> cmd) {
                int sz = 0;
                if (!parse_int(cmd, sz))
                    std::cout << "info string ignoring non-numeric Hash value: " << cmd
                              << std::endl;
                else if (!engine.set_hash_size(sz))
                    std::cout << "info string ignoring Hash value " << sz << ": must be "
                              << kMinHashMb << ".." << kMaxHashMb
                              << " MB and fit in memory, keeping the current table"
                              << std::endl;
                break;
            }
            if (cmd == "clear" && instream >> cmd) {
                if (cmd == "hash")
                    engine.tt().clear();
            }
            if (cmd == "threads" && instream >> cmd && instream >> cmd) {
                int n = 0;
                if (!parse_int(cmd, n))
                    std::cout << "info string ignoring non-numeric Threads value: " << cmd
                              << std::endl;
                else
                    engine.set_threads(n);
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
            if (cmd == "evalfile" && instream >> cmd && instream >> cmd) {
                // "none" is how a GUI turns the network back off; without it
                // installing one would be a one-way door for the session.
                if (cmd == "none" || cmd == "<empty>") {
                    engine.set_evaluator_factory(nullptr);
                    std::cout << "info string EvalFile cleared, using the handcrafted evaluation"
                              << std::endl;
                    break;
                }
                std::string err;
                auto net = load_network_file(cmd, err);
                if (!net) {
                    // Keep playing with the evaluation that already works
                    // rather than refusing to start: a missing net is a
                    // configuration mistake, not a reason to forfeit.
                    std::cout << "info string " << err << ", keeping the current evaluation"
                              << std::endl;
                    break;
                }
                engine.set_evaluator_factory(
                    [net](Searchthread&) { return std::make_unique<NNUEEvaluator>(net); });
                std::cout << "info string Loaded network from " << cmd << std::endl;
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
            // A new "go" supersedes whatever is running. Plain wait() hung here
            // forever if the previous search was "go infinite" or a ponder the GUI
            // never resolved.
            settle();
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
        } else if (cmd == "ponderhit") {
            // The GUI played the move the ponder search assumed. Keep
            // searching, but start charging it to the clock. Without this the
            // engine pondered on until stdin closed and lost on time.
            engine.ponder_hit();
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
            settle();
            engine.clear();
            uci_pos.clear();
        } else if (cmd == "uci") {
            settle();
            engine.clear();
            uci_pos.clear();
            std::cout << "id name " << ENGINE_NAME << " " << VERSION_STRING << std::endl;
            std::cout << "id author " << ENGINE_AUTHOR << std::endl;
            std::cout << "option name Threads type spin default 1 min " << kMinThreads
                      << " max " << kMaxThreads << std::endl;
            std::cout << "option name Hash type spin default " << kDefaultHashMb << " min "
                      << kMinHashMb << " max " << kMaxHashMb << std::endl;
            std::cout << "option name SyzygyPath type string default <empty>" << std::endl;
            std::cout << "option name BookFile type string default <empty>" << std::endl;
            std::cout << "option name ParamFile type string default <empty>" << std::endl;
            std::cout << "option name EvalFile type string default <empty>" << std::endl;
            std::cout << "uciok" << std::endl;
        } else if (cmd == "bench") {
            settle();
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
                      << static_cast<U64>(static_cast<double>(total_nodes) * 1000.0 / ms)
                      << " nps" << std::endl;
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
        // Promotions are compared against move_to_string, which spells the
        // piece in lower case. A GUI that sends "a7a8Q" is not wrong -- the
        // case of the promotion letter is a long-standing interop wart -- and
        // matching it exactly meant the move was quietly dropped.
        std::transform(token.begin(), token.end(), token.begin(),
                       [](unsigned char c) { return static_cast<char>(::tolower(c)); });

        Movegen mvs(uci_pos);
        mvs.generate<pseudo_legal, pieces>();

        bool played = false;
        for (int j = 0; j < mvs.size(); ++j) {
            if (!uci_pos.is_legal(mvs[j]))
                continue;
            if (move_to_string(mvs[j]) == token) {
                uci_pos.do_move(mvs[j]);
                played = true;
                break;
            }
        }

        // Skipping the move silently is the worst thing to do here. The engine
        // is now a move behind the GUI and every later move in the list is
        // applied to the wrong position, so it goes on to answer a position
        // nobody is playing -- and the first move it returns that is not legal
        // in the real game loses it. Say so, and stop rather than compound it.
        if (!played) {
            std::cout << "info string ignoring unplayable move '" << token
                      << "' -- position may be out of sync with the GUI" << std::endl;
            return;
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
