// The link's transport on Linux and macOS (docs/LINK_TRANSPORTS.md §2-3).
//
// Linux: a SOCK_SEQPACKET socketpair (the kernel keeps message boundaries),
// memfd for memory, fork + execve.
// macOS: no SEQPACKET for AF_UNIX and no memfd, so a SOCK_STREAM socketpair
// framed by MsgHeader.size, shm_open under a random name unlinked at once, and
// posix_spawn with POSIX_SPAWN_CLOEXEC_DEFAULT. A datagram socket would keep
// boundaries but gives no EOF when the runner dies, and that EOF is how the hub
// learns of a crash.

#include "transport.hpp"

#include "link_protocol.hpp"

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/uio.h>
#include <sys/wait.h>
#include <unistd.h>

#if defined(__APPLE__)
#  include <spawn.h>
#  include <sys/event.h>
#else
#  include <sys/syscall.h>
#endif

extern char** environ;

namespace retro::corelink {

namespace {

std::string errno_text(const char* what) {
    return std::string(what) + ": " + std::strerror(errno);
}

#if defined(__APPLE__)
void set_cloexec(int fd) {
    const int flags = ::fcntl(fd, F_GETFD);
    if (flags >= 0) ::fcntl(fd, F_SETFD, flags | FD_CLOEXEC);
}
#endif

// Every fd that arrived in a message's control data.
void take_fds(msghdr& mh, std::vector<int>& out) {
    for (cmsghdr* c = CMSG_FIRSTHDR(&mh); c; c = CMSG_NXTHDR(&mh, c)) {
        if (c->cmsg_level != SOL_SOCKET || c->cmsg_type != SCM_RIGHTS) continue;
        const std::size_t count = (c->cmsg_len - CMSG_LEN(0)) / sizeof(int);
        const int* p = reinterpret_cast<const int*>(CMSG_DATA(c));
        for (std::size_t i = 0; i < count; ++i) {
#if defined(__APPLE__)
            set_cloexec(p[i]); // no MSG_CMSG_CLOEXEC on macOS
#endif
            out.push_back(p[i]);
        }
    }
}

// Waits until `fd` is readable: false on timeout.
bool wait_readable(int fd, int timeout_ms) {
    pollfd p{fd, POLLIN, 0};
    for (;;) {
        const int n = ::poll(&p, 1, timeout_ms);
        if (n < 0 && errno == EINTR) continue;
        return n != 0;
    }
}

#if defined(__APPLE__)
constexpr int kSendFlags = 0; // SO_NOSIGPIPE is set on the socket instead
#else
constexpr int kSendFlags = MSG_NOSIGNAL;
#endif

} // namespace

void close_handle(NativeHandle h) {
    if (h >= 0) ::close(h);
}

// ---- memory -------------------------------------------------------------------

bool create_shared(std::size_t size, const char* name, SharedMemory& out, std::string* error) {
    const std::size_t len = size ? size : 1;
#if defined(__APPLE__)
    // A random name that exists only between shm_open and shm_unlink. macOS
    // caps these names at 31 bytes (PSHMNAMLEN).
    (void)name;
    int fd = -1;
    for (int attempt = 0; attempt < 16 && fd < 0; ++attempt) {
        char shm_name[32];
        std::snprintf(shm_name, sizeof shm_name, "/rcl.%d.%08x", static_cast<int>(::getpid()),
                      static_cast<unsigned>(arc4random()));
        fd = ::shm_open(shm_name, O_RDWR | O_CREAT | O_EXCL, 0600);
        if (fd >= 0) ::shm_unlink(shm_name);
        else if (errno != EEXIST) break;
    }
    if (fd < 0) {
        if (error) *error = errno_text("shm_open");
        return false;
    }
    set_cloexec(fd);
#else
    const int fd = ::memfd_create(name, MFD_CLOEXEC);
    if (fd < 0) {
        if (error) *error = errno_text("memfd_create");
        return false;
    }
#endif
    if (::ftruncate(fd, static_cast<off_t>(len)) != 0) {
        if (error) *error = errno_text("ftruncate");
        ::close(fd);
        return false;
    }
    return map_shared(fd, size, out, error);
}

bool map_shared(NativeHandle h, std::size_t size, SharedMemory& out, std::string* error) {
    const std::size_t len = size ? size : 1;
    struct stat st{};
    if (::fstat(h, &st) != 0 || static_cast<std::size_t>(st.st_size) < len) {
        if (error) *error = "the shared object is missing or smaller than " + std::to_string(len);
        ::close(h);
        return false;
    }
    void* p = ::mmap(nullptr, len, PROT_READ | PROT_WRITE, MAP_SHARED, h, 0);
    if (p == MAP_FAILED) {
        if (error) *error = errno_text("mmap");
        ::close(h);
        return false;
    }
    out.handle = h;
    out.data = p;
    out.size = size;
    return true;
}

void release_shared(SharedMemory& m) {
    if (m.data) ::munmap(m.data, m.size ? m.size : 1);
    m.data = nullptr;
    close_shared_handle(m);
}

void close_shared_handle(SharedMemory& m) {
    close_handle(m.handle);
    m.handle = kNoHandle;
}

// ---- the channel ----------------------------------------------------------------

Channel::~Channel() { close(); }

void Channel::close() {
    close_handle(handle);
    handle = kNoHandle;
#if defined(__APPLE__)
    for (int fd : pending_fds) ::close(fd);
    pending_fds.clear();
    pending.clear();
#endif
}

bool send_packet(Channel& ch, const void* msg, std::size_t size, const NativeHandle* handles,
                 std::size_t count) {
    const auto* bytes = static_cast<const unsigned char*>(msg);
    std::size_t sent = 0;
    std::vector<char> ctrl;
    while (sent < size) {
        iovec iov{const_cast<unsigned char*>(bytes + sent), size - sent};
        msghdr mh{};
        mh.msg_iov = &iov;
        mh.msg_iovlen = 1;
        if (count && sent == 0) { // the handles ride with the message's first byte
            ctrl.assign(CMSG_SPACE(sizeof(int) * count), 0);
            mh.msg_control = ctrl.data();
            mh.msg_controllen = static_cast<socklen_t>(ctrl.size());
            cmsghdr* c = CMSG_FIRSTHDR(&mh);
            c->cmsg_level = SOL_SOCKET;
            c->cmsg_type = SCM_RIGHTS;
            c->cmsg_len = CMSG_LEN(sizeof(int) * count);
            std::memcpy(CMSG_DATA(c), handles, sizeof(int) * count);
        }
        const ssize_t n = ::sendmsg(ch.handle, &mh, kSendFlags);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) return false;
#if !defined(__APPLE__)
        // SEQPACKET sends a message whole or not at all.
        return static_cast<std::size_t>(n) == size;
#endif
        sent += static_cast<std::size_t>(n);
    }
    return true;
}

