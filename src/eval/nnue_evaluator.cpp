#include "havoc/eval/nnue_evaluator.hpp"

#include <fstream>

namespace havoc {

std::shared_ptr<const nnue::Network> load_network_file(const std::string& path, std::string& err) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        err = "network: cannot open " + path;
        return nullptr;
    }
    auto net = std::make_shared<nnue::Network>();
    err = net->load(in);
    if (!err.empty())
        return nullptr;
    return net;
}

} // namespace havoc
