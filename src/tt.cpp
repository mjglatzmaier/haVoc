#include "havoc/tt.hpp"

#include <algorithm>
#include <cstring>
#include <memory>
#include <new>

namespace havoc {

namespace {
inline size_t next_pow2(size_t x) {
    if (x <= 2)
        return 2;
    return 1ULL << (64 - __builtin_clzll(x - 1));
}
} // namespace

hash_table::hash_table() {
    // 128 MB at construction. If even this fails the process has no usable
    // table, but there is nothing useful to do about it here and throwing from
    // a constructor that runs before main() is worse than starting up.
    (void)resize(128);
}
bool hash_table::resize(size_t size_mb) {
    size_t clusters = 1024ULL * 1024 * size_mb / sizeof(hash_cluster);
    clusters = next_pow2(clusters);
    if (clusters < 1024)
        clusters = 1024;

    // Allocate before committing. A GUI is free to ask for more memory than the
    // machine has, and throwing out of here killed the process; dropping the
    // existing table first would leave a null entries_ behind even if the caller
    // did catch. Refusing the change keeps the engine playable.
    std::unique_ptr<hash_cluster[]> fresh;
    try {
        fresh = std::make_unique<hash_cluster[]>(clusters);
    } catch (const std::bad_alloc&) {
        return false;
    }

    entries_ = std::move(fresh);
    cluster_count_ = clusters;
    sz_mb_ = size_mb;
    clear();
    return true;
}

void hash_table::clear() {
    // Value-initialisation rather than memset: every member of `entry` carries a
    // zero default initialiser, so the two are equivalent here, but the type is
    // not trivially default-constructible and memset on it is what -Wclass-memaccess
    // objects to. GCC still lowers this to a memset.
    std::fill_n(entries_.get(), cluster_count_, hash_cluster{});
}

bool hash_table::fetch(U64 key, hash_data& e) {
    entry* stored = first_entry(key);
    prefetch(stored);

    for (unsigned i = 0; i < cluster_size; ++i, ++stored) {
        if ((stored->pkey ^ stored->dkey) == key) {
            e.decode(stored->dkey);

            // An entry that is still being hit is still useful, but eviction
            // scores it by when it was *written*, not by when it was last
            // needed: entry_value() is depth - 8 * relative_age(). Without a
            // refresh, everything carried over from the previous search looks
            // one generation stale no matter how often the current search
            // transposes into it, and gets thrown out in favour of whatever
            // shallow entry the current search happens to write next.
            //
            // Stamp the current generation on the way past. The age field is
            // bits 55-62 of dkey, so it can be rewritten in place; pkey has to
            // be re-derived from the key so the XOR validation still holds.
            if (e.age != generation_) {
                const U64 refreshed =
                    (stored->dkey & ~(0xFFULL << 55)) | (U64(generation_) << 55);
                stored->dkey = refreshed;
                stored->pkey = key ^ refreshed;
                e.age = generation_;
            }
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
