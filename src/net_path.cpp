#include "havoc/net_path.hpp"

#include <cstdlib>
#include <filesystem>

#if defined(_WIN32)
#include <windows.h>
#elif defined(__APPLE__)
#include <mach-o/dyld.h>
#include <vector>
#else
#include <unistd.h>
#endif

namespace havoc::net {
namespace {

/// Reads an environment variable, treating "set but empty" as unset. An empty
/// value would otherwise turn into a path like "/nn-....nnue".
std::optional<std::string> env(const char* key) {
#if defined(_MSC_VER)
    // getenv is deprecated under MSVC and _dupenv_s is the sanctioned form.
    char* buf = nullptr;
    size_t len = 0;
    if (_dupenv_s(&buf, &len, key) != 0 || buf == nullptr)
        return std::nullopt;
    std::string value(buf);
    free(buf);
    if (value.empty())
        return std::nullopt;
    return value;
#else
    const char* raw = std::getenv(key);
    if (raw == nullptr || *raw == '\0')
        return std::nullopt;
    return std::string(raw);
#endif
}

} // namespace

std::string_view default_net_name() {
#if defined(HAVOC_DEFAULT_NET)
    return HAVOC_DEFAULT_NET;
#else
    return {};
#endif
}

std::string executable_dir() {
    std::error_code ec;
#if defined(_WIN32)
    std::wstring buf(MAX_PATH, L'\0');
    for (;;) {
        const DWORD n = GetModuleFileNameW(nullptr, buf.data(), static_cast<DWORD>(buf.size()));
        if (n == 0)
            return {};
        // Truncation is reported by filling the buffer exactly; grow and retry
        // rather than silently using a clipped path.
        if (n < buf.size()) {
            buf.resize(n);
            break;
        }
        if (buf.size() > 32768)
            return {};
        buf.resize(buf.size() * 2);
    }
    const std::filesystem::path exe(buf);
#elif defined(__APPLE__)
    std::uint32_t size = 0;
    _NSGetExecutablePath(nullptr, &size); // asks for the required size
    std::vector<char> buf(size + 1, '\0');
    if (_NSGetExecutablePath(buf.data(), &size) != 0)
        return {};
    const std::filesystem::path exe(buf.data());
#else
    const std::filesystem::path link("/proc/self/exe");
    const std::filesystem::path exe = std::filesystem::read_symlink(link, ec);
    if (ec)
        return {};
#endif
    const std::filesystem::path dir = std::filesystem::absolute(exe, ec).parent_path();
    if (ec)
        return {};
    return dir.string();
}

std::vector<std::string> candidate_paths(std::string_view name) {
    std::vector<std::string> out;
    if (const auto explicit_file = env("HAVOC_EVAL_FILE"))
        out.push_back(*explicit_file);

    if (name.empty())
        return out;

    const std::filesystem::path leaf(name);
    const auto push = [&out, &leaf](const std::filesystem::path& dir) {
        if (dir.empty())
            return;
        out.push_back((dir / leaf).string());
        out.push_back((dir / "nets" / leaf).string());
    };

    push(executable_dir());

    std::error_code ec;
    const std::filesystem::path cwd = std::filesystem::current_path(ec);
    if (!ec)
        push(cwd);

    if (const auto dir = env("HAVOC_NET_DIR"))
        out.push_back((std::filesystem::path(*dir) / leaf).string());

    // Platform convention for per-user application data. This is where
    // scripts/nnue/fetch-net.sh installs, so a fetched network is found by a
    // binary anywhere on the system.
#if defined(_WIN32)
    if (const auto appdata = env("LOCALAPPDATA"))
        out.push_back((std::filesystem::path(*appdata) / "havoc" / leaf).string());
#else
    if (const auto xdg = env("XDG_DATA_HOME"))
        out.push_back((std::filesystem::path(*xdg) / "havoc" / leaf).string());
    else if (const auto home = env("HOME"))
        out.push_back((std::filesystem::path(*home) / ".local" / "share" / "havoc" / leaf).string());
#endif
    return out;
}

std::optional<std::string> find_default_net() {
    std::error_code ec;
    for (const auto& path : candidate_paths(default_net_name())) {
        if (std::filesystem::is_regular_file(path, ec))
            return path;
    }
    return std::nullopt;
}

} // namespace havoc::net
