// The link's transport on Windows (docs/LINK_TRANSPORTS.md §2, §4).
//
// Control: a named pipe in MESSAGE mode, which keeps packet boundaries the way
// SOCK_SEQPACKET does. The hub creates the server end (first instance only,
// one instance, local clients only, a DACL naming only this user) and opens
// the client end itself before the spawn, so nothing else can squat the name
// or connect to it. The runner inherits the client end; when the runner dies
// its handle closes and the hub's read fails with ERROR_BROKEN_PIPE: the EOF.
//
// Memory: unnamed sections. The hub's region is inherited through an explicit
// handle list; save regions are created by the runner and duplicated out of it
// by the hub, which holds the runner's process handle. The hub always does the
// duplicating, so the runner is never given a handle to the hub.
//
// Handles ride in a trailer after the message -- { u32 count; u32 pad;
// u64 handle[count] } -- told apart from it by MsgHeader.size, and returned in
// the same vector SCM_RIGHTS fills on POSIX. Messages themselves are
// byte-identical on every OS.

#include "transport.hpp"

#include "link_protocol.hpp"

#ifndef WIN32_LEAN_AND_MEAN
#  define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#  define NOMINMAX
#endif
#include <windows.h>
#include <bcrypt.h>
#include <sddl.h>
#include <shellapi.h>

#include <algorithm>
#include <cstring>
#include <cwctype>

namespace retro::corelink {

namespace {

HANDLE H(NativeHandle h) { return reinterpret_cast<HANDLE>(h); }
NativeHandle N(HANDLE h) { return reinterpret_cast<NativeHandle>(h); }

std::string win_error(const std::string& what, DWORD code = GetLastError()) {
    return what + " failed (Windows error " + std::to_string(code) + ")";
}

std::wstring widen(const std::string& s) {
    if (s.empty()) return {};
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), int(s.size()), nullptr, 0);
    std::wstring w(static_cast<std::size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), int(s.size()), w.data(), n);
    return w;
}

// One argument quoted so the MSVCRT (and CommandLineToArgvW) parse it back whole.
void append_quoted(std::wstring& cmd, const std::wstring& arg) {
    if (!cmd.empty()) cmd += L' ';
    if (!arg.empty() && arg.find_first_of(L" \t\n\v\"") == std::wstring::npos) {
        cmd += arg;
        return;
    }
    cmd += L'"';
    for (auto it = arg.begin();; ++it) {
        std::size_t backslashes = 0;
        while (it != arg.end() && *it == L'\\') {
            ++it;
            ++backslashes;
        }
        if (it == arg.end()) {
            cmd.append(backslashes * 2, L'\\');
            break;
        }
        if (*it == L'"') {
            cmd.append(backslashes * 2 + 1, L'\\');
        } else {
            cmd.append(backslashes, L'\\');
        }
        cmd += *it;
    }
    cmd += L'"';
}

std::wstring env_name(const std::wstring& kv) {
    // "=C:=C:\\dir" entries have a name that starts with '='.
    const std::size_t eq = kv.find(L'=', 1);
    std::wstring name = kv.substr(0, eq);
    for (auto& c : name) c = static_cast<wchar_t>(std::towupper(c));
    return name;
}

// The hub's environment with spec.env overriding by name, as a
// CREATE_UNICODE_ENVIRONMENT block.
std::vector<wchar_t> env_block(const std::vector<std::string>& overrides) {
    std::vector<std::wstring> over;
    for (const auto& o : overrides) over.push_back(widen(o));
    std::vector<std::wstring> entries;
    if (wchar_t* env = GetEnvironmentStringsW()) {
        for (const wchar_t* p = env; *p; p += std::wcslen(p) + 1) {
            const std::wstring kv = p;
            const std::wstring name = env_name(kv);
            const bool overridden = std::any_of(over.begin(), over.end(),
                [&](const std::wstring& o) { return env_name(o) == name; });
            if (!overridden) entries.push_back(kv);
        }
        FreeEnvironmentStringsW(env);
    }
    entries.insert(entries.end(), over.begin(), over.end());
    std::vector<wchar_t> block;
    for (const auto& e : entries) {
        block.insert(block.end(), e.begin(), e.end());
        block.push_back(L'\0');
    }
    block.push_back(L'\0');
    if (entries.empty()) block.push_back(L'\0');
    return block;
}