#if defined(__APPLE__)

namespace {

// The whole message at the front of ch.pending, if there is one.
RecvResult take_pending(Channel& ch, std::vector<unsigned char>& buf,
                        std::vector<NativeHandle>* handles, bool& have) {
    have = false;
    if (ch.pending.size() < sizeof(MsgHeader)) return RecvResult::WouldBlock;
    MsgHeader h;
    std::memcpy(&h, ch.pending.data(), sizeof h);
    // A size the protocol cannot have means the stream is lost: there is no
    // boundary left to resynchronise on, as with a torn SEQPACKET.
    if (h.size < sizeof(MsgHeader) || h.size > kMaxMsgSize) return RecvResult::Error;
    if (ch.pending.size() < h.size) return RecvResult::WouldBlock;
    buf.assign(ch.pending.begin(), ch.pending.begin() + h.size);
    ch.pending.erase(ch.pending.begin(), ch.pending.begin() + h.size);
    // Fds belong to the message whose first byte they arrived with; a message
    // carries at most one set, and only SaveRegions carries any.
    if (handles) handles->insert(handles->end(), ch.pending_fds.begin(), ch.pending_fds.end());
    else for (int fd : ch.pending_fds) ::close(fd);
    ch.pending_fds.clear();
    have = true;
    return RecvResult::Packet;
}

} // namespace

RecvResult recv_packet(Channel& ch, std::vector<unsigned char>& buf,
                       std::vector<NativeHandle>* handles, int timeout_ms) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
    for (;;) {
        bool have = false;
        const RecvResult pr = take_pending(ch, buf, handles, have);
        if (have || pr == RecvResult::Error) return pr;

        int wait = timeout_ms;
        if (timeout_ms > 0) {
            const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(
                deadline - std::chrono::steady_clock::now()).count();
            wait = left > 0 ? static_cast<int>(left) : 0;
        }
        if (!wait_readable(ch.handle, wait)) return RecvResult::WouldBlock;

        unsigned char chunk[4096];
        iovec iov{chunk, sizeof chunk};
        msghdr mh{};
        mh.msg_iov = &iov;
        mh.msg_iovlen = 1;
        alignas(cmsghdr) char ctrl[CMSG_SPACE(sizeof(int) * kMaxRegions)];
        mh.msg_control = ctrl;
        mh.msg_controllen = sizeof ctrl;
        const ssize_t n = ::recvmsg(ch.handle, &mh, MSG_DONTWAIT);
        if (n < 0) {
            if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK) continue;
            return RecvResult::Error;
        }
        if (n == 0) return RecvResult::Closed;
        take_fds(mh, ch.pending_fds);
        if (mh.msg_flags & MSG_CTRUNC) return RecvResult::Error;
        ch.pending.insert(ch.pending.end(), chunk, chunk + n);
    }
}

