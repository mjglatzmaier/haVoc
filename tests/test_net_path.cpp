/// @file test_net_path.cpp
/// @brief Where the engine looks for its network, and in what order.
///
/// The search order is the whole point of this module: an engine that finds
/// the wrong network, or silently finds none, plays about 160 Elo below what
/// the binary is capable of and says nothing useful about why. The order is
/// therefore pinned by tests rather than left to whatever the implementation
/// happens to do.
///
/// These tests only exercise path *construction* and existence checks. They
/// deliberately do not load a network: the network is a 21 MB artifact that is
/// not in the repository, so a test that needed one could not run in CI.

#include "havoc/net_path.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>

namespace {

void set_env(const char* key, const char* value) {
#if defined(_WIN32)
    _putenv_s(key, value);
#else
    setenv(key, value, 1);
#endif
}

void clear_env(const char* key) {
#if defined(_WIN32)
    _putenv_s(key, "");
#else
    unsetenv(key);
#endif
}

/// Saves and restores every variable these tests touch, so that test order
/// cannot leak state and so a failure part-way through does not poison the
/// rest of the suite.
class NetPathEnv : public ::testing::Test {
protected:
    void SetUp() override {
        for (const char* key : keys_) {
            if (const char* value = std::getenv(key))
                saved_.emplace_back(key, value);
            clear_env(key);
        }
    }
    void TearDown() override {
        for (const char* key : keys_)
            clear_env(key);
        for (const auto& [key, value] : saved_)
            set_env(key.c_str(), value.c_str());
        saved_.clear();
    }
    static constexpr const char* keys_[] = {"HAVOC_EVAL_FILE", "HAVOC_NET_DIR"};
    std::vector<std::pair<std::string, std::string>> saved_;
};

bool contains(const std::vector<std::string>& haystack, const std::string& needle) {
    return std::find(haystack.begin(), haystack.end(), needle) != haystack.end();
}

std::size_t index_of(const std::vector<std::string>& haystack, const std::string& needle) {
    const auto it = std::find(haystack.begin(), haystack.end(), needle);
    return static_cast<std::size_t>(std::distance(haystack.begin(), it));
}

} // namespace

TEST_F(NetPathEnv, ExplicitOverrideIsTriedFirstAndUsedVerbatim) {
    set_env("HAVOC_EVAL_FILE", "/some/where/custom.nnue");
    const auto paths = havoc::net::candidate_paths("nn-000000000000.nnue");
    ASSERT_FALSE(paths.empty());
    // Verbatim: the override names a file, not a directory to search, so no
    // network name is appended to it.
    EXPECT_EQ(paths.front(), "/some/where/custom.nnue");
}

TEST_F(NetPathEnv, AnEmptyOverrideIsTreatedAsUnset) {
    // "set but empty" is what an unset shell variable expands to in a script,
    // and turning that into the path "/nn-....nnue" would be a confusing way
    // to fail.
    set_env("HAVOC_EVAL_FILE", "");
    const auto paths = havoc::net::candidate_paths("nn-000000000000.nnue");
    for (const auto& p : paths)
        EXPECT_NE(p, "");
    EXPECT_FALSE(contains(paths, std::string("nn-000000000000.nnue")));
}

TEST_F(NetPathEnv, WithoutANameOnlyTheExplicitOverrideIsOffered) {
    // A build with HAVOC_DEFAULT_NET="" should not go hunting for files whose
    // name it does not know.
    EXPECT_TRUE(havoc::net::candidate_paths("").empty());
    set_env("HAVOC_EVAL_FILE", "/x/y.nnue");
    EXPECT_EQ(havoc::net::candidate_paths("").size(), 1u);
}