// A DACL granting this user, and no one else, full access.
struct UserOnlySecurity {
    PSECURITY_DESCRIPTOR sd = nullptr;
    SECURITY_ATTRIBUTES sa{sizeof(SECURITY_ATTRIBUTES), nullptr, FALSE};
    ~UserOnlySecurity() {
        if (sd) LocalFree(sd);
    }
    bool build(std::string* error) {
        HANDLE token = nullptr;
        if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) {
            if (error) *error = win_error("OpenProcessToken");
            return false;
        }
        DWORD len = 0;
        GetTokenInformation(token, TokenUser, nullptr, 0, &len);
        std::vector<unsigned char> buf(len);
        const bool ok = GetTokenInformation(token, TokenUser, buf.data(), len, &len);
        CloseHandle(token);
        if (!ok) {
            if (error) *error = win_error("GetTokenInformation");
            return false;
        }
        LPWSTR sid = nullptr;
        if (!ConvertSidToStringSidW(reinterpret_cast<TOKEN_USER*>(buf.data())->User.Sid, &sid)) {
            if (error) *error = win_error("ConvertSidToStringSid");
            return false;
        }
        const std::wstring sddl = L"D:P(A;;GA;;;" + std::wstring(sid) + L")";
        LocalFree(sid);
        if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(sddl.c_str(), SDDL_REVISION_1,
                                                                  &sd, nullptr)) {
            if (error) *error = win_error("ConvertStringSecurityDescriptorToSecurityDescriptor");
            return false;
        }
        sa.lpSecurityDescriptor = sd;
        return true;
    }
};

struct Trailer {
    std::uint32_t count;
    std::uint32_t pad;
};
constexpr std::size_t kMaxPacket = kMaxMsgSize + sizeof(Trailer) + 8 * kMaxRegions;
constexpr DWORD kPipeBuffer = 256 * 1024;

// Splits a received packet into its message and its handles, duplicating
// each handle into this process when `peer` (the hub's view of the runner)
// is given.
RecvResult unpack(const unsigned char* data, std::size_t n, NativeHandle peer,
                  std::vector<unsigned char>& buf, std::vector<NativeHandle>* handles) {
    if (n < sizeof(MsgHeader)) return RecvResult::Error;
    MsgHeader h;
    std::memcpy(&h, data, sizeof h);
    if (h.size < sizeof(MsgHeader) || h.size > n) return RecvResult::Error;
    std::vector<NativeHandle> got;
    if (n > h.size) {
        Trailer t;
        if (n - h.size < sizeof t) return RecvResult::Error;
        std::memcpy(&t, data + h.size, sizeof t);
        if (t.count > kMaxRegions || n != h.size + sizeof t + 8 * std::size_t(t.count)) {
            return RecvResult::Error;
        }
        for (std::uint32_t i = 0; i < t.count; ++i) {
            std::uint64_t v;
            std::memcpy(&v, data + h.size + sizeof t + 8 * i, 8);
            NativeHandle mine = static_cast<NativeHandle>(v);
            if (peer != kNoHandle) {
                HANDLE dup = nullptr;
                if (!DuplicateHandle(H(peer), H(mine), GetCurrentProcess(), &dup, 0, FALSE,
                                     DUPLICATE_SAME_ACCESS)) {
                    for (NativeHandle g : got) close_handle(g);
                    return RecvResult::Error;
                }
                mine = N(dup);
            }
            got.push_back(mine);
        }
    }
    buf.assign(data, data + h.size);
    if (handles) handles->insert(handles->end(), got.begin(), got.end());
    else for (NativeHandle g : got) close_handle(g);
    return RecvResult::Packet;
}

} // namespace

void close_handle(NativeHandle h) {
    if (h != kNoHandle) CloseHandle(H(h));
}

// ---- memory -------------------------------------------------------------------

bool create_shared(std::size_t size, const char* name, SharedMemory& out, std::string* error) {
    (void)name;
    const std::uint64_t len = size ? size : 1;
    HANDLE h = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE,
                                  static_cast<DWORD>(len >> 32), static_cast<DWORD>(len), nullptr);
    if (!h) {
        if (error) *error = win_error("CreateFileMapping");
        return false;
    }
    return map_shared(N(h), size, out, error);
}

bool map_shared(NativeHandle h, std::size_t size, SharedMemory& out, std::string* error) {
    // A view larger than the section fails, so this is also the size check.
    void* p = MapViewOfFile(H(h), FILE_MAP_ALL_ACCESS, 0, 0, size ? size : 1);
    if (!p) {
        if (error) *error = win_error("MapViewOfFile");
        close_handle(h);
        return false;
    }
    out.handle = h;
    out.data = p;
    out.size = size;
    return true;
}

