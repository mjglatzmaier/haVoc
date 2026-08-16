#pragma once

/// @file tt.hpp
/// @brief Transposition table with XOR-based entry encoding.

#include <atomic>
#include "havoc/types.hpp"

#include <cstring>
#include <memory>

#if defined(_MSC_VER)
#include <xmmintrin.h>
#endif

namespace havoc {

/// Bounds for the Hash spin option advertised by the "uci" handshake. uci.cpp
/// prints these and hash_table::resize() enforces them, so the advertised range
/// and the enforced range cannot drift apart.
///
/// kDefaultHashMb must also be the size the constructor actually builds. It
/// said 1024 while the constructor built 128, so a GUI that only sends
/// setoption for values the operator changed left the engine on an eighth of
/// the table it was displaying, and nothing reported the discrepancy.
constexpr int kMinHashMb = 1;
constexpr int kMaxHashMb = 33554432;
constexpr int kDefaultHashMb = 128;

// ─── Bounds ─────────────────────────────────────────────────────────────────

enum Bound { bound_low, bound_high, bound_exact, no_bound };

// ─── Prefetch ───────────────────────────────────────────────────────────────

inline void prefetch(const void* addr) {
#if defined(__GNUC__) || defined(__clang__)
    __builtin_prefetch(addr, 0, 3);
#elif defined(_MSC_VER)
    _mm_prefetch(static_cast<const char*>(addr), _MM_HINT_T0);
#endif
}

// ─── TT entry ───────────────────────────────────────────────────────────────
//
// dkey layout (64 bits):
//   bits  0- 7 : from-square  (8 bits)
//   bits  8-15 : to-square    (8 bits)
//   bits 16-23 : move type    (8 bits)
//   bits 24-25 : (unused)
//   bits 26-29 : bound        (4 bits)
//   bits 30-37 : depth        (8 bits)
//   bits 38-53 : |score|      (16 bits)
//   bit  54    : score sign   (1 bit, 1 = negative)
//   bits 55-62 : generation   (8 bits)
//   bit  63    : (unused)
//
// pkey = zobrist_key ^ dkey   (Hyatt's XOR trick)

// Both halves are atomic because every search thread reads and writes them
// concurrently. The XOR trick below detects a *torn* pair, which is a
// different problem: it is a semantic check on the values, and it does nothing
// about the fact that concurrent non-atomic access is a data race and so
// undefined behaviour. Without the atomics the compiler is entitled to assume
// no other thread touches these words, and may reload or split the accesses.
//
// The ordering is relaxed throughout. Nothing here publishes any other memory,
// so there is nothing to synchronise-with; the entry is self-describing and
// the XOR check validates it. On x86-64 a relaxed 64-bit load or store is a
// plain mov, so this costs nothing at the instruction level.
struct entry {
    std::atomic<U64> pkey{0};
    std::atomic<U64> dkey{0};

    [[nodiscard]] U64 pk() const { return pkey.load(std::memory_order_relaxed); }
    [[nodiscard]] U64 dk() const { return dkey.load(std::memory_order_relaxed); }

    /// Callers must load each half exactly once and work from the locals, or
    /// the validation and the decode can see different writes.
    void put(U64 p, U64 d) {
        dkey.store(d, std::memory_order_relaxed);
        pkey.store(p, std::memory_order_relaxed);
    }

    [[nodiscard]] bool empty() const { return pk() == 0 && dk() == 0; }

    [[nodiscard]] static U64 encode(U8 depth, U8 bound, U8 age, const Move& m, int16_t score) {
        U64 d = 0;
        d |= U64(m.f);
        d |= U64(m.t) << 8;
        d |= U64(m.type) << 16;
        d |= U64(bound & 0xF) << 26;
        d |= U64(depth) << 30;
        d |= U64(score < 0 ? -score : score) << 38;
        d |= U64(score < 0 ? 1ULL : 0ULL) << 54;
        d |= U64(age) << 55;
        return d;
    }

    [[nodiscard]] U8 depth() const { return U8((dk() >> 30) & 0xFF); }
    [[nodiscard]] U8 bound() const { return U8((dk() >> 26) & 0xF); }
    [[nodiscard]] U8 age() const { return U8((dk() >> 55) & 0xFF); }
};

// ─── Decoded hash data ──────────────────────────────────────────────────────

struct hash_data {
    U8 depth = 0;
    U8 bound = 0;
    U8 age = 0;
    int16_t score = 0;
    Move move;

    void decode(U64 dk) {
        U8 f = U8(dk & 0xFF);
        U8 t = U8((dk >> 8) & 0xFF);
        auto type = static_cast<Movetype>((dk >> 16) & 0xFF);
        bound = U8((dk >> 26) & 0xF);
        depth = U8((dk >> 30) & 0xFF);
        score = static_cast<int16_t>((dk >> 38) & 0xFFFF);
        int sign = static_cast<int>((dk >> 54) & 1);
        if (sign)
            score = static_cast<int16_t>(-score);
        age = U8((dk >> 55) & 0xFF);
        move.set(f, t, type);
    }
};

// ─── Cluster ────────────────────────────────────────────────────────────────

constexpr unsigned cluster_size = 4;

struct hash_cluster {
    entry cluster_entries[cluster_size];
};

// ─── Hash table ─────────────────────────────────────────────────────────────

class hash_table {
    size_t sz_mb_ = 0;
    size_t cluster_count_ = 0;
    U8 generation_ = 0;
    std::unique_ptr<hash_cluster[]> entries_;

    void alloc(size_t size_mb);

    /// Number of searches elapsed since `age` was written, accounting for the
    /// 8-bit wraparound of the generation counter.
    [[nodiscard]] int relative_age(U8 age) const {
        return static_cast<int>(static_cast<U8>(generation_ - age));
    }

    /// Eviction priority: deep, recently written entries are worth keeping.
    [[nodiscard]] int entry_value(const entry* e) const {
        return static_cast<int>(e->depth()) - 8 * relative_age(e->age());
    }

  public:
    hash_table();
    hash_table(const hash_table&) = delete;
    hash_table(hash_table&&) = delete;
    ~hash_table() = default;

    hash_table& operator=(const hash_table&) = delete;
    hash_table& operator=(hash_table&&) = delete;

    void save(U64 key, U8 depth, U8 bound, const Move& m, int16_t score, bool pv_node);
    bool fetch(U64 key, hash_data& e);
    void clear();
    /// Returns false and keeps the current table if the requested size cannot
    /// be allocated.
    [[nodiscard]] bool resize(size_t size_mb);

    /// Bumps the generation counter; call once at the start of every search so
    /// that entries from previous searches become preferred replacement victims.
    void new_search() { ++generation_; }

    [[nodiscard]] U8 generation() const { return generation_; }

    /// Size the table was last given, in MB.
    [[nodiscard]] size_t size_mb() const { return sz_mb_; }
    [[nodiscard]] int hashfull() const;

    [[nodiscard]] inline entry* first_entry(U64 key) {
        return &entries_[key & (cluster_count_ - 1)].cluster_entries[0];
    }
};

} // namespace havoc