TEST_F(NetPathEnv, TheExecutableDirectoryIsSearchedBeforeTheWorkingDirectory) {
    // A GUI can launch the engine from any working directory, so a network
    // shipped next to the binary must win over an unrelated file that happens
    // to share its name in the directory the GUI chose.
    //
    // The working directory is moved somewhere else for the duration: ctest
    // runs the suite *from* the executable's directory, which would make the
    // two candidates the same string and the comparison vacuous.
    const std::string name = "nn-000000000000.nnue";
    const std::filesystem::path exe_dir(havoc::net::executable_dir());
    ASSERT_FALSE(exe_dir.empty()) << "executable_dir() must work on this platform";

    const auto original = std::filesystem::current_path();
    const auto elsewhere = std::filesystem::temp_directory_path();
    ASSERT_NE(std::filesystem::canonical(elsewhere), std::filesystem::canonical(exe_dir))
        << "test needs a working directory distinct from the executable's";
    std::filesystem::current_path(elsewhere);

    const auto paths = havoc::net::candidate_paths(name);
    const std::string beside_exe = (exe_dir / name).string();
    const std::string beside_cwd = (std::filesystem::current_path() / name).string();

    std::filesystem::current_path(original);

    ASSERT_TRUE(contains(paths, beside_exe));
    ASSERT_TRUE(contains(paths, beside_cwd));
    EXPECT_LT(index_of(paths, beside_exe), index_of(paths, beside_cwd));
}

TEST_F(NetPathEnv, ANetsSubdirectoryIsSearchedInBothPlaces) {
    const std::string name = "nn-000000000000.nnue";
    const auto paths = havoc::net::candidate_paths(name);
    const std::filesystem::path exe_dir(havoc::net::executable_dir());
    EXPECT_TRUE(contains(paths, (exe_dir / "nets" / name).string()));
    EXPECT_TRUE(contains(paths, (std::filesystem::current_path() / "nets" / name).string()));
}

TEST_F(NetPathEnv, HavocNetDirIsHonoured) {
    set_env("HAVOC_NET_DIR", "/opt/havoc-nets");
    const std::string name = "nn-000000000000.nnue";
    const auto paths = havoc::net::candidate_paths(name);
    const std::string configured = (std::filesystem::path("/opt/havoc-nets") / name).string();
    ASSERT_TRUE(contains(paths, configured));

    // Setting HAVOC_NET_DIR is a statement about which network to use, so it
    // has to beat a file that merely happens to sit next to the binary.
    const std::filesystem::path exe_dir(havoc::net::executable_dir());
    ASSERT_FALSE(exe_dir.empty());
    EXPECT_LT(index_of(paths, configured), index_of(paths, (exe_dir / name).string()));
}

TEST_F(NetPathEnv, ExecutableDirectoryIsAbsoluteAndReal) {
    const std::string dir = havoc::net::executable_dir();
    ASSERT_FALSE(dir.empty());
    const std::filesystem::path p(dir);
    EXPECT_TRUE(p.is_absolute());
    EXPECT_TRUE(std::filesystem::is_directory(p));
}

TEST_F(NetPathEnv, MissingNetworkIsReportedRatherThanGuessed) {
    // Nothing by this name exists anywhere, so discovery must decline instead
    // of returning a path that does not resolve.
    set_env("HAVOC_NET_DIR", "/nonexistent-havoc-net-dir");
    const auto paths = havoc::net::candidate_paths("nn-ffffffffffff.nnue");
    std::error_code ec;
    for (const auto& p : paths)
        ASSERT_FALSE(std::filesystem::is_regular_file(p, ec)) << p << " unexpectedly exists";
}

TEST_F(NetPathEnv, AnExistingFileIsFoundThroughTheOverride) {
    const auto tmp = std::filesystem::temp_directory_path() / "havoc-net-path-test.nnue";
    {
        std::ofstream out(tmp, std::ios::binary);
        out << "not a real network";
    }
    set_env("HAVOC_EVAL_FILE", tmp.string().c_str());
    const auto found = havoc::net::find_default_net();
    // find_default_net only consults the override when it exists on disk, and
    // it does here, so it must be returned regardless of the built-in name.
    ASSERT_TRUE(found.has_value());
    EXPECT_EQ(*found, tmp.string());
    std::filesystem::remove(tmp);
}

TEST_F(NetPathEnv, ADirectoryIsNotMistakenForANetwork) {
    // is_regular_file, not exists: pointing EvalFile at a directory should
    // fall through to the next candidate rather than trying to read it.
    const auto dir = std::filesystem::temp_directory_path() / "havoc-net-path-dir";
    std::filesystem::create_directories(dir);
    set_env("HAVOC_EVAL_FILE", dir.string().c_str());
    const auto found = havoc::net::find_default_net();
    if (found)
        EXPECT_NE(*found, dir.string());
    std::filesystem::remove(dir);
}
