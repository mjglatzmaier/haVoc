cmake_minimum_required(VERSION 3.20)

project(haVoc
    VERSION 2.0.0
    DESCRIPTION "A modern UCI chess engine"
    HOMEPAGE_URL "https://github.com/mjglatzmaier/chess"
    LANGUAGES CXX
)

# ── C++ Standard ─────────────────────────────────────────────────────────────
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# ── Options ──────────────────────────────────────────────────────────────────
option(HAVOC_ENABLE_TESTS      "Build unit tests"                     ON)
option(HAVOC_ENABLE_BENCH      "Build benchmarks"                     OFF)
option(HAVOC_ENABLE_SANITIZERS "Enable ASan/UBSan in Debug builds"    OFF)
option(HAVOC_NATIVE            "Enable -march=native for SIMD/popcnt" ON)

# ── CMake Modules ────────────────────────────────────────────────────────────
include(cmake/CompilerWarnings.cmake)
include(cmake/Sanitizers.cmake)
include(cmake/StaticAnalysis.cmake)

# ── Default Build Type ───────────────────────────────────────────────────────
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    message(STATUS "No build type specified — defaulting to Release.")
    set(CMAKE_BUILD_TYPE "Release" CACHE STRING "Build type" FORCE)
    set_property(CACHE CMAKE_BUILD_TYPE PROPERTY STRINGS "Debug" "Release" "RelWithDebInfo" "MinSizeRel")
endif()

# ── Core Library ─────────────────────────────────────────────────────────────
# All engine logic lives here so it can be linked by both the executable and tests.
set(HAVOC_CORE_SOURCES
    src/bitboard.cpp
    src/magics.cpp
    src/zobrist.cpp
    src/position.cpp
    src/movegen.cpp
    src/tt.cpp
    src/pawn_table.cpp
    src/material_table.cpp
    src/kpk.cpp
    src/eval/hce.cpp
    src/move_order.cpp
    src/thread_pool.cpp
    src/search.cpp
    src/uci.cpp
    src/parameters.cpp
    src/tablebase.cpp
    src/book.cpp
)

add_library(havoc_core STATIC ${HAVOC_CORE_SOURCES})

target_include_directories(havoc_core
    PUBLIC  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src
)

target_compile_features(havoc_core PUBLIC cxx_std_20)

# Native arch optimizations (popcnt, BMI2, AVX2, NEON, etc.)
if(HAVOC_NATIVE)
    target_compile_options(havoc_core PUBLIC
        $<$<CXX_COMPILER_ID:GNU>:-march=native>
        $<$<CXX_COMPILER_ID:Clang>:-march=native>
        $<$<CXX_COMPILER_ID:AppleClang>:-march=native>
    )
    # MSVC: /arch:AVX2 is reasonable for modern x86; skip on ARM
    if(MSVC AND CMAKE_SYSTEM_PROCESSOR MATCHES "AMD64|x86_64|x86")
        target_compile_options(havoc_core PUBLIC /arch:AVX2)
    endif()
endif()

havoc_set_warnings(havoc_core)
havoc_enable_sanitizers(havoc_core)

# ── Executable ───────────────────────────────────────────────────────────────
add_executable(havoc src/main.cpp)
target_link_libraries(havoc PRIVATE havoc_core)
havoc_set_warnings(havoc)
havoc_enable_sanitizers(havoc)

# ── Tests ────────────────────────────────────────────────────────────────────
if(HAVOC_ENABLE_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()

# ── Benchmarks ───────────────────────────────────────────────────────────────
if(HAVOC_ENABLE_BENCH)
    add_subdirectory(bench)
endif()

# ── Tuning Tools ─────────────────────────────────────────────────────────────
option(HAVOC_BUILD_TOOLS "Build tuning tools (datagen, texel)" OFF)

if(HAVOC_BUILD_TOOLS)
    add_executable(havoc_datagen tools/datagen.cpp)
    target_link_libraries(havoc_datagen PRIVATE havoc_core)
    target_include_directories(havoc_datagen PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)
    havoc_set_warnings(havoc_datagen)

    add_executable(havoc_texel tools/texel_tune.cpp)
    target_link_libraries(havoc_texel PRIVATE havoc_core)
    target_include_directories(havoc_texel PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)
    havoc_set_warnings(havoc_texel)

    add_executable(havoc_pgn2epd tools/pgn2epd.cpp)
    target_link_libraries(havoc_pgn2epd PRIVATE havoc_core)
    target_include_directories(havoc_pgn2epd PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)
    havoc_set_warnings(havoc_pgn2epd)

    add_executable(havoc_eval_sensitivity tools/eval_sensitivity.cpp)
    target_link_libraries(havoc_eval_sensitivity PRIVATE havoc_core)
    target_include_directories(havoc_eval_sensitivity PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)
    havoc_set_warnings(havoc_eval_sensitivity)
endif()
