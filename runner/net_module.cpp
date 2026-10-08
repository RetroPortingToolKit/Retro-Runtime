#include "net_module.hpp"

#if defined(RETRO_RUNNER_NETPLAY_MODULE)

#include <dlfcn.h>

#include <mutex>

extern "C" {
#include "recomp_net/module.h"
#include "recomp_net/rb_driver.h"
#include "recomp_net/session.h"
}

#include "net_module_api.hpp"

namespace retro::runner {

NetModuleApi g_nm{};

namespace {
std::mutex g_mu;
NetModuleInfo g_info;
bool g_loaded = false;
} // namespace

const NetModuleInfo* net_module_info() { return g_loaded ? &g_info : nullptr; }
std::uint32_t net_module_abi_required() { return RNET_MODULE_ABI_VERSION; }

bool net_module_load(const std::string& path, std::string* error) {
    std::lock_guard<std::mutex> lock(g_mu);
    auto fail = [&](const std::string& why) {
        if (error) *error = "netplay module " + path + ": " + why;
        return false;
    };
    if (path.empty()) {
        if (error) *error = "netplay unavailable: no netplay module (--net-module or RETRO_NETPLAY_MODULE)";
        return false;
    }
    if (g_loaded) {
        if (g_info.path == path) return true;
        return fail("a different module (" + g_info.path + ") is already loaded");
    }
    void* h = dlopen(path.c_str(), RTLD_NOW | RTLD_LOCAL);
    if (!h) {
        const char* e = dlerror();
        return fail(e ? e : "dlopen failed");
    }
    auto bail = [&](const std::string& why) {
        dlclose(h);
        return fail(why);
    };
    auto info_fn = reinterpret_cast<size_t (*)(RNetModuleInfo*, size_t)>(dlsym(h, "rnet_module_info"));
    if (!info_fn) return bail("has no rnet_module_info (not a netplay module)");
    RNetModuleInfo m{};
    const size_t got = info_fn(&m, sizeof m);
    if (const char* why = rnet_module_check(&m, got, RNET_MODULE_FEATURE_RBENGINE)) return bail(why);

    NetModuleApi api{};
    bool ok = true;
    std::string missing;
#define RETRO_NM_SYM(n)                                                        \
    api.n = reinterpret_cast<decltype(api.n)>(dlsym(h, #n));                    \
    if (!api.n) { ok = false; missing += " " #n; }
    RETRO_NET_MODULE_SYMBOLS(RETRO_NM_SYM)
#undef RETRO_NM_SYM
    if (!ok) return bail("missing symbols:" + missing);

    g_nm = api;
    g_info = NetModuleInfo{path, m.abi_version, m.abi_minor, m.wire_version, m.features,
                           m.version_major, m.version_minor, m.version_patch,
                           (got >= sizeof m && m.build_id) ? m.build_id : "unknown"};
    g_loaded = true;
    return true;
}

} // namespace retro::runner

#else

namespace retro::runner {
bool net_module_load(const std::string&, std::string* error) {
    if (error) *error = "this runner was not built with the netplay module (RETRO_RUNTIME_NETPLAY_MODULE)";
    return false;
}
const NetModuleInfo* net_module_info() { return nullptr; }
std::uint32_t net_module_abi_required() { return 0; }
} // namespace retro::runner

#endif
