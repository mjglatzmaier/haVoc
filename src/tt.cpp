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
    entry* const first = first_entry(key);
    entry* replace = nullptr;
    bool same_key = false;

    for (unsigned i = 0; i < cluster_size; ++i) {
        entry* e = first + i;
        if (e->empty()) {
            replace = e;
            break;
        }
        if ((e->pkey ^ e->dkey) == key) {
            replace = e;
            same_key = true;
            break;
        }
    }

    // The cluster is full and holds nothing for this key, so something has to
    // go. Evict whichever entry we are least likely to want again: shallow
    // entries first, and entries left over from earlier searches before those
    // written by the current one.
    if (replace == nullptr) {
        replace = first;
        int worst = entry_value(first);
        for (unsigned i = 1; i < cluster_size; ++i) {
            const int value = entry_value(first + i);
            if (value < worst) {
                worst = value;
                replace = first + i;
            }
        }
    }

    Move best = m;
    if (same_key) {
        hash_data existing;
        existing.decode(replace->dkey);

        // Keep a much deeper result from this same search unless the new one is
        // exact, which is always worth having.
        if (bound != Bound::bound_exact && relative_age(existing.age) == 0 &&
            static_cast<int>(depth) + 4 < static_cast<int>(existing.depth))
            return;

        // Never trade a usable move for no move at all.
        if (best.is_null())
            best = existing.move;
    }

    replace->encode(depth, bound, generation_, best, score);
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
