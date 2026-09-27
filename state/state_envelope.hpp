#pragma once

// The savestate envelope (docs/CORE_ABI.md, "Savestates"): the core's own
// bytes, wrapped in a record of everything they are valid against. The
// envelope is a host format, not ABI -- a core never sees it.
//
// The runner writes and checks envelopes (it holds every identity the load
// rule names); a host only reads their headers, to list slots with a picture,
// a time and a hint of whether they will load.
//
// File layout, little-endian:
//
//   magic      8 bytes  "RCSTATE\0"
//   version    u32      kEnvelopeVersion
//   header     u32 length, then UTF-8 `key=value` lines (see state_envelope.cpp)
//   thumbnail  u64 length, then RGBA8 bytes (thumb_w * thumb_h * 4), or 0
//   state      u64 length, then the core's bytes
//
// A text header so a later version can add a field without a reader of this
// one refusing it: unknown keys are kept and ignored.

#include <cstdint>
#include <filesystem>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace retro::state {

namespace fs = std::filesystem;

constexpr char kEnvelopeMagic[8] = {'R', 'C', 'S', 'T', 'A', 'T', 'E', '\0'};
constexpr std::uint32_t kEnvelopeVersion = 1;
// Thumbnails are stored at this size, whatever the picture was.
constexpr std::uint32_t kThumbWidth = 160;
constexpr std::uint32_t kThumbHeight = 120;

struct AccessoryIdentity {
    std::uint32_t seat = 0, slot = 0;
    std::string type_id;
    std::string content_sha256; // empty = the accessory takes no content
    bool operator==(const AccessoryIdentity& o) const {
        return seat == o.seat && slot == o.slot && type_id == o.type_id &&
               content_sha256 == o.content_sha256;
    }
};

// Everything the load rule compares, in its order.
struct StateIdentity {
    std::uint32_t abi_major = 0;
    std::string core_id;
    std::string core_sha256;                    // the host's hash of the loaded library
    std::optional<std::string> state_compat_id; // the core's promise, if it made one
    std::string package_sha256;                 // CAP_GAME_PACKAGE; empty otherwise
    std::string content_sha256;
    std::vector<AccessoryIdentity> accessories; // every binding
    // Every RCORE_OPT_FLAG_NETPLAY option, resolved; nullopt = unset.
    std::map<std::string, std::optional<std::string>> sim_options;
};

struct StateHeader {
    std::uint32_t version = kEnvelopeVersion;
    StateIdentity identity;
    std::uint64_t state_size = 0;
    std::string state_sha256;
    // For display only.
    std::uint64_t frame_number = 0;
    std::int64_t saved_unix = 0; // seconds
    std::uint32_t thumb_w = 0, thumb_h = 0;
};

// A picture shrunk to the thumbnail size (nearest neighbour), RGBA8, opaque.
std::vector<std::uint8_t> make_thumbnail(const std::uint8_t* rgba, std::uint32_t width,
                                         std::uint32_t height, std::uint32_t stride);

// Writes beside, then renames, so a crash never leaves half a state where a
// good one was. `thumb` is kThumbWidth x kThumbHeight RGBA8, or empty.
// header.state_size / state_sha256 / thumb_* are filled in here.
bool write_state(const fs::path& path, StateHeader header, const std::vector<std::uint8_t>& thumb,
                 const void* state, std::uint64_t size, std::string* error);

// True when the file begins with the envelope magic. A file that does not is
// a bare core state (what n64lle's gates and `--load-state` have always used).
bool is_envelope(const fs::path& path);

// The header, and the thumbnail when `thumb` is given -- never the state
// bytes, so listing twelve slots of an 8 MB machine reads a few KB each.
bool read_state_header(const fs::path& path, StateHeader& out, std::vector<std::uint8_t>* thumb,
                       std::string* error);

// The whole envelope.
bool read_state(const fs::path& path, StateHeader& out, std::vector<std::uint8_t>& state,
                std::string* error);

// The load rule (docs/CORE_ABI.md): refuse, never attempt. Checks in the
// rule's order and stops at the first mismatch, naming both values. Returns
// an empty string when the state may be loaded. `state` is the core's bytes
// as read; their hash is the last check (a corrupt file).
std::string check_state(const StateHeader& saved, const StateIdentity& running,
                        const std::vector<std::uint8_t>& state);

} // namespace retro::state
