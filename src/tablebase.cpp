/// @file tablebase.cpp
/// @brief Syzygy tablebase probing, implemented over the vendored Fathom
///        library (third_party/fathom, MIT).
///
/// Why this file exists at all
/// --------------------------
/// `SyzygyPath` has been advertised in the UCI option list since the engine
/// was first written, and until now `init()` returned false and the search
/// never probed anything. An engine that offers an option it does not
/// implement is lying to its operator, and a GUI that sets the path has no way
/// to find out. This replaces that stub.
///
/// What is probed and what is not
/// ------------------------------
/// Only WDL, and only in the search. Fathom's `tb_probe_wdl` refuses any
/// position with castling rights or a non-zero fifty-move counter, because the
/// tables assume neither exists; both conditions are re-checked here rather
/// than relying on that, so the caller's tb-hit counter stays honest.
///
/// DTZ is not used. DTZ is what tells you *how far* a win is, and without it a
/// won position where every move says "win" gives the search nothing to
/// maximise. The answer used here is the usual one: the caller scores a
/// tablebase win as `kTbWin - ply`, so the same win found sooner beats it
/// found later and the search converts rather than shuffles. That is weaker
/// than true DTZ play, and docs/ says so -- but it is correct, whereas DTZ's
/// deliberately "unnatural" moves are only safe behind root-move filtering.
///
/// Cursed wins count as draws
/// --------------------------
/// `TB_CURSED_WIN` is a win the fifty-move rule takes away. Under the rules
/// this engine actually plays by that is a draw, and reporting it as a win
/// would steer the search into a position it cannot convert.

#include "havoc/tablebase.hpp"

#include <mutex>
#include <string>

#if HAVOC_SYZYGY
#include "tbprobe.h"
#endif

namespace havoc::tablebase {

#if HAVOC_SYZYGY
namespace {
/// Guards `tb_init`/`tb_free` only. Probing itself is thread safe -- Fathom is
/// built without TB_NO_THREADS -- but tearing the tables down underneath a
/// running search is not, and `setoption` can arrive at any moment.
std::mutex g_init_mutex;
bool g_initialised = false;
} // namespace
#endif

bool init(const std::string& path) {
#if HAVOC_SYZYGY
    std::lock_guard<std::mutex> lock(g_init_mutex);
    if (g_initialised) {
        tb_free();
        g_initialised = false;
    }
    // An empty path is how a GUI says "no tablebases". That is a successful
    // teardown, not a failed load.
    if (path.empty() || path == "<empty>")
        return true;
    if (!tb_init(path.c_str()))
        return false;
    g_initialised = true;
    return TB_LARGEST > 0;
#else
    // Nothing to load, but clearing still succeeds: an operator who unsets
    // SyzygyPath should get the same answer in every build, and reporting a
    // failure would send them looking for tables they never asked for.
    return path.empty() || path == "<empty>";
#endif
}

void shutdown() {
#if HAVOC_SYZYGY
    std::lock_guard<std::mutex> lock(g_init_mutex);
    if (g_initialised) {
        tb_free();
        g_initialised = false;
    }
#endif
}

int probe_wdl(const position& pos) {
#if HAVOC_SYZYGY
    if (TB_LARGEST == 0)
        return kProbeFailed;

    // The tables encode neither castling rights nor progress toward the
    // fifty-move rule, so a position carrying either is not the position the
    // table describes.
    if (pos.castle_mask() != 0 || pos.rule50() != 0)
        return kProbeFailed;

    const U64 white_pieces = pos.get_pieces<white>();
    const U64 black_pieces = pos.get_pieces<black>();
    if (static_cast<unsigned>(bits::count(white_pieces | black_pieces)) > TB_LARGEST)
        return kProbeFailed;

    const Square ep = pos.eps();
    const unsigned result =
        tb_probe_wdl(white_pieces, black_pieces,
                     pos.get_pieces<white, king>() | pos.get_pieces<black, king>(),
                     pos.get_pieces<white, queen>() | pos.get_pieces<black, queen>(),
                     pos.get_pieces<white, rook>() | pos.get_pieces<black, rook>(),
                     pos.get_pieces<white, bishop>() | pos.get_pieces<black, bishop>(),
                     pos.get_pieces<white, knight>() | pos.get_pieces<black, knight>(),
                     pos.get_pieces<white, pawn>() | pos.get_pieces<black, pawn>(), 0u, 0u,
                     ep == no_square ? 0u : static_cast<unsigned>(ep), pos.to_move() == white);

    if (result == TB_RESULT_FAILED)
        return kProbeFailed;

    switch (result) {
    case TB_WIN:
        return 1;
    case TB_LOSS:
        return -1;
    // TB_CURSED_WIN and TB_BLESSED_LOSS are decided by the fifty-move rule,
    // which this engine enforces, so in play they are draws.
    default:
        return 0;
    }
#else
    (void)pos;
    return kProbeFailed;
#endif
}

bool available() {
#if HAVOC_SYZYGY
    return TB_LARGEST > 0;
#else
    return false;
#endif
}

int max_pieces() {
#if HAVOC_SYZYGY
    return static_cast<int>(TB_LARGEST);
#else
    return 0;
#endif
}

} // namespace havoc::tablebase
