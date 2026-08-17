# ── Fetch Google Test ─────────────────────────────────────────────────────────
include(FetchContent)

FetchContent_Declare(
    googletest
    GIT_REPOSITORY https://github.com/google/googletest.git
    GIT_TAG        v1.14.0
)

# Prevent GTest from overriding parent project compiler/linker settings on Windows
set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(googletest)

# ── Test Executable ──────────────────────────────────────────────────────────
set(HAVOC_TEST_SOURCES
    test_smoke.cpp
    test_perft.cpp
    test_position.cpp
    test_eval.cpp
    test_eval_discrimination.cpp
    test_incremental_eval.cpp
    test_nnue_features.cpp
    test_nnue_network.cpp
    test_nnue_evaluator.cpp
    test_search.cpp
    test_tablebase.cpp
)

add_executable(havoc_tests ${HAVOC_TEST_SOURCES})

target_link_libraries(havoc_tests
    PRIVATE
        havoc_core
        GTest::gtest_main
)

# Register tests with CTest
include(GoogleTest)
gtest_discover_tests(havoc_tests)
