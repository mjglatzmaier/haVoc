/// @file eval_pairs.hpp
/// @brief Paired quiet positions of equal material, and the feature each pair
///        isolates.
///
/// This corpus is shared by two consumers that ask different questions of it:
///
///   tests/test_eval_discrimination.cpp asserts the *ordering* -- the better
///   position must score higher. That is a pass/fail regression guard.
///
///   tools/eval_bench.cpp asks the harder questions: by how much, and because
///   of which term? An evaluation can rank a pair correctly for entirely the
///   wrong reason -- a piece-square table accident rather than the feature the
///   pair was built to probe -- and an ordering assertion cannot tell the two
///   apart. Attribution can.
///
/// Rules for a case to be admissible as evidence:
///
///   Equal material on both sides, asserted mechanically by the test. Material
///   is worth 357cp on this evaluation and would swamp every positional term.
///
///   Exactly one feature differs. This is the rule that is easiest to break by
///   accident. An earlier open-file pair moved the pawns rather than the rook,
///   which isolated the file but also changed the king's cover, so the pair
///   silently measured two things and failed once king file terms were given
///   real magnitudes.
///
///   Enough material to avoid the specialised endgame evaluators (KRK, KPK,
///   KBNK and friends in hce.cpp), which return mate-driving scores and never
///   consult the general term under test.
///
///   The same side to move in both positions.

#pragma once

#include <vector>

namespace havoc::eval_pairs {

/// One discrimination case.
///
/// `expects` names the parameter family that ought to be responsible for the
/// score difference, as a substring match against the registered parameter
/// name. It is advisory: eval_bench reports the terms that actually decide the
/// pair, and a mismatch against `expects` is a finding to investigate, not a
/// failure. An empty string means "no strong prior -- just report".
struct Pair {
    const char* feature;
    const char* better;
    const char* worse;
    const char* rationale;
    const char* expects;
};

inline const std::vector<Pair>& all() {
    static const std::vector<Pair> pairs = {
    {"connected pawns beat isolated pawns",
     "4k3/8/8/8/8/8/PP6/4K3 w - - 0 1",
     "4k3/8/8/8/8/8/P1P5/4K3 w - - 0 1",
     "a2+b2 defend each other; a2+c2 are both isolated and need pieces to hold them",
     "isolated_pawn_penalty"},

    {"healthy pawns beat doubled pawns",
     "4k3/8/8/8/8/8/PP6/4K3 w - - 0 1",
     "4k3/8/8/8/P7/8/P7/4K3 w - - 0 1",
     "doubled a-pawns cover fewer files and cannot defend one another",
     "doubled_pawn_penalty"},

    {"a passed pawn beats a blocked pawn",
     "4k3/p7/8/4P3/8/8/8/4K3 w - - 0 1",
     "4k3/8/4p3/4P3/8/8/8/4K3 w - - 0 1",
     "e5 with no black pawn ahead is passed; e5 facing e6 is permanently blocked",
     "passed_pawn"},

    // Only the rook moves. An earlier version of this pair kept the rook on
    // d1 and moved the pawns from a2/b2 to d2/e2, which does isolate the open
    // file -- but it also moves both pawns away from the king on e1, so the
    // king's own file cover changes at the same time. That made the pair test
    // two things at once, and once the king file penalties were given real
    // magnitudes the shelter difference outweighed the file difference and the
    // pair failed for a reason that had nothing to do with rooks. A pair is
    // only evidence about a feature if it is the single thing that differs.
    {"a rook prefers an open file",
     "4k3/8/8/8/8/8/2PP4/1R2K3 w - - 0 1",
     "4k3/8/8/8/8/8/2PP4/2R1K3 w - - 0 1",
     "the b-file has no pawn on it; the c-rook stares at its own c2 pawn",
     "open_file_bonus"},

    {"a castled king beats a king in the centre",
     "4k2r/5ppp/8/8/8/8/5PPP/5RK1 w k - 0 1",
     "4k2r/5ppp/8/8/8/8/5PPP/4KR2 w k - 0 1",
     "same pieces; g1 sits behind an intact f2-g2-h2 shelter, e1 does not",
     "king_shelter"},

    {"a knight prefers an advanced supported outpost",
     "4k3/pp6/8/3N4/4P3/8/8/4K3 w - - 0 1",
     "4k3/pp6/8/8/4P3/3N4/8/4K3 w - - 0 1",
     "d5 is supported by e4 and cannot be driven off by a pawn; d3 is passive",
     "outpost"},

    {"a rook prefers the seventh rank",
     "4k3/3R1ppp/8/8/8/8/5PPP/4K3 w - - 0 1",
     "4k3/5ppp/8/8/8/8/3R1PPP/4K3 w - - 0 1",
     "d7 rakes the seventh rank and ties black to f7; d2 is passive",
     ""},

    {"an endgame king prefers the centre",
     "7k/8/8/3K4/8/8/8/R6r w - - 0 1",
     "7k/8/8/8/8/8/K7/R6r w - - 0 1",
     "with queens off, a centralised king is worth roughly a third of a pawn",
     "pst_eg_king"},

    {"an intact shelter beats an advanced one",
     "6k1/5ppp/8/8/8/8/5PPP/6K1 w - - 0 1",
     "6k1/5ppp/8/8/5P1P/8/6P1/6K1 w - - 0 1",
     "f4/h4 have left holes on g3 and the third rank in front of the king",
     "king_shelter"},

    {"a bishop prefers pawns on the opposite colour",
     "4k3/pp6/8/8/3P4/4P3/8/4KB2 w - - 0 1",
     "4k3/pp6/8/8/4P3/3P4/8/4KB2 w - - 0 1",
     "the f1 bishop is light-squared; d4/e3 are dark, while e4/d3 block it",
     "bishop_own_pawn_penalty"},

    {"a rook belongs behind its own passed pawn",
     "5k2/8/8/4P3/8/8/8/4RK2 w - - 0 1",
     "5k2/8/4R3/4P3/8/8/8/5K2 w - - 0 1",
     "the rook supports the advance from behind instead of blocking it",
     ""},

    {"a protected passer beats a lone passer",
     "4k3/8/8/3PP3/8/8/8/4K3 w - - 0 1",
     "4k3/8/8/4P3/8/3P4/8/4K3 w - - 0 1",
     "d5 and e5 defend each other as they advance; d3 and e5 are split",
     "passed_pawn"},
    };
    return pairs;
}

} // namespace havoc::eval_pairs
