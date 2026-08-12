#include "havoc/tt.hpp"

#include <algorithm>
#include <cstring>

namespace havoc {

namespace {
inline size_t next_pow2(size_t x) {
    if (x <= 2)
        return 2;
    return 1ULL << (64 - __builtin_clzll(x - 1));
}
} // namespace

hash_table::hash_table() {
    resize(128);
}
void hash_table::resize(size_t size_mb) {
    sz_mb_ = size_mb;
    cluster_count_ = 1024ULL * 1024 * sz_mb_ / sizeof(hash_cluster);
    cluster_count_ = next_pow2(cluster_count_);
    if (cluster_count_ < 1024)
        cluster_count_ = 1024;

    entries_.reset();
    entries_ = std::make_unique<hash_cluster[]>(cluster_count_);
    clear();
}

void hash_table::clear() {
    std::memset(entries_.get(), 0, sizeof(hash_cluster) * cluster_count_);
}

bool hash_table::fetch(U64 key, hash_data& e) {
    entry* stored = first_entry(key);
    prefetch(stored);

    for (unsigned i = 0; i < cluster_size; ++i, ++stored) {
        if ((stored->pkey ^ stored->dkey) == key) {
            e.decode(stored->dkey);
            return true;
        }
    }
    return false;
}

void hash_table::save(U64 key, U8 depth, U8 bound, const Move& m, int16_t score,
                      bool /*pv_node*/) {
    const U8 age = generation_;
    entry* e = first_entry(key);
    entry* replace = e;

    for (unsigned i = 0; i < cluster_size; ++i, ++e) {
        if (e->empty()) {
            replace = e;
            break;
        }

        if ((e->pkey ^ e->dkey) == key) {
            if (relative_age(e->age()) > 1 && depth > e->depth() - 4) {
                replace = e;
                break;
            }
            if (e->depth() < depth) {
                replace = e;
                break;
            }
            if (e->bound() != Bound::bound_low && bound == Bound::bound_low) {
                replace = e;
                continue;
            }
        }
    }

    replace->encode(depth, bound, age, m, score);
    replace->pkey = key ^ replace->dkey;
}

int hash_table::hashfull() const {
    int used = 0;
    size_t sample = std::min(cluster_count_, size_t(1000));
    for (size_t i = 0; i < sample; ++i) {
        for (unsigned j = 0; j < cluster_size; ++j) {
            if (!entries_[i].cluster_entries[j].empty())
                ++used;
        }
    }
    return used * 1000 / static_cast<int>(sample * cluster_size);
}

} // namespace havoc
