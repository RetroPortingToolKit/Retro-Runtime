#pragma once

// The host <-> runner link (docs/CORE_LINK.md). Shared by both sides.
//
// VERSIONING, so the runner can update separately from the hosts that start it
// (the same rules as rcore.h's ABI):
//   kProtocolMajor  changes on any incompatible edit. Host and runner must
//                   agree exactly; the runner refuses a mismatch, naming both.
//   kProtocolMinor  changes on append-only edits: a new message type, a new
//                   field at the END of a message or of SharedHeader. Each side
//                   states its minor (host in SharedHeader, runner in Hello);
//                   the session speaks min(host, runner), and neither side
//                   sends anything the other's minor does not know.
//
//   control  SOCK_SEQPACKET socketpair: one message per packet, fds passed
//            with SCM_RIGHTS, EOF when the runner dies.
//   bulk     one memfd the HUB creates: 3 frame slots and an audio ring.
//   saves    one memfd per region, created by the RUNNER after load() (the
//            sizes are only known then), sent to the hub, which maps, fills
//            and persists them -- so a runner crash cannot lose a save.
//
// Input rides inside each GRANT: every seat's pad for exactly that frame, so
// "identical within a frame" (the contract's replay rule) holds by
// construction and there is no shared input block to race.

#include "rcore/rcore.h"

#include <atomic>
#include <cstddef>
#include <cstdint>

namespace retro::corelink {

// 2.0 (2026-09-30): frame slots 2048x1536 (from 1024x1024), for a core that
// presents above its console's native raster (n64lle's internal resolution up
// to 4x is 1280x960). The shared region's layout moved, so a 1.x peer is
// refused at hello rather than reading frames at the wrong offsets.
constexpr std::uint32_t kProtocolMajor = 2;
constexpr std::uint32_t kProtocolMinor = 0;
// A link only comes up between peers of the same major, and a new major
// carries every message the old one had: savestates arrived in 1.1, so a 2.x
// peer has them whatever its minor. Gate on this, never on the minor alone.
constexpr bool link_has_savestates(std::uint32_t peer_minor) {
    return kProtocolMajor > 1 || peer_minor >= 1;
}
constexpr char kMagic[8] = {'R', 'C', 'L', 'I', 'N', 'K', '1', '\0'};

// Frame slots big enough for any frame a core presents: 640x480 at a console's
// 1x, and a core's internal resolution above that (1280x960 for n64lle at 4x).
// A frame that does not fit is a runner FAULT, not a scaled copy.
constexpr std::uint32_t kFrameSlots = 3;
constexpr std::uint32_t kMaxFrameWidth = 2048;
constexpr std::uint32_t kMaxFrameHeight = 1536;
constexpr std::size_t kFrameSlotBytes = std::size_t(kMaxFrameWidth) * kMaxFrameHeight * 4;
constexpr std::uint32_t kFresh = 0x80000000u; // `middle` holds an unread frame

// Stereo S16 frames. ~1.4 s at 48 kHz: generous, because a paused hub
// stops draining and the runner must not block on audio.
constexpr std::uint32_t kAudioCapacity = 1u << 16;

// The fixed numbers the runner finds its inherited descriptors at.
constexpr int kSocketFd = 3;
constexpr int kSharedFd = 4;

static_assert(std::atomic<std::uint32_t>::is_always_lock_free, "shared atomics must be lock-free");
static_assert(std::atomic<std::uint64_t>::is_always_lock_free, "shared atomics must be lock-free");

struct FrameInfo {
    std::uint32_t width, height, stride, pixel_format;
    std::uint32_t aspect_num, aspect_den;
    std::uint64_t frame_number;
};

// At offset 0 of the shared region. Frame pixels start at frames_offset,
// kFrameSlotBytes apart, always tightly packed (stride == width * 4).
struct SharedHeader {
    char magic[8];
    std::uint32_t protocol_major; // the host's; must equal the runner's
    std::uint32_t protocol_minor; // the host's; the session speaks min(host, runner)
    std::uint32_t header_size;    // sizeof(SharedHeader) as the host built it
    std::uint32_t _pad0;
    std::uint64_t total_size;
    std::uint64_t frames_offset;
    std::uint64_t audio_offset;

