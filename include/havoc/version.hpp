#pragma once

/// @file version.hpp
/// @brief Compile-time version information for haVoc chess engine.

#include <string_view>

namespace havoc {

inline constexpr int VERSION_MAJOR = 2;
inline constexpr int VERSION_MINOR = 1;
inline constexpr int VERSION_PATCH = 0;
inline constexpr std::string_view VERSION_STRING = "2.2.0";
inline constexpr std::string_view ENGINE_NAME = "haVoc";
inline constexpr std::string_view ENGINE_AUTHOR = "M.Glatzmaier";

} // namespace havoc
