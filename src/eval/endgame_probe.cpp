/// @file endgame_probe.cpp

#include "havoc/eval/endgame_probe.hpp"

#include "havoc/bitboard.hpp"
#include "havoc/kpk.hpp"

namespace havoc::eval {

bool probe_kpk(const position& p, bool& strong_wins) {
    const U64 wp = p.get_pieces<white, pawn>();
    const U64 bp = p.get_pieces<black, pawn>();
    // Two kings and one pawn, nothing else. Counting the occupancy is what
    // makes this usable from an evaluator that has no material table in hand.
    if (bits::count(p.all_pieces()) != 3 || bits::count(wp | bp) != 1)
        return false;

    const Color strong = wp ? white : black;
    int psq = bits::lsb(wp | bp);
    int sk = static_cast<int>(p.king_square(strong));
    int wk = static_cast<int>(p.king_square(strong == white ? black : white));

    // The bitbase is indexed from the point of view of a white pawn standing on
    // files a-d, so both reflections have to be applied to every square.
    if (strong == black) {
        psq ^= 56;
        sk ^= 56;
        wk ^= 56;
    }
    if (util::col(psq) > Col::D) {
        psq ^= 7;
        sk ^= 7;
        wk ^= 7;
    }

    const Color stm = (p.to_move() == strong) ? white : black;
    strong_wins = kpk::probe(sk, psq, wk, stm);
    return true;
}

}  // namespace havoc::eval
