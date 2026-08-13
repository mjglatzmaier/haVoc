#pragma once

// Mirror a FEN vertically and swap piece colors, so that the resulting
// position is the exact color-and-rank reflection of the original. A
// color-symmetric engine must score a position and its mirror identically
// from the side to move's point of view, which makes this the cheapest
// available detector for coordinate-system bugs in evaluation and search.

#include <algorithm>
#include <cctype>
#include <sstream>
#include <string>
#include <vector>

namespace havoc {
namespace testing {

inline std::string mirror_fen(const std::string& fen) {
    std::istringstream iss(fen);
    std::string board, stm, castle, ep;
    std::string half = "0", full = "1";
    iss >> board >> stm >> castle >> ep;
    iss >> half >> full;

    std::vector<std::string> ranks;
    std::string cur;
    for (char ch : board) {
        if (ch == '/') {
            ranks.push_back(cur);
            cur.clear();
        } else {
            cur.push_back(ch);
        }
    }
    ranks.push_back(cur);

    std::string mirrored_board;
    for (int i = static_cast<int>(ranks.size()) - 1; i >= 0; --i) {
        for (char ch : ranks[i]) {
            auto uc = static_cast<unsigned char>(ch);
            mirrored_board.push_back(std::isupper(uc)
                                         ? static_cast<char>(std::tolower(uc))
                                         : static_cast<char>(std::toupper(uc)));
        }
        if (i != 0)
            mirrored_board.push_back('/');
    }

    std::string mirrored_castle;
    if (castle != "-") {
        // Swap the case of each right, then re-emit in canonical KQkq order so
        // the string does not depend on the order the original happened to use.
        std::string swapped;
        for (char ch : castle) {
            auto uc = static_cast<unsigned char>(ch);
            swapped.push_back(std::isupper(uc) ? static_cast<char>(std::tolower(uc))
                                               : static_cast<char>(std::toupper(uc)));
        }
        for (char want : std::string("KQkq"))
            if (swapped.find(want) != std::string::npos)
                mirrored_castle.push_back(want);
    }
    if (mirrored_castle.empty())
        mirrored_castle = "-";

    std::string mirrored_ep = ep;
    if (ep != "-" && ep.size() == 2)
        mirrored_ep[1] = (ep[1] == '6') ? '3' : '6';

    return mirrored_board + " " + (stm == "w" ? "b" : "w") + " " + mirrored_castle + " " +
           mirrored_ep + " " + half + " " + full;
}

}  // namespace testing
}  // namespace havoc
