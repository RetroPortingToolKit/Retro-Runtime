#pragma once

// The runner's LINK mode: the hub spawned it holding the control channel and
// the shared region (corelink/transport.hpp says where each OS puts them).

#include "core_library.hpp"
#include "core_manifest.hpp"
#include "host_session.hpp"
#include "net_session.hpp"

#include <map>
#include <optional>
#include <array>
#include <cstddef>
#include <string>
#include <vector>

namespace retro::runner {

// Seats that can carry a Transfer Pak: the N64's four controller ports
// (`transfer_pak_seats` in --version; --tpak1-rom .. --tpak4-rom).
constexpr std::size_t kTransferPakSeats = 4;

// One n64.transfer_pak binding per seat that has a cartridge (slot 0: one pak
// per controller, rcore.h / docs/CORE_ABI.md). The strings must outlive them.
std::vector<rcore_accessory_binding> transfer_pak_bindings(
    const std::array<std::string, kTransferPakSeats>& roms);

struct LinkArgs {
    std::string rom;
    std::string package;     // --package; empty for a core without game_package
    std::string package_sha256; // the runner's hash of it, for savestate envelopes
    std::string title_dir = ".";
    fs::path out;            // session dir: core.log, events.tsv, the core's cache_dir
    bool gl = false;
    bool strict = false;
    std::map<std::string, std::string> overrides;
    std::optional<fs::path> load_state;
    // --tpakN-rom: the Game Boy cartridge in seat N's Transfer Pak (N = 1-4),
    // empty for a seat without one.
    std::array<std::string, kTransferPakSeats> tpak_roms;
    std::string link_handles; // --link-handles (Windows)
    // Netplay (--net-*): the hub grants frames with the LOCAL player's pad in
    // seat 0; the session supplies every seat's published row.
    bool netplay = false;
    NetParams net;
};

// Runs the session until the hub sends Quit or goes away. Returns the
// process exit code (the same table as headless mode).
int run_link_mode(const LoadedCore& core, const CoreManifest& manifest, const LinkArgs& args,
                  void (*lend_gl)(HostSession&));

} // namespace retro::runner
