#include "havoc/bitboard.hpp"
#include "havoc/kpk.hpp"
#include "havoc/magics.hpp"
#include "havoc/search.hpp"
#include "havoc/uci.hpp"
#include "havoc/version.hpp"
#include "havoc/zobrist.hpp"

#include <iostream>

int main() {
    std::cout << havoc::ENGINE_NAME << " v" << havoc::VERSION_STRING << std::endl;
    std::cout << "by " << havoc::ENGINE_AUTHOR << std::endl;

    havoc::bitboards::init();
    havoc::magics::init();
    havoc::zobrist::init();
    havoc::kpk::init();

    havoc::SearchEngine engine;
    havoc::uci::loop(engine);

    return 0;
}