#else // Linux

RecvResult recv_packet(Channel& ch, std::vector<unsigned char>& buf,
                       std::vector<NativeHandle>* handles, int timeout_ms) {
    if (timeout_ms > 0) wait_readable(ch.handle, timeout_ms);
    buf.resize(kMaxMsgSize);
    iovec iov{buf.data(), buf.size()};
    msghdr mh{};
    mh.msg_iov = &iov;
    mh.msg_iovlen = 1;
    alignas(cmsghdr) char ctrl[CMSG_SPACE(sizeof(int) * kMaxRegions)];
    mh.msg_control = ctrl;
    mh.msg_controllen = sizeof ctrl;
    for (;;) {
        const ssize_t n = ::recvmsg(ch.handle, &mh,
                                    MSG_CMSG_CLOEXEC | (timeout_ms < 0 ? 0 : MSG_DONTWAIT));
        if (n < 0) {
            if (errno == EINTR) continue;
            if (errno == EAGAIN || errno == EWOULDBLOCK) return RecvResult::WouldBlock;
            return RecvResult::Error;
        }
        if (n == 0) return RecvResult::Closed;
        buf.resize(static_cast<std::size_t>(n));
        std::vector<int> fds;
        take_fds(mh, fds);
        if (handles) handles->insert(handles->end(), fds.begin(), fds.end());
        else for (int fd : fds) ::close(fd);
        if (mh.msg_flags & (MSG_TRUNC | MSG_CTRUNC)) return RecvResult::Error;
        return RecvResult::Packet;
    }
}

#endif

// ---- the hub: the runner process -------------------------------------------------

bool RunnerProcess::running() const { return pid > 0; }

bool spawn_runner(const SpawnSpec& spec, const SharedMemory& region, Channel& hub_end,
                  RunnerProcess& out, std::string* error) {
    auto fail = [&](const std::string& m) {
        if (error) *error = m;
        return false;
    };
    int sv[2];
#if defined(__APPLE__)
    if (::socketpair(AF_UNIX, SOCK_STREAM, 0, sv) != 0) return fail(errno_text("socketpair"));
    for (int fd : sv) {
        set_cloexec(fd);
        const int one = 1;
        ::setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof one);
        // The default AF_UNIX buffer (8 KiB) is smaller than a burst of Log messages.
        const int bytes = 256 * 1024;
        ::setsockopt(fd, SOL_SOCKET, SO_SNDBUF, &bytes, sizeof bytes);
        ::setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &bytes, sizeof bytes);
    }
#else
    if (::socketpair(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0, sv) != 0) {
        return fail(errno_text("socketpair"));
    }
#endif

    std::vector<std::string> args = spec.args;
    std::vector<char*> argv;
    for (auto& s : args) argv.push_back(s.data());
    argv.push_back(nullptr);
    // The whole environment, built before the spawn: nothing between fork and
    // exec may allocate (the hub has other threads). spec.env overrides by name.
    std::vector<std::string> env_strings;
    for (char** e = environ; e && *e; ++e) {
        const std::string kv = *e;
        const std::string name = kv.substr(0, kv.find('='));
        const bool overridden = std::any_of(spec.env.begin(), spec.env.end(),
            [&](const std::string& o) { return o.compare(0, name.size() + 1, name + "=") == 0; });
        if (!overridden) env_strings.push_back(kv);
    }
    env_strings.insert(env_strings.end(), spec.env.begin(), spec.env.end());
    std::vector<char*> envp;
    for (auto& s : env_strings) envp.push_back(s.data());
    envp.push_back(nullptr);
    const std::string log_path = spec.log.string();