void release_shared(SharedMemory& m) {
    if (m.data) UnmapViewOfFile(m.data);
    m.data = nullptr;
    close_shared_handle(m);
}

void close_shared_handle(SharedMemory& m) {
    close_handle(m.handle);
    m.handle = kNoHandle;
}

// ---- the channel ----------------------------------------------------------------

struct Channel::Read {
    OVERLAPPED ov{};
    std::vector<unsigned char> buf = std::vector<unsigned char>(kMaxPacket);
    bool pending = false;
};

Channel::~Channel() { close(); }

void Channel::close() {
    if (read) {
        if (read->pending) {
            CancelIoEx(H(handle), &read->ov);
            DWORD n = 0;
            GetOverlappedResult(H(handle), &read->ov, &n, TRUE);
        }
        CloseHandle(read->ov.hEvent);
        delete read;
        read = nullptr;
    }
    close_handle(handle);
    handle = kNoHandle;
    peer_process = kNoHandle; // not ours: RunnerProcess owns it
}

bool send_packet(Channel& ch, const void* msg, std::size_t size, const NativeHandle* handles,
                 std::size_t count) {
    std::vector<unsigned char> pkt(static_cast<const unsigned char*>(msg),
                                   static_cast<const unsigned char*>(msg) + size);
    if (count) {
        if (count > kMaxRegions) return false;
        Trailer t{static_cast<std::uint32_t>(count), 0};
        pkt.insert(pkt.end(), reinterpret_cast<unsigned char*>(&t),
                   reinterpret_cast<unsigned char*>(&t) + sizeof t);
        for (std::size_t i = 0; i < count; ++i) {
            // The hub duplicates into the runner and sends the runner's values;
            // the runner sends its own, and the hub duplicates them out.
            NativeHandle v = handles[i];
            if (ch.peer_process != kNoHandle) {
                HANDLE dup = nullptr;
                if (!DuplicateHandle(GetCurrentProcess(), H(v), H(ch.peer_process), &dup, 0, FALSE,
                                     DUPLICATE_SAME_ACCESS)) {
                    return false;
                }
                v = N(dup);
            }
            const std::uint64_t v64 = v;
            pkt.insert(pkt.end(), reinterpret_cast<const unsigned char*>(&v64),
                       reinterpret_cast<const unsigned char*>(&v64) + 8);
        }
    }
    DWORD written = 0;
    if (!ch.overlapped) {
        return WriteFile(H(ch.handle), pkt.data(), DWORD(pkt.size()), &written, nullptr) &&
               written == pkt.size();
    }
    OVERLAPPED ov{};
    ov.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    BOOL ok = WriteFile(H(ch.handle), pkt.data(), DWORD(pkt.size()), nullptr, &ov);
    if (!ok && GetLastError() == ERROR_IO_PENDING) ok = TRUE;
    if (ok) ok = GetOverlappedResult(H(ch.handle), &ov, &written, TRUE);
    CloseHandle(ov.hEvent);
    return ok && written == pkt.size();
}

