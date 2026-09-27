#pragma once

// What a retro-core-runner binary is, asked of the binary itself
// (retro-core-runner --version; docs/RELEASES.md). A host uses it to choose
// between runners and to check an update before trusting it: the version in a
// manifest or a directory name is a claim, the binary's own report is not.

#include "link_protocol.hpp"

#include <cstdint>
#include <filesystem>
#include <string>

namespace retro::corelink {

struct RunnerVersion {
    std::string version; // "0.1.0", or "dev" for a build outside a release
    std::string commit;
    std::uint32_t link_major = 0, link_minor = 0;
    std::uint32_t abi_major = 0, draft_revision = 0;
    bool gl = false;
    // 1 when the runner takes --package (GAME_PACKAGE cores); 0 for a runner
    // from before it did, which prints no game_package line.
    std::uint32_t game_package = 0;
    // 1 when the runner answers --describe (a core's declared options and
    // inputs, no ROM; docs/CORE_RUNNER.md); 0 for a runner from before it.
    std::uint32_t describe = 0;
    // Whether this host can drive it: the link major and the rcore ABI major
    // are this host's own.
    bool compatible() const {
        return link_major == kProtocolMajor && abi_major == RCORE_ABI_MAJOR;
    }
};

// The runner's file name on this OS: retro-core-runner, or .exe on Windows.
const char* runner_file_name();

// Runs `runner --version` (at most timeout_ms) and parses what it prints.
bool probe_runner(const std::filesystem::path& runner, RunnerVersion& out, std::string* error,
                  int timeout_ms = 5000);

} // namespace retro::corelink
