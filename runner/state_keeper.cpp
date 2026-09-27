#include "state_keeper.hpp"

#include "sha256.hpp"
#include "transport.hpp"

#include <ctime>
#include <fstream>
#include <iterator>

namespace retro::runner {

StateKeeper::StateKeeper(const LoadedCore& core, const HostSession& session, std::string rom,
                         std::string package_sha256,
                         const std::vector<rcore_accessory_binding>& bindings)
    : core_(core), session_(session), rom_(std::move(rom)), package_sha_(std::move(package_sha256)) {
    for (const rcore_accessory_binding& b : bindings) {
        accessories_.push_back({b.seat, b.slot, b.type_id ? b.type_id : "",
                                b.content_path ? b.content_path : ""});
    }
}

StateKeeper::~StateKeeper() {
    if (hashing_.valid()) hashing_.wait();
}

StateKeeper::ContentHashes StateKeeper::hash_content() const {
    ContentHashes h;
    h.content = file_sha256_hex(corelink::utf8_path(rom_));
    for (const Accessory& a : accessories_) {
        h.accessories.push_back(a.content_path.empty()
                                    ? std::string()
                                    : file_sha256_hex(corelink::utf8_path(a.content_path)));
    }
    return h;
}

void StateKeeper::hash_in_background() {
    if (have_identity_ || hashing_.valid()) return;
    hashing_ = std::async(std::launch::async, [this] { return hash_content(); });
}

const state::StateIdentity& StateKeeper::identity() {
    if (have_identity_) return identity_;
    const ContentHashes hashes = hashing_.valid() ? hashing_.get() : hash_content();
    const rcore_core_info& info = *core_.info;
    state::StateIdentity& id = identity_;
    id.abi_major = info.abi_major;
    id.core_id = info.core_id ? info.core_id : "";
    id.core_sha256 = core_.sha256;
    if (RCORE_HAS(&info, rcore_core_info, state_compat_id) && info.state_compat_id) {
        id.state_compat_id = info.state_compat_id;
    }
    id.package_sha256 = package_sha_;
    id.content_sha256 = hashes.content;
    for (std::size_t i = 0; i < accessories_.size(); ++i) {
        state::AccessoryIdentity ai;
        ai.seat = accessories_[i].seat;
        ai.slot = accessories_[i].slot;
        ai.type_id = accessories_[i].type_id;
        ai.content_sha256 = hashes.accessories[i];
        id.accessories.push_back(ai);
    }
    // Only the options that change what the machine computes: the ones the
    // core flagged NETPLAY. A renderer choice does not invalidate a state.
    std::uint32_t n = 0;
    const rcore_option* opts = core_.api->options ? core_.api->options(&n) : nullptr;
    for (std::uint32_t i = 0; opts && i < n; ++i) {
        if (!(opts[i].flags & RCORE_OPT_FLAG_NETPLAY) || !opts[i].key) continue;
        const auto it = session_.options().find(opts[i].key);
        id.sim_options[opts[i].key] = it == session_.options().end() ? std::nullopt : it->second;
    }
    have_identity_ = true;
    return identity_;
}

bool StateKeeper::save(const fs::path& path, std::uint64_t frame_number,
                       const std::vector<std::uint8_t>& thumb, std::uint64_t* bytes,
                       std::string* detail) {
    const rcore_core_api& api = *core_.api;
    if (!(core_.info->capabilities & RCORE_CAP_SAVESTATE) || !api.serialize_size || !api.serialize) {
        *detail = "the core does not declare savestate";
        return false;
    }
    const std::uint64_t size = api.serialize_size();
    if (!size) {
        *detail = "the core could not size its state (serialize_size() is 0; its log says why)";
        return false;
    }
    std::vector<std::uint8_t> buf(static_cast<std::size_t>(size));
    if (const rcore_result rc = api.serialize(buf.data(), size); rc != RCORE_OK) {
        *detail = "serialize() -> " + std::to_string(rc) + " (the core's log says why)";
        return false;
    }
    state::StateHeader h;
    h.identity = identity();
    h.frame_number = frame_number;
    h.saved_unix = static_cast<std::int64_t>(std::time(nullptr));
    if (!state::write_state(path, h, thumb, buf.data(), size, detail)) return false;
    *bytes = size;
    return true;
}

bool StateKeeper::load(const fs::path& path, bool allow_bare, std::uint64_t* bytes,
                       std::string* detail) {
    const rcore_core_api& api = *core_.api;
    if (!(core_.info->capabilities & RCORE_CAP_SAVESTATE) || !api.unserialize) {
        *detail = "the core does not declare savestate";
        return false;
    }
    std::vector<std::uint8_t> buf;
    if (!state::is_envelope(path)) {
        if (!allow_bare) {
            *detail = path.string() + ": not a savestate envelope";
            return false;
        }
        std::ifstream in(path, std::ios::binary);
        if (!in) {
            *detail = path.string() + ": cannot open";
            return false;
        }
        buf.assign(std::istreambuf_iterator<char>(in), {});
    } else {
        state::StateHeader h;
        if (!state::read_state(path, h, buf, detail)) return false;
        if (std::string why = state::check_state(h, identity(), buf); !why.empty()) {
            *detail = "refused: " + why;
            return false;
        }
    }
    if (buf.empty()) {
        *detail = path.string() + ": empty";
        return false;
    }
    if (const rcore_result rc = api.unserialize(buf.data(), buf.size()); rc != RCORE_OK) {
        *detail = "the core refused the state: unserialize() -> " + std::to_string(rc) +
                  " (the core's log says why)";
        return false;
    }
    *bytes = buf.size();
    return true;
}

} // namespace retro::runner