RecvResult recv_packet(Channel& ch, std::vector<unsigned char>& buf,
                       std::vector<NativeHandle>* handles, int timeout_ms) {
    auto failed = [](DWORD e) {
        return (e == ERROR_BROKEN_PIPE || e == ERROR_PIPE_NOT_CONNECTED || e == ERROR_NO_DATA)
                   ? RecvResult::Closed
                   : RecvResult::Error; // ERROR_MORE_DATA included: a message too big
    };
    if (!ch.overlapped) {
        // The runner's end: it blocks for the next message, or asks whether
        // one is already here. It never waits with a timeout.
        if (timeout_ms > 0) return RecvResult::Error;
        if (timeout_ms == 0) {
            DWORD avail = 0;
            if (!PeekNamedPipe(H(ch.handle), nullptr, 0, nullptr, &avail, nullptr)) {
                return failed(GetLastError());
            }
            if (!avail) return RecvResult::WouldBlock;
        }
        std::vector<unsigned char> raw(kMaxPacket);
        DWORD n = 0;
        if (!ReadFile(H(ch.handle), raw.data(), DWORD(raw.size()), &n, nullptr)) {
            return failed(GetLastError());
        }
        return unpack(raw.data(), n, kNoHandle, buf, handles);
    }

    // The hub's end: one overlapped read stays in flight across calls, so a
    // poll (timeout 0) never loses a message that arrives later.
    if (!ch.read) {
        ch.read = new Channel::Read;
        ch.read->ov.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    }
    Channel::Read& r = *ch.read;
    if (!r.pending) {
        ResetEvent(r.ov.hEvent);
        if (!ReadFile(H(ch.handle), r.buf.data(), DWORD(r.buf.size()), nullptr, &r.ov)) {
            const DWORD e = GetLastError();
            if (e != ERROR_IO_PENDING) return failed(e);
        }
        r.pending = true;
    }
    const DWORD wait = WaitForSingleObject(r.ov.hEvent,
                                           timeout_ms < 0 ? INFINITE : DWORD(timeout_ms));
    if (wait == WAIT_TIMEOUT) return RecvResult::WouldBlock;
    DWORD n = 0;
    const BOOL ok = GetOverlappedResult(H(ch.handle), &r.ov, &n, FALSE);
    r.pending = false;
    if (!ok) return failed(GetLastError());
    return unpack(r.buf.data(), n, ch.peer_process, buf, handles);
}

// ---- the hub: the runner process -------------------------------------------------

bool RunnerProcess::running() const { return process != kNoHandle && !ended; }

RunnerProcess::~RunnerProcess() {
    close_handle(job); // kill-on-close: a runner still alive ends here
    close_handle(process);
}

