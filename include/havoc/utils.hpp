#pragma once

/// @file utils.hpp
/// @brief General utility functions for the haVoc chess engine.

#include "havoc/types.hpp"

#include <chrono>
#include <cmath>
#include <cstdlib>
#include <random>
#include <sstream>
#include <string>
#include <vector>

namespace havoc::util {

/// @name Square Utilities
/// @brief Extract row/column information from square indices.
/// @{
[[nodiscard]] constexpr int row(int s) {
    return s >> 3;
}
[[nodiscard]] constexpr int col(int s) {
    return s & 7;
}
/// std::abs is not constexpr until C++23. GCC and Clang accept it in a
/// constant expression as an extension; MSVC does not, which broke the
/// Windows build. Subtract and negate by hand instead.
[[nodiscard]] constexpr int row_dist(int s1, int s2) {
    const int d = row(s1) - row(s2);
    return d < 0 ? -d : d;
}
[[nodiscard]] constexpr int col_dist(int s1, int s2) {
    const int d = col(s1) - col(s2);
    return d < 0 ? -d : d;
}
[[nodiscard]] constexpr bool on_board(int s) {
    return s >= 0 && s <= 63;
}
[[nodiscard]] constexpr bool same_row(int s1, int s2) {
    return row(s1) == row(s2);
}
[[nodiscard]] constexpr bool same_col(int s1, int s2) {
    return col(s1) == col(s2);
}
[[nodiscard]] constexpr bool on_diagonal(int s1, int s2) {
    return col_dist(s1, s2) == row_dist(s1, s2);
}
[[nodiscard]] constexpr bool aligned(int s1, int s2) {
    return on_diagonal(s1, s2) || same_row(s1, s2) || same_col(s1, s2);
}
[[nodiscard]] constexpr bool aligned(int s1, int s2, int s3) {
    return (same_col(s1, s2) && same_col(s1, s3)) || (same_row(s1, s2) && same_row(s1, s3)) ||
           (on_diagonal(s1, s3) && on_diagonal(s1, s2) && on_diagonal(s2, s3));
}
/// @}

/// @brief Returns the squares in front of square `s` on the given column bitboard.
[[nodiscard]] inline U64 squares_infront(U64 colbb, Color c, int s) {
    return c == white ? colbb << (8 * (row(s) + 1)) : colbb >> (8 * (8 - row(s)));
}

/// @brief Returns the squares behind square `s` on the given column bitboard.
[[nodiscard]] inline U64 squares_behind(U64 colbb, Color c, int s) {
    return ~squares_infront(colbb, c, s) & colbb;
}

/// @brief Split a string by delimiter.
[[nodiscard]] inline std::vector<std::string> split(const std::string& s, char delimiter) {
    std::vector<std::string> tokens;
    std::stringstream ss(s);
    std::string t;
    while (std::getline(ss, t, delimiter)) {
        tokens.push_back(t);
    }
    return tokens;
}

/// @brief Simple high-resolution timer using steady_clock.
class Clock {
    std::chrono::time_point<std::chrono::steady_clock> start_;

  public:
    Clock() : start_(std::chrono::steady_clock::now()) {}

    /// Reset the timer.
    void reset() { start_ = std::chrono::steady_clock::now(); }

    /// @brief Returns elapsed time in milliseconds since construction or last reset.
    [[nodiscard]] double elapsed_ms() const {
        auto now = std::chrono::steady_clock::now();
        return std::chrono::duration<double, std::milli>(now - start_).count();
    }
};

/// @brief Simple random number generator.
template <typename T> class Rand {
    std::mt19937_64 gen_;
    std::uniform_int_distribution<T> dis_;

  public:
    Rand() : gen_(0), dis_(0, std::numeric_limits<T>::max()) {}
    explicit Rand(uint64_t seed) : gen_(seed), dis_(0, std::numeric_limits<T>::max()) {}

    T next() { return dis_(gen_); }
};

} // namespace havoc::util
