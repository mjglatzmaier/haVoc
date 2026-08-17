#include "havoc/bitboard.hpp"
#include "havoc/eval/nnue_evaluator.hpp"
#include "havoc/kpk.hpp"
#include "havoc/magics.hpp"
#include "havoc/net_path.hpp"
#include "havoc/search.hpp"
#include "havoc/uci.hpp"
#include "havoc/version.hpp"
#include "havoc/zobrist.hpp"

#include <iostream>
#include <memory>

namespace {

/// Loads the network the binary was built to expect, if it can be found.
///
/// A missing network is not fatal -- the handcrafted evaluation still plays
/// chess -- but it is worth complaining about, because the engine is far
/// weaker without it and nothing else would say so. `EvalFile` overrides
/// whatever happens here.
void load_default_network(havoc::SearchEngine& engine) {
    const auto name = havoc::net::default_net_name();
    if (name.empty())
        return;

    const auto path = havoc::net::find_default_net();
    if (!path) {
        std::cout << "info string no network found (expected " << name
                  << "), using the handcrafted evaluation, which is much weaker.\n"
                  << "info string fetch it with scripts/nnue/fetch-net.sh, or set EvalFile."
                  << std::endl;
        return;
    }

    std::string err;
    auto net = havoc::load_network_file(*path, err);
    if (!net) {
        std::cout << "info string " << err << ", using the handcrafted evaluation" << std::endl;
        return;
    }
    engine.set_evaluator_factory([net](havoc::Searchthread&) {
        return std::make_unique<havoc::NNUEEvaluator>(net);
    });
    std::cout << "info string Loaded network from " << *path << std::endl;
}

} // namespace

int main() {
    std::cout << havoc::ENGINE_NAME << " v" << havoc::VERSION_STRING << std::endl;
    std::cout << "by " << havoc::ENGINE_AUTHOR << std::endl;

    havoc::bitboards::init();
    havoc::magics::init();
    havoc::zobrist::init();
    havoc::kpk::init();

    havoc::SearchEngine engine;
    load_default_network(engine);
    havoc::uci::loop(engine);

    return 0;
}