bool spawn_runner(const SpawnSpec& spec, const SharedMemory& region, Channel& hub_end,
                  RunnerProcess& out, std::string* error) {
    auto fail = [&](const std::string& m) {
        if (error) *error = m;
        return false;
    };
    UserOnlySecurity sec;
    if (!sec.build(error)) return false;

    // ---- the pipe: both ends, before the runner exists ------------------------
    HANDLE server = INVALID_HANDLE_VALUE;
    std::wstring name;
    for (int attempt = 0; attempt < 8 && server == INVALID_HANDLE_VALUE; ++attempt) {
        unsigned char rnd[16];
        if (BCryptGenRandom(nullptr, rnd, sizeof rnd, BCRYPT_USE_SYSTEM_PREFERRED_RNG) != 0) {
            return fail("BCryptGenRandom failed");
        }
        wchar_t hex[33];
        for (int i = 0; i < 16; ++i) swprintf(hex + 2 * i, 3, L"%02x", rnd[i]);
        name = L"\\\\.\\pipe\\retro-core-link-" + std::to_wstring(GetCurrentProcessId()) + L"-" +
               hex;
        server = CreateNamedPipeW(
            name.c_str(), PIPE_ACCESS_DUPLEX | FILE_FLAG_FIRST_PIPE_INSTANCE | FILE_FLAG_OVERLAPPED,
            PIPE_TYPE_MESSAGE | PIPE_READMODE_MESSAGE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS, 1,
            kPipeBuffer, kPipeBuffer, 0, &sec.sa);
        // A name that already exists fails FIRST_PIPE_INSTANCE: pick another.
        if (server == INVALID_HANDLE_VALUE && GetLastError() != ERROR_ACCESS_DENIED &&
            GetLastError() != ERROR_PIPE_BUSY) {
            return fail(win_error("CreateNamedPipe"));
        }
    }
    if (server == INVALID_HANDLE_VALUE) return fail(win_error("CreateNamedPipe"));

    SECURITY_ATTRIBUTES inherit{sizeof(SECURITY_ATTRIBUTES), nullptr, TRUE};
    HANDLE client = CreateFileW(name.c_str(), GENERIC_READ | GENERIC_WRITE, 0, &inherit,
                                OPEN_EXISTING, SECURITY_SQOS_PRESENT | SECURITY_IDENTIFICATION,
                                nullptr);
    if (client == INVALID_HANDLE_VALUE) {
        CloseHandle(server);
        return fail(win_error("open the pipe's client end"));
    }
    DWORD mode = PIPE_READMODE_MESSAGE;
    SetNamedPipeHandleState(client, &mode, nullptr, nullptr);
    {
        OVERLAPPED ov{};
        ov.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        const BOOL ok = ConnectNamedPipe(server, &ov);
        const DWORD e = ok ? ERROR_PIPE_CONNECTED : GetLastError();
        CloseHandle(ov.hEvent);
        if (e != ERROR_PIPE_CONNECTED) {
            CloseHandle(client);
            CloseHandle(server);
            return fail(win_error("connect the pipe", e));
        }
    }

    // ---- what the runner inherits, and nothing else -----------------------------
    HANDLE shared = nullptr;
    if (!DuplicateHandle(GetCurrentProcess(), H(region.handle), GetCurrentProcess(), &shared, 0,
                         TRUE, DUPLICATE_SAME_ACCESS)) {
        CloseHandle(client);
        CloseHandle(server);
        return fail(win_error("DuplicateHandle (shared region)"));
    }
    HANDLE log = CreateFileW(spec.log.wstring().c_str(), GENERIC_WRITE,
                             FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, &inherit,
                             CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    auto close_inherited = [&] {
        CloseHandle(client);
        CloseHandle(shared);
        if (log != INVALID_HANDLE_VALUE) CloseHandle(log);
    };

    std::vector<std::string> args = spec.args;
    args.push_back("--link-handles");
    args.push_back(std::to_string(reinterpret_cast<std::uintptr_t>(client)) + "," +
                   std::to_string(reinterpret_cast<std::uintptr_t>(shared)));
    std::wstring cmd;
    for (const auto& a : args) append_quoted(cmd, widen(a));
    const std::wstring app = widen(args[0]);
    std::vector<wchar_t> env = env_block(spec.env);

    HANDLE list[3] = {client, shared, log};
    const DWORD nlist = log != INVALID_HANDLE_VALUE ? 3 : 2;
    SIZE_T attr_size = 0;
    InitializeProcThreadAttributeList(nullptr, 1, 0, &attr_size);
    std::vector<unsigned char> attr_buf(attr_size);
    auto* attrs = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(attr_buf.data());
    if (!InitializeProcThreadAttributeList(attrs, 1, 0, &attr_size) ||
        !UpdateProcThreadAttribute(attrs, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST, list,
                                   nlist * sizeof(HANDLE), nullptr, nullptr)) {
        close_inherited();
        CloseHandle(server);
        return fail(win_error("the inherited-handle list"));
    }
    STARTUPINFOEXW si{};
    si.StartupInfo.cb = sizeof si;
    si.lpAttributeList = attrs;
    if (log != INVALID_HANDLE_VALUE) {
        si.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
        si.StartupInfo.hStdOutput = log;
        si.StartupInfo.hStdError = log;
    }
    PROCESS_INFORMATION pi{};
    const BOOL created = CreateProcessW(
        app.c_str(), cmd.data(), nullptr, nullptr, TRUE,
        EXTENDED_STARTUPINFO_PRESENT | CREATE_UNICODE_ENVIRONMENT | CREATE_NO_WINDOW |
            CREATE_SUSPENDED,
        env.data(), nullptr, &si.StartupInfo, &pi);
    const DWORD create_error = GetLastError();
    DeleteProcThreadAttributeList(attrs);
    close_inherited(); // the runner holds its own copies now
    if (!created) {
        CloseHandle(server);
        return fail(win_error("CreateProcess " + args[0], create_error));
    }

    // Kill-on-close: if the hub dies, so does a runner stuck inside run_frame.
    // (A broken pipe already ends one that is waiting for a grant.)
    HANDLE job = CreateJobObjectW(nullptr, nullptr);
    if (job) {
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION li{};
        li.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        if (!SetInformationJobObject(job, JobObjectExtendedLimitInformation, &li, sizeof li) ||
            !AssignProcessToJobObject(job, pi.hProcess)) {
            CloseHandle(job);
            job = nullptr;
        }
    }
    ResumeThread(pi.hThread);
    CloseHandle(pi.hThread);

    hub_end.handle = N(server);
    hub_end.overlapped = true;
    hub_end.peer_process = N(pi.hProcess);
    out.process = N(pi.hProcess);
    out.job = N(job);
    out.ended = false;
    return true;
}

std::optional<int> wait_exit(RunnerProcess& p, int timeout_ms) {
    if (!p.running()) return std::nullopt;
    const DWORD w = WaitForSingleObject(H(p.process), timeout_ms < 0 ? INFINITE : DWORD(timeout_ms));
    if (w != WAIT_OBJECT_0) return std::nullopt;
    DWORD code = 0;
    GetExitCodeProcess(H(p.process), &code);
    p.ended = true; // the handle stays open until RunnerProcess goes
    return static_cast<int>(code);
}

void kill_runner(RunnerProcess& p) {
    if (p.running()) TerminateProcess(H(p.process), 1);
}

std::optional<int> run_to_completion(const SpawnSpec& spec, int timeout_ms, std::string* error) {
    SECURITY_ATTRIBUTES inherit{sizeof(SECURITY_ATTRIBUTES), nullptr, TRUE};
    HANDLE log = CreateFileW(spec.log.wstring().c_str(), GENERIC_WRITE,
                             FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, &inherit,
                             CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (log == INVALID_HANDLE_VALUE) {
        if (error) *error = win_error("open " + path_utf8(spec.log));
        return std::nullopt;
    }
    std::wstring cmd;
    for (const auto& a : spec.args) append_quoted(cmd, widen(a));
    const std::wstring app = widen(spec.args[0]);
    std::vector<wchar_t> env = env_block(spec.env);
    SIZE_T attr_size = 0;
    InitializeProcThreadAttributeList(nullptr, 1, 0, &attr_size);
    std::vector<unsigned char> attr_buf(attr_size);
    auto* attrs = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(attr_buf.data());
    InitializeProcThreadAttributeList(attrs, 1, 0, &attr_size);
    UpdateProcThreadAttribute(attrs, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST, &log, sizeof(HANDLE),
                              nullptr, nullptr);
    STARTUPINFOEXW si{};
    si.StartupInfo.cb = sizeof si;
    si.lpAttributeList = attrs;
    si.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
    si.StartupInfo.hStdOutput = log;
    si.StartupInfo.hStdError = log;
    PROCESS_INFORMATION pi{};
    const BOOL created = CreateProcessW(
        app.c_str(), cmd.data(), nullptr, nullptr, TRUE,
        EXTENDED_STARTUPINFO_PRESENT | CREATE_UNICODE_ENVIRONMENT | CREATE_NO_WINDOW, env.data(),
        nullptr, &si.StartupInfo, &pi);
    const DWORD create_error = GetLastError();
    DeleteProcThreadAttributeList(attrs);
    CloseHandle(log);
    if (!created) {
        if (error) *error = win_error("CreateProcess " + spec.args[0], create_error);
        return std::nullopt;
    }
    CloseHandle(pi.hThread);
    RunnerProcess p;
    p.process = N(pi.hProcess);
    if (auto code = wait_exit(p, timeout_ms)) return code;
    kill_runner(p);
    wait_exit(p, -1);
    if (error) *error = spec.args[0] + ": still running after " + std::to_string(timeout_ms) +
                        " ms; killed";
    return std::nullopt;
}

// ---- the runner -------------------------------------------------------------------

bool runner_endpoints(const std::string& link_handles, Channel& control, NativeHandle& shared,
                      std::string* error) {
    const auto comma = link_handles.find(',');
    if (link_handles.empty() || comma == std::string::npos) {
        if (error) *error = "on Windows the link needs --link-handles <control>,<shared> "
                            "(the hub passes it)";
        return false;
    }
    const NativeHandle c = static_cast<NativeHandle>(std::stoull(link_handles.substr(0, comma)));
    const NativeHandle s = static_cast<NativeHandle>(std::stoull(link_handles.substr(comma + 1)));
    DWORD flags = 0;
    if (!GetHandleInformation(H(c), &flags) || !GetHandleInformation(H(s), &flags)) {
        if (error) *error = "--link-handles names handles this process does not hold";
        return false;
    }
    control.handle = c;
    control.overlapped = false;
    shared = s;
    return true;
}

std::vector<std::string> utf8_args(int, char**) {
    std::vector<std::string> out;
    int n = 0;
    LPWSTR* w = CommandLineToArgvW(GetCommandLineW(), &n);
    for (int i = 0; w && i < n; ++i) {
        const int len = WideCharToMultiByte(CP_UTF8, 0, w[i], -1, nullptr, 0, nullptr, nullptr);
        std::string s(static_cast<std::size_t>(len > 0 ? len - 1 : 0), '\0');
        if (len > 1) WideCharToMultiByte(CP_UTF8, 0, w[i], -1, s.data(), len, nullptr, nullptr);
        out.push_back(std::move(s));
    }
    if (w) LocalFree(w);
    return out;
}

std::string path_utf8(const fs::path& p) { return p.u8string(); }
fs::path utf8_path(const std::string& s) { return fs::u8path(s); }

} // namespace retro::corelink
