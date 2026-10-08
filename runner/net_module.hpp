// The netplay module (recomp-net include/recomp_net/module.h), opened at run
// time. In a module build (RETRO_RUNNER_NETPLAY_MODULE) net_session.cpp calls
// recomp-net through the pointers resolved here instead of linking it, so the
// netcode is versioned and shipped apart from the runner and the cores. The
// retcomm-launcher owns fetching and verifying the file; this only opens it.
#pragma once

#include <cstdint>
#include <string>

namespace retro::runner {

struct NetModuleInfo {
    std::string path;
    std::uint32_t abi_version = 0, abi_minor = 0, wire_version = 0, features = 0;
    std::uint32_t major = 0, minor = 0, patch = 0;
    std::string build_id;
};

// Opens `path` once per process and checks it: module ABI, rbengine, and every
// function the runner calls. Returns false with *error saying which check
// failed. No fallback: a runner with no usable module refuses --net-*.
bool net_module_load(const std::string& path, std::string* error);
const NetModuleInfo* net_module_info(); // null until loaded
// The module ABI this runner was built for (recomp-net RNET_MODULE_ABI_VERSION),
// or 0 when netplay is linked in or absent. Reported by --version so a host
// can pick a module whose ABI matches without opening it.
std::uint32_t net_module_abi_required();

} // namespace retro::runner
