#pragma once

// Everything OS-specific about the hub <-> runner link (docs/LINK_TRANSPORTS.md).
// The session code -- core_link.cpp on the hub's side, runner_link.cpp on the
// runner's -- sees only this interface, so the protocol is one implementation
// on every OS:
//
//                 control channel                 memory       handles ride
//   Linux    SOCK_SEQPACKET socketpair            memfd        SCM_RIGHTS
//   macOS    SOCK_STREAM socketpair, framed       shm_open     SCM_RIGHTS
//            by MsgHeader.size                    (unlinked)
//   Windows  message-mode named pipe, both ends   unnamed      a trailer after the
//            opened by the hub before the spawn   section      message; the hub
//                                                              DuplicateHandles
//
// Handles a channel returns are the receiver's own: it closes them.

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace retro::corelink {

namespace fs = std::filesystem;

#if defined(_WIN32)
using NativeHandle = std::uintptr_t; // a HANDLE
constexpr NativeHandle kNoHandle = 0;
#else
using NativeHandle = int; // a file descriptor
constexpr NativeHandle kNoHandle = -1;
#endif

void close_handle(NativeHandle h);

// A shared memory object and this process's mapping of it.
struct SharedMemory {
    NativeHandle handle = kNoHandle;
    void* data = nullptr;
    std::size_t size = 0;
};

// A new, zero-filled object of `size` bytes (a 0-byte region maps 1 byte),
// mapped read-write. `name` is a debugging label only; nothing else can open it.
bool create_shared(std::size_t size, const char* name, SharedMemory& out, std::string* error);
// Maps an object this process holds a handle to. Takes ownership of `h`.
bool map_shared(NativeHandle h, std::size_t size, SharedMemory& out, std::string* error);
// Unmaps and closes whatever of the two is still held.
void release_shared(SharedMemory& m);
// Closes the handle and keeps the mapping (the mapping holds the object).
void close_shared_handle(SharedMemory& m);

// One end of the control channel.
struct Channel {
    NativeHandle handle = kNoHandle;
#if defined(__APPLE__)
    std::vector<unsigned char> pending;   // bytes of a message not yet whole
    std::vector<int> pending_fds;         // fds that arrived with them
#endif
#if defined(_WIN32)
    NativeHandle peer_process = kNoHandle; // hub end: the runner, to duplicate through
    bool overlapped = false;               // the hub's end is; the runner's is not
    struct Read;                           // an overlapped read that may still be in flight
    Read* read = nullptr;
#endif
    Channel() = default;
    Channel(const Channel&) = delete;
    Channel& operator=(const Channel&) = delete;
    ~Channel();
    void close();
};

// True when the whole message (and every handle) went out.
bool send_packet(Channel& ch, const void* msg, std::size_t size,
                 const NativeHandle* handles = nullptr, std::size_t count = 0);

enum class RecvResult { Packet, WouldBlock, Closed, Error };

// One whole message into `buf`. Handles that came with it are appended to
// *handles. timeout_ms: -1 waits for one, 0 only takes one already here, >0
// waits up to that long. Closed means the peer is gone -- for the hub, that the
// runner died, however it died.
RecvResult recv_packet(Channel& ch, std::vector<unsigned char>& buf,
                       std::vector<NativeHandle>* handles, int timeout_ms);

// ---- the hub: start and end the runner ------------------------------------

struct SpawnSpec {
    std::vector<std::string> args; // UTF-8; args[0] is the runner binary
    std::vector<std::string> env;  // NAME=value, overriding the hub's own by name
    fs::path log;                  // the runner's stdout and stderr
};

struct RunnerProcess {
#if defined(_WIN32)
    NativeHandle process = kNoHandle;
    NativeHandle job = kNoHandle; // kill-on-close: the runner cannot outlive the hub
    bool ended = false;
    ~RunnerProcess();
#else
    int pid = -1;
#endif
    RunnerProcess() = default;
    RunnerProcess(const RunnerProcess&) = delete;
    RunnerProcess& operator=(const RunnerProcess&) = delete;
    bool running() const;
};

// Starts the runner holding the other end of `hub_end` and a handle to
// `region`. On Windows this appends --link-handles <control>,<shared>; on
// POSIX the runner finds them at kSocketFd and kSharedFd.
bool spawn_runner(const SpawnSpec& spec, const SharedMemory& region, Channel& hub_end,
                  RunnerProcess& out, std::string* error);

// The exit code once the runner has ended: its code, -signal on POSIX, the
// raw code (an NTSTATUS for a crash) on Windows. timeout_ms as recv_packet.
std::optional<int> wait_exit(RunnerProcess& p, int timeout_ms);
void kill_runner(RunnerProcess& p);

// Runs a program with no link, stdout and stderr to spec.log, and waits up to
// timeout_ms for it (killing it after that). The exit code, or nullopt when
// it could not start or was killed. Hosts use it to ask a runner --version.
std::optional<int> run_to_completion(const SpawnSpec& spec, int timeout_ms, std::string* error);

// ---- the runner: find what the hub handed over ----------------------------

// `link_handles` is the --link-handles value (Windows; empty elsewhere).
bool runner_endpoints(const std::string& link_handles, Channel& control,
                      NativeHandle& shared, std::string* error);

// The command line as UTF-8: argv itself on POSIX; on Windows, whose argv is
// the ANSI code page, the UTF-16 command line converted.
std::vector<std::string> utf8_args(int argc, char** argv);

// A path as UTF-8, and a path from UTF-8, on every OS (Windows' narrow
// strings are the ANSI code page otherwise).
std::string path_utf8(const fs::path& p);
fs::path utf8_path(const std::string& s);

} // namespace retro::corelink
