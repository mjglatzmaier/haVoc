#include "havoc/movegen.hpp"

namespace havoc {

void Movegen::print() {
    for (int j = 0; j < last; ++j) {
        std::cout << kSanSquares[store.moves[j].f] << kSanSquares[store.moves[j].t] << " ";
    }
    std::cout << "\n";
}

void Movegen::print_legal(position& p) {
    for (int j = 0; j < last; ++j) {
        if (p.is_legal(store.moves[j]))
            std::cout << kSanSquares[store.moves[j].f] << kSanSquares[store.moves[j].t] << " ";
    }
    std::cout << "\n";
}

} // namespace havoc