    // Frames: a lock-free triple buffer. The runner owns a BACK slot, the hub
    // owns a FRONT slot, and `middle` holds the third. The runner fills back
    // (pixels and slot[back]), then swaps it into middle with kFresh set; the
    // hub, seeing kFresh, swaps its front for middle. Each index is only ever
    // touched by its owner, so neither side can write what the other reads,
    // and neither ever waits. Initially front = 0, middle = 1, back = 2.
    std::atomic<std::uint32_t> middle;
    FrameInfo slot[kFrameSlots];

    // Audio: single producer (runner), single consumer (hub). Indices count
    // stereo frames and only grow; position = index % capacity.
    std::atomic<std::uint64_t> audio_write;
    std::atomic<std::uint64_t> audio_read;
    std::atomic<std::uint32_t> audio_rate;
    std::uint32_t audio_capacity;
    std::atomic<std::uint64_t> audio_dropped; // frames the runner could not fit

    // The core's nominal frame rate (rev 5 set_frame_rate), packed as
    // num << 32 | den so one atomic carries both; 0 = unstated.
    std::atomic<std::uint64_t> frame_rate;
};

constexpr std::size_t shared_frames_offset() {
    return (sizeof(SharedHeader) + 4095) & ~std::size_t(4095);
}
constexpr std::size_t shared_audio_offset() {
    return shared_frames_offset() + kFrameSlots * kFrameSlotBytes;
}
constexpr std::size_t shared_total_size() {
    return shared_audio_offset() + std::size_t(kAudioCapacity) * 2 * sizeof(std::int16_t);
}

// ---- control messages -------------------------------------------------------

enum class Msg : std::uint32_t {
    // runner -> hub
    Hello = 1,       // identity, after the manifest check
    SaveRegions = 2, // after load(); carries one memfd per region
    Ready = 3,       // saves adopted, state (if any) loaded: grant frames now
    FrameDone = 4,   // the granted frame ran; its picture (if any) is `latest`
    Log = 5,         // a log() line at WARN or above (all lines go to core.log)
    Event = 6,       // a report(): BRIDGE / DISPATCH_MISS / FAULT
    Exiting = 7,     // the runner is stopping on purpose; the reason follows
    StateDone = 8,   // 1.1: how the last SaveState / LoadState went
    // hub -> runner
    SavesFilled = 64, // every region filled from its file; the seats at power-on
    Grant = 65,       // run exactly one frame, with these pads
    Quit = 66,        // unload, deinit, exit 0
    SaveState = 67,   // 1.1: serialize now, into an envelope at this path
    LoadState = 68,   // 1.1: check the envelope at this path, then unserialize
};

struct MsgHeader {
    Msg type;
    std::uint32_t size; // whole packet, header included
};

struct HelloMsg {
    MsgHeader h;
    std::uint32_t protocol_major;
    std::uint32_t protocol_minor; // the runner's
    std::uint32_t abi_major;
    std::uint32_t draft_revision;
    std::uint32_t engine_dirty;
    std::uint64_t capabilities;
    char sha256[65];
    char core_id[63];
    char core_version[64];
    char platforms[64];
};

constexpr std::uint32_t kMaxRegions = 16;
struct RegionDesc {
    char id[48];
    std::uint32_t kind, seat, slot, erase_value;
    std::uint64_t size;
};
struct SaveRegionsMsg {
    MsgHeader h;
    std::uint32_t count; // fds arrive in the same order, via SCM_RIGHTS
    std::uint32_t _pad;
    RegionDesc region[kMaxRegions];
};

struct FrameDoneMsg {
    MsgHeader h;
    std::uint64_t frame_number;
    std::int32_t result; // run_frame's rcore_result
    std::uint32_t _pad;
};

struct LogMsg {
    MsgHeader h;
    std::uint32_t level;
    char text[1024]; // truncated; core.log keeps the whole line
};

struct EventMsg {
    MsgHeader h;
    std::uint32_t kind;
    std::uint32_t _pad;
    std::uint64_t frame_number;
    std::uint64_t guest_address;
    char detail[512];
};

struct ExitingMsg {
    MsgHeader h;
    std::int32_t code;
    char reason[512];
};

// The seats as they stand before the first frame. A core may read input
// outside a frame -- while unserialize() restores a state, say -- and a seat's
// connected flag is guest-visible, so the hub states it up front rather than
// leaving the runner to guess.
struct SavesFilledMsg {
    MsgHeader h;
    rcore_pad pads[RCORE_MAX_SEATS];
};

struct GrantMsg {
    MsgHeader h;
    std::uint64_t frame_number; // 1-based, as the headless runner counts
    rcore_pad pads[RCORE_MAX_SEATS];
};

struct EmptyMsg {
    MsgHeader h;
};

// 1.1. Savestates, between frames: the hub sends one only while no grant is
// outstanding, and grants nothing until StateDone. The RUNNER writes and
// checks the envelope (state/state_envelope.hpp, docs/CORE_ABI.md
// "Savestates"): it holds every identity the load rule compares. The path is
// the hub's choice, UTF-8.
struct StateRequestMsg {
    MsgHeader h;          // SaveState or LoadState
    std::uint64_t frame_number; // frames done so far, recorded in the envelope
    char path[1024];
};

struct StateDoneMsg {
    MsgHeader h;
    Msg request;          // SaveState or LoadState
    std::int32_t ok;      // 1 = written / loaded; 0 = refused or failed, see detail
    std::uint64_t bytes;  // the core's state size
    char detail[512];     // why not, naming both values (the load rule's words)
};

constexpr std::size_t kMaxMsgSize = sizeof(SaveRegionsMsg); // the largest message
static_assert(kMaxMsgSize >= sizeof(LogMsg) && kMaxMsgSize >= sizeof(StateRequestMsg) &&
                  kMaxMsgSize >= sizeof(StateDoneMsg) && kMaxMsgSize >= sizeof(EventMsg),
              "kMaxMsgSize must hold every message");

// ---- the wire layout, pinned ------------------------------------------------
//
// The hub and the runner may be built by different compilers (Retro Launcher's
// Windows hub is MSVC). Every byte both sides touch is laid out exactly as the
// Linux GCC build laid it out when protocol 1.0 shipped; a compiler that
// disagrees fails here instead of corrupting a session (LINK_TRANSPORTS.md §6).
// Measured 2026-09-26 on GCC 16 x86_64 and confirmed identical on MinGW-w64.
// A change to any line below is a protocol major bump.

static_assert(sizeof(FrameInfo) == 32 && alignof(FrameInfo) == 8, "FrameInfo");
static_assert(offsetof(FrameInfo, width) == 0, "FrameInfo::width");
static_assert(offsetof(FrameInfo, height) == 4, "FrameInfo::height");
static_assert(offsetof(FrameInfo, stride) == 8, "FrameInfo::stride");
static_assert(offsetof(FrameInfo, pixel_format) == 12, "FrameInfo::pixel_format");
static_assert(offsetof(FrameInfo, aspect_num) == 16, "FrameInfo::aspect_num");
static_assert(offsetof(FrameInfo, aspect_den) == 20, "FrameInfo::aspect_den");
static_assert(offsetof(FrameInfo, frame_number) == 24, "FrameInfo::frame_number");
static_assert(sizeof(SharedHeader) == 192 && alignof(SharedHeader) == 8, "SharedHeader");
static_assert(offsetof(SharedHeader, magic) == 0, "SharedHeader::magic");
static_assert(offsetof(SharedHeader, protocol_major) == 8, "SharedHeader::protocol_major");
static_assert(offsetof(SharedHeader, protocol_minor) == 12, "SharedHeader::protocol_minor");
static_assert(offsetof(SharedHeader, header_size) == 16, "SharedHeader::header_size");
static_assert(offsetof(SharedHeader, total_size) == 24, "SharedHeader::total_size");
static_assert(offsetof(SharedHeader, frames_offset) == 32, "SharedHeader::frames_offset");
static_assert(offsetof(SharedHeader, audio_offset) == 40, "SharedHeader::audio_offset");
static_assert(offsetof(SharedHeader, middle) == 48, "SharedHeader::middle");
static_assert(offsetof(SharedHeader, slot) == 56, "SharedHeader::slot");
static_assert(offsetof(SharedHeader, audio_write) == 152, "SharedHeader::audio_write");
static_assert(offsetof(SharedHeader, audio_read) == 160, "SharedHeader::audio_read");
static_assert(offsetof(SharedHeader, audio_rate) == 168, "SharedHeader::audio_rate");
static_assert(offsetof(SharedHeader, audio_capacity) == 172, "SharedHeader::audio_capacity");
static_assert(offsetof(SharedHeader, audio_dropped) == 176, "SharedHeader::audio_dropped");
static_assert(offsetof(SharedHeader, frame_rate) == 184, "SharedHeader::frame_rate");
static_assert(sizeof(MsgHeader) == 8 && alignof(MsgHeader) == 4, "MsgHeader");
static_assert(offsetof(MsgHeader, type) == 0, "MsgHeader::type");
static_assert(offsetof(MsgHeader, size) == 4, "MsgHeader::size");
static_assert(sizeof(HelloMsg) == 296 && alignof(HelloMsg) == 8, "HelloMsg");
static_assert(offsetof(HelloMsg, h) == 0, "HelloMsg::h");
static_assert(offsetof(HelloMsg, protocol_major) == 8, "HelloMsg::protocol_major");
static_assert(offsetof(HelloMsg, protocol_minor) == 12, "HelloMsg::protocol_minor");
static_assert(offsetof(HelloMsg, abi_major) == 16, "HelloMsg::abi_major");
static_assert(offsetof(HelloMsg, draft_revision) == 20, "HelloMsg::draft_revision");
static_assert(offsetof(HelloMsg, engine_dirty) == 24, "HelloMsg::engine_dirty");
static_assert(offsetof(HelloMsg, capabilities) == 32, "HelloMsg::capabilities");
static_assert(offsetof(HelloMsg, sha256) == 40, "HelloMsg::sha256");
static_assert(offsetof(HelloMsg, core_id) == 105, "HelloMsg::core_id");
static_assert(offsetof(HelloMsg, core_version) == 168, "HelloMsg::core_version");
static_assert(offsetof(HelloMsg, platforms) == 232, "HelloMsg::platforms");
static_assert(sizeof(RegionDesc) == 72 && alignof(RegionDesc) == 8, "RegionDesc");
static_assert(offsetof(RegionDesc, id) == 0, "RegionDesc::id");
static_assert(offsetof(RegionDesc, kind) == 48, "RegionDesc::kind");
static_assert(offsetof(RegionDesc, seat) == 52, "RegionDesc::seat");
static_assert(offsetof(RegionDesc, slot) == 56, "RegionDesc::slot");
static_assert(offsetof(RegionDesc, erase_value) == 60, "RegionDesc::erase_value");
static_assert(offsetof(RegionDesc, size) == 64, "RegionDesc::size");
static_assert(sizeof(SaveRegionsMsg) == 1168 && alignof(SaveRegionsMsg) == 8, "SaveRegionsMsg");
static_assert(offsetof(SaveRegionsMsg, h) == 0, "SaveRegionsMsg::h");
static_assert(offsetof(SaveRegionsMsg, count) == 8, "SaveRegionsMsg::count");
static_assert(offsetof(SaveRegionsMsg, region) == 16, "SaveRegionsMsg::region");
static_assert(sizeof(FrameDoneMsg) == 24 && alignof(FrameDoneMsg) == 8, "FrameDoneMsg");
static_assert(offsetof(FrameDoneMsg, h) == 0, "FrameDoneMsg::h");
static_assert(offsetof(FrameDoneMsg, frame_number) == 8, "FrameDoneMsg::frame_number");
static_assert(offsetof(FrameDoneMsg, result) == 16, "FrameDoneMsg::result");
static_assert(sizeof(LogMsg) == 1036 && alignof(LogMsg) == 4, "LogMsg");
static_assert(offsetof(LogMsg, h) == 0, "LogMsg::h");
static_assert(offsetof(LogMsg, level) == 8, "LogMsg::level");
static_assert(offsetof(LogMsg, text) == 12, "LogMsg::text");
static_assert(sizeof(EventMsg) == 544 && alignof(EventMsg) == 8, "EventMsg");
static_assert(offsetof(EventMsg, h) == 0, "EventMsg::h");
static_assert(offsetof(EventMsg, kind) == 8, "EventMsg::kind");
static_assert(offsetof(EventMsg, frame_number) == 16, "EventMsg::frame_number");
static_assert(offsetof(EventMsg, guest_address) == 24, "EventMsg::guest_address");
static_assert(offsetof(EventMsg, detail) == 32, "EventMsg::detail");
static_assert(sizeof(ExitingMsg) == 524 && alignof(ExitingMsg) == 4, "ExitingMsg");
static_assert(offsetof(ExitingMsg, h) == 0, "ExitingMsg::h");
static_assert(offsetof(ExitingMsg, code) == 8, "ExitingMsg::code");
static_assert(offsetof(ExitingMsg, reason) == 12, "ExitingMsg::reason");
static_assert(sizeof(SavesFilledMsg) == 232 && alignof(SavesFilledMsg) == 4, "SavesFilledMsg");
static_assert(offsetof(SavesFilledMsg, h) == 0, "SavesFilledMsg::h");
static_assert(offsetof(SavesFilledMsg, pads) == 8, "SavesFilledMsg::pads");
static_assert(sizeof(GrantMsg) == 240 && alignof(GrantMsg) == 8, "GrantMsg");
static_assert(offsetof(GrantMsg, h) == 0, "GrantMsg::h");
static_assert(offsetof(GrantMsg, frame_number) == 8, "GrantMsg::frame_number");
static_assert(offsetof(GrantMsg, pads) == 16, "GrantMsg::pads");
static_assert(sizeof(EmptyMsg) == 8 && alignof(EmptyMsg) == 4, "EmptyMsg");
static_assert(offsetof(EmptyMsg, h) == 0, "EmptyMsg::h");
static_assert(sizeof(rcore_pad) == 28 && alignof(rcore_pad) == 4, "rcore_pad");
// 1.1
static_assert(sizeof(StateRequestMsg) == 1040 && alignof(StateRequestMsg) == 8, "StateRequestMsg");
static_assert(offsetof(StateRequestMsg, h) == 0, "StateRequestMsg::h");
static_assert(offsetof(StateRequestMsg, frame_number) == 8, "StateRequestMsg::frame_number");
static_assert(offsetof(StateRequestMsg, path) == 16, "StateRequestMsg::path");
static_assert(sizeof(StateDoneMsg) == 536 && alignof(StateDoneMsg) == 8, "StateDoneMsg");
static_assert(offsetof(StateDoneMsg, h) == 0, "StateDoneMsg::h");
static_assert(offsetof(StateDoneMsg, request) == 8, "StateDoneMsg::request");
static_assert(offsetof(StateDoneMsg, ok) == 12, "StateDoneMsg::ok");
static_assert(offsetof(StateDoneMsg, bytes) == 16, "StateDoneMsg::bytes");
static_assert(offsetof(StateDoneMsg, detail) == 24, "StateDoneMsg::detail");


} // namespace retro::corelink
