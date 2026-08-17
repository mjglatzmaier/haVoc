#pragma once

/// @file net_path.hpp
/// @brief Finding the evaluation network without being told where it is.
///
/// The engine ships as a binary and a separate ~21 MB network file. Requiring
/// the user to set `EvalFile` before the network is used makes the handcrafted
/// evaluation the silent default, which is roughly 160 Elo weaker -- a mistake
/// that produces a working but much worse engine and no error message. So the
/// binary looks for its network on startup in the places a network is likely
/// to be, and says loudly which one it found, or that it found none.
///
/// Networks are named after their own content: `nn-<first 12 hex of sha256>.nnue`.
/// The name is therefore immutable and self-verifying -- two files with the same
/// name are the same file, and a truncated download cannot masquerade as a good
/// one. `HAVOC_DEFAULT_NET` is the name the binary was built to expect.

#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace havoc::net {

/// The network filename this binary was built against, e.g. "havoc-69c7d05e4298.nnue".
/// Empty if the build did not specify one, in which case discovery is skipped
/// and the handcrafted evaluation is used unless `EvalFile` says otherwise.
[[nodiscard]] std::string_view default_net_name();

/// Directory containing the running executable, or empty if it cannot be
/// determined. Used so that a network sitting next to the binary is found
/// regardless of the working directory the GUI happens to launch from.
[[nodiscard]] std::string executable_dir();

/// Every path that would be accepted, in the order they are tried. Exposed
/// so that the startup message can explain where it looked when it fails,
/// and so the search order can be tested without touching the filesystem.
///
/// Order, most specific first:
///   1. `$HAVOC_EVAL_FILE`             -- an explicit override, used verbatim
///   2. `$HAVOC_NET_DIR/<name>`        -- an explicitly configured directory
///   3. `<executable dir>/<name>`      -- shipped alongside the binary
///   4. `<executable dir>/nets/<name>`
///   5. `<cwd>/<name>`                 -- the usual place while developing
///   6. `<cwd>/nets/<name>`            -- where fetch-net.sh installs by default
///   7. `<user data dir>/havoc/<name>` -- installed once, found from anywhere
[[nodiscard]] std::vector<std::string> candidate_paths(std::string_view name);

/// First candidate that exists and is a regular file, if any.
[[nodiscard]] std::optional<std::string> find_default_net();

} // namespace havoc::net