#if defined(__APPLE__)
    // Only the fds named here are inherited (POSIX_SPAWN_CLOEXEC_DEFAULT). They
    // are first moved clear of 3 and 4, so neither dup2 clobbers the other.
    const int s_tmp = ::fcntl(sv[1], F_DUPFD_CLOEXEC, 10);
    const int m_tmp = ::fcntl(region.handle, F_DUPFD_CLOEXEC, 10);
    posix_spawn_file_actions_t fa;
    posix_spawnattr_t attr;
    posix_spawn_file_actions_init(&fa);
    posix_spawnattr_init(&attr);
    posix_spawnattr_setflags(&attr, POSIX_SPAWN_CLOEXEC_DEFAULT);
    posix_spawn_file_actions_addopen(&fa, 1, log_path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
    posix_spawn_file_actions_adddup2(&fa, 1, 2);
    posix_spawn_file_actions_adddup2(&fa, s_tmp, kSocketFd);
    posix_spawn_file_actions_adddup2(&fa, m_tmp, kSharedFd);
    pid_t pid = -1;
    const int rc = ::posix_spawn(&pid, argv[0], &fa, &attr, argv.data(), envp.data());
    posix_spawn_file_actions_destroy(&fa);
    posix_spawnattr_destroy(&attr);
    ::close(s_tmp);
    ::close(m_tmp);
    if (rc != 0) {
        ::close(sv[0]);
        ::close(sv[1]);
        return fail(std::string("posix_spawn ") + argv[0] + ": " + std::strerror(rc));
    }
#else
    const pid_t pid = ::fork();
    if (pid < 0) {
        ::close(sv[0]);
        ::close(sv[1]);
        return fail(errno_text("fork"));
    }
    if (pid == 0) {
        // Child: only async-signal-safe calls from here to exec.
        const int log_fd = ::open(log_path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (log_fd >= 0) {
            ::dup2(log_fd, 1);
            ::dup2(log_fd, 2);
        }
        // Move both fds clear of 3 and 4 before placing them, so neither
        // dup2 clobbers the other; dup2 also clears CLOEXEC on the result.
        const int s_tmp = ::fcntl(sv[1], F_DUPFD, 10);
        const int m_tmp = ::fcntl(region.handle, F_DUPFD, 10);
        ::dup2(s_tmp, kSocketFd);
        ::dup2(m_tmp, kSharedFd);
        ::execve(argv[0], argv.data(), envp.data());
        const char msg[] = "retro-core-runner: exec failed\n";
        (void)!::write(2, msg, sizeof msg - 1);
        ::_exit(127);
    }
#endif
    ::close(sv[1]);
    hub_end.handle = sv[0];
    out.pid = pid;
    return true;
}

namespace {

std::optional<int> reap(RunnerProcess& p, bool block) {
    int status = 0;
    for (;;) {
        const pid_t got = ::waitpid(p.pid, &status, block ? 0 : WNOHANG);
        if (got < 0 && errno == EINTR) continue;
        if (got != p.pid) return std::nullopt;
        break;
    }
    p.pid = -1;
    return WIFEXITED(status) ? WEXITSTATUS(status) : WIFSIGNALED(status) ? -WTERMSIG(status) : -1;
}

// Waits up to timeout_ms for the process to exit, without polling on a sleep.
void wait_for_exit_event(int pid, int timeout_ms) {
#if defined(__APPLE__)
    const int kq = ::kqueue();
    if (kq < 0) return;
    struct kevent ev;
    EV_SET(&ev, pid, EVFILT_PROC, EV_ADD | EV_ONESHOT, NOTE_EXIT, 0, nullptr);
    timespec ts{timeout_ms / 1000, (timeout_ms % 1000) * 1000000L};
    struct kevent got;
    // A process already gone fails EV_ADD with ESRCH, and waitpid then reaps it.
    ::kevent(kq, &ev, 1, &got, 1, &ts);
    ::close(kq);
#else
    const int pidfd = static_cast<int>(::syscall(SYS_pidfd_open, pid, 0));
    if (pidfd < 0) return; // a kernel before 5.3: the caller reaps without waiting
    wait_readable(pidfd, timeout_ms);
    ::close(pidfd);
#endif
}

} // namespace

std::optional<int> wait_exit(RunnerProcess& p, int timeout_ms) {
    if (p.pid <= 0) return std::nullopt;
    if (timeout_ms < 0) return reap(p, true);
    if (auto code = reap(p, false)) return code;
    if (timeout_ms == 0) return std::nullopt;
    wait_for_exit_event(p.pid, timeout_ms);
    return reap(p, false);
}

void kill_runner(RunnerProcess& p) {
    if (p.pid > 0) ::kill(p.pid, SIGKILL);
}

// ---- the runner -------------------------------------------------------------------

bool runner_endpoints(const std::string& link_handles, Channel& control, NativeHandle& shared,
                      std::string* error) {
    if (!link_handles.empty()) {
        if (error) *error = "--link-handles is for Windows; here the link is at fds 3 and 4";
        return false;
    }
    control.handle = kSocketFd;
    shared = kSharedFd;
    return true;
}

std::vector<std::string> utf8_args(int argc, char** argv) {
    return std::vector<std::string>(argv, argv + argc);
}

std::string path_utf8(const fs::path& p) { return p.string(); }
fs::path utf8_path(const std::string& s) { return fs::path(s); }

} // namespace retro::corelink
