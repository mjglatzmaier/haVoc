#pragma once

#include "havoc/types.hpp"

#include <array>
#include <cassert>
#include <cstdint>

namespace havoc {

/// What changed on the board when the last move was made.
///
/// An NNUE-style evaluator is affordable only because its first layer is kept
/// in an accumulator and updated by adding and subtracting the few columns a
/// move touches. That requires the board to be able to answer, cheaply and
/// exactly, *which (colour, piece, square) features appeared and disappeared*.
/// This is that answer, and nothing more.
///
/// Deliberately it says what changed, never what it means: the mapping from
/// these events to feature indices, king buckets and output buckets belongs to
/// the evaluator, so changing the network topology does not touch move-making.
/// See `docs/nnue-integration.md`.
///
/// This is a log of the most recent mutation, not part of the board state. It
/// is non-empty only immediately after a move was made: `undo_move` and
/// `undo_null_move` empty it again, because undoing runs the same primitives
/// and would otherwise leave the inverse events visible, and an accumulator
/// pops its own stack rather than replaying events backwards.
struct FeatureDelta {
    struct Event {
        Color c;
        Piece p;
        Square sq;
        bool added; ///< true if the feature appeared, false if it disappeared.
    };

    /// Four is the exact worst case, not a guess. A piece moving is a remove
    /// plus an add, so castling -- two pieces moving -- is four events, and
    /// nothing else reaches it: promotion-capture is three (remove the captured
    /// piece, remove the pawn, add the promoted piece).
    static constexpr int max_events = 4;

    std::array<Event, max_events> events{};

    /// Number of events recorded. A value greater than `max_events` means a
    /// caller recorded more than one mutation without clearing, which is a bug
    /// in the caller; the surplus events are dropped rather than written past
    /// the buffer, so it stays detectable instead of corrupting the board state
    /// that sits next to it in memory.
    int n = 0;

    void clear() { n = 0; }

    void add(const Color& c, const Piece& p, const Square& sq) { record(c, p, sq, true); }

    void remove(const Color& c, const Piece& p, const Square& sq) { record(c, p, sq, false); }

    const Event* begin() const { return events.data(); }
    const Event* end() const { return events.data() + (n < max_events ? n : max_events); }

  private:
    void record(const Color& c, const Piece& p, const Square& sq, bool added) {
        assert(n < max_events);
        if (n < max_events)
            events[static_cast<std::size_t>(n)] = Event{c, p, sq, added};
        ++n;
    }
};

} // namespace havoc
