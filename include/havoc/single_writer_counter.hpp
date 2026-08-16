#pragma once

#include "havoc/types.hpp"

#include <atomic>

namespace havoc {

/// A counter written by exactly one thread and read by many.
///
/// The search's node counters live on each thread's own position and are only
/// ever incremented by that thread, but they are summed *from other threads*:
/// by the node-limit check, which every thread evaluates over every thread's
/// count, and by the UCI loop reporting live nps while the search runs. Reading
/// a plain U64 that another thread is writing is a data race, and therefore
/// undefined behaviour, however benign the generated code happens to look --
/// the compiler is entitled to assume nobody else touches the object.
///
/// Relaxed atomics fix that for free, but only if the increment is written
/// carefully. `fetch_add` is a read-modify-write and lowers to a LOCK XADD on
/// x86: tens of cycles, on a line other threads are reading, in the hottest
/// loop in the engine. With a single writer no atomic RMW is needed -- nothing
/// else can change the value between the load and the store -- so `add()` is a
/// deliberately separate relaxed load and relaxed store, which is a pair of
/// plain MOVs.
///
/// What readers get is *a* value the counter held at some point. A sum across
/// threads is not a consistent snapshot of anything. Both consumers only need
/// an approximation, so that is fine; do not build an exactness requirement on
/// top of this type.
///
/// Copy and move are value copies rather than the deleted ones std::atomic
/// would otherwise impose on every enclosing class.
class single_writer_counter {
  public:
    single_writer_counter() = default;
    ~single_writer_counter() = default;
    single_writer_counter(const single_writer_counter& o) noexcept { set(o.get()); }
    single_writer_counter(single_writer_counter&& o) noexcept { set(o.get()); }
    single_writer_counter& operator=(const single_writer_counter& o) noexcept {
        set(o.get());
        return *this;
    }
    single_writer_counter& operator=(single_writer_counter&& o) noexcept {
        set(o.get());
        return *this;
    }

    /// Assignment from a raw count, so the type drops into existing code.
    single_writer_counter& operator=(U64 n) noexcept {
        set(n);
        return *this;
    }

    [[nodiscard]] U64 get() const noexcept { return v_.load(std::memory_order_relaxed); }
    void set(U64 n) noexcept { v_.store(n, std::memory_order_relaxed); }
    void add(U64 dn) noexcept { set(get() + dn); }

    /// Only the owning thread may call this.
    single_writer_counter& operator++() noexcept {
        add(1);
        return *this;
    }

  private:
    std::atomic<U64> v_{0};
};

} // namespace havoc
