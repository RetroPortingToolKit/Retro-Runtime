#pragma once

// Player savestates, as the runner keeps them: the core's bytes inside an
// envelope recording everything they are valid against, written and checked
// here because the runner holds every identity the load rule compares
// (docs/CORE_ABI.md, "Savestates"; state/state_envelope.hpp).

#include "core_library.hpp"
#include "host_session.hpp"
#include "state_envelope.hpp"

#include <cstdint>
#include <future>
#include <string>
#include <vector>

namespace retro::runner {

class StateKeeper {
public:
    // `rom` and each binding's content are hashed the first time an identity
    // is needed, unless hash_in_background() started it sooner.
    StateKeeper(const LoadedCore& core, const HostSession& session, std::string rom,
                std::string package_sha256, const std::vector<rcore_accessory_binding>& bindings);
    ~StateKeeper();

    // Hash the content now, on another thread, so the player's first save does
    // not stall on it (a CD image is hundreds of megabytes). It only reads
    // files: the core is never called from that thread.
    void hash_in_background();

    // The identity this session's states carry and are checked against.
    const state::StateIdentity& identity();

    // Serializes now -- the caller is at a frame boundary -- and writes the
    // envelope at `path`. `thumb` is kThumbWidth x kThumbHeight RGBA8, or empty.
    bool save(const fs::path& path, std::uint64_t frame_number, const std::vector<std::uint8_t>& thumb,
              std::uint64_t* bytes, std::string* detail);

    // Checks the envelope at `path` by the load rule, then unserializes. A
    // refusal leaves the machine untouched and names the first mismatch.
    // `allow_bare`: a file without an envelope goes to the core as it is --
    // the bare states n64lle's gates and headless --load-state have always
    // used. Never over the hub's menu, whose slots are all envelopes.
    bool load(const fs::path& path, bool allow_bare, std::uint64_t* bytes, std::string* detail);

private:
    struct Accessory {
        std::uint32_t seat, slot;
        std::string type_id, content_path;
    };

    const LoadedCore& core_;
    const HostSession& session_;
    std::string rom_, package_sha_;
    std::vector<Accessory> accessories_;
    struct ContentHashes {
        std::string content;
        std::vector<std::string> accessories; // one per accessories_ entry
    };
    ContentHashes hash_content() const;

    bool have_identity_ = false;
    state::StateIdentity identity_;
    std::future<ContentHashes> hashing_;
};

} // namespace retro::runner
