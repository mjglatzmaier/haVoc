cmake_minimum_required(VERSION 3.20)

project(haVoc
    VERSION 2.2.0
    DESCRIPTION "A modern UCI chess engine"
    HOMEPAGE_URL "https://github.com/mjglatzmaier/haVoc"
    LANGUAGES C CXX
)

# ── C++ Standard ─────────────────────────────────────────────────────────────
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# ── Options ──────────────────────────────────────────────────────────────────
option(HAVOC_ENABLE_TESTS      "Build unit tests"                     ON)
option(HAVOC_ENABLE_BENCH      "Build benchmarks"                     OFF)
option(HAVOC_ENABLE_SANITIZERS "Enable ASan/UBSan in Debug builds"    OFF)
option(HAVOC_WERROR            "Treat compiler warnings as errors"    OFF)
option(HAVOC_NATIVE            "Enable -march=native for SIMD/popcnt" ON)
option(HAVOC_SYZYGY            "Build Syzygy tablebase probing (Fathom)" ON)

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
    src/eval/nnue_evaluator.cpp
    src/move_order.cpp
    src/thread_pool.cpp
    src/search.cpp
    src/uci.cpp
    src/parameters.cpp
    src/tablebase.cpp
    src/book.cpp
)

add_library(havoc_core STATIC ${HAVOC_CORE_SOURCES})

# ── Syzygy tablebases (Fathom, vendored, MIT) ────────────────────────────────
# Third-party C sources are their own target: they are not ours to keep clean,
# so havoc_set_warnings deliberately does not touch them. tbchess.c is included
# by tbprobe.c rather than compiled separately, which is how Fathom is built.
if(HAVOC_SYZYGY)
    add_library(fathom STATIC third_party/fathom/tbprobe.c)
    target_include_directories(fathom PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/third_party/fathom>)
    set_target_properties(fathom PROPERTIES C_STANDARD 11 POSITION_INDEPENDENT_CODE ON)
    if(NOT MSVC)
        target_compile_options(fathom PRIVATE -w)
    endif()
    find_package(Threads REQUIRED)
    target_link_libraries(fathom PUBLIC Threads::Threads)
    target_link_libraries(havoc_core PUBLIC fathom)
    target_compile_definitions(havoc_core PUBLIC HAVOC_SYZYGY=1)
endif()

# The commit a binary was built from, regenerated on every build so it cannot
# go stale. Anything that writes a training corpus stamps it into the sidecar,
# which is the only way a corpus stays traceable to the evaluation that
# labelled it.
set(HAVOC_BUILD_INFO_DIR "${CMAKE_CURRENT_BINARY_DIR}/generated/havoc")
file(MAKE_DIRECTORY "${HAVOC_BUILD_INFO_DIR}")
add_custom_target(havoc_build_info
    COMMAND ${CMAKE_COMMAND}
            -DHAVOC_GIT_DIR=${CMAKE_CURRENT_SOURCE_DIR}
            -DHAVOC_OUTPUT=${HAVOC_BUILD_INFO_DIR}/build_info.hpp
            -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/GitInfo.cmake
    BYPRODUCTS ${HAVOC_BUILD_INFO_DIR}/build_info.hpp
    COMMENT "Recording the build's git commit")
# Generate it once now too, so a first configure+compile has a header to find.
execute_process(COMMAND ${CMAKE_COMMAND}
    -DHAVOC_GIT_DIR=${CMAKE_CURRENT_SOURCE_DIR}
    -DHAVOC_OUTPUT=${HAVOC_BUILD_INFO_DIR}/build_info.hpp
    -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/GitInfo.cmake)
add_dependencies(havoc_core havoc_build_info)

target_include_directories(havoc_core
    PUBLIC  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
            $<BUILD_INTERFACE:${CMAKE_CURRENT_BINARY_DIR}/generated>
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

    add_executable(havoc_eval_bench tools/eval_bench.cpp)
    target_link_libraries(havoc_eval_bench PRIVATE havoc_core)
    target_include_directories(havoc_eval_bench PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)
    havoc_set_warnings(havoc_eval_bench)

    add_executable(havoc_nnue_export tools/nnue_export.cpp)
    target_link_libraries(havoc_nnue_export PRIVATE havoc_core)
    target_include_directories(havoc_nnue_export PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)
    havoc_set_warnings(havoc_nnue_export)
endif()
