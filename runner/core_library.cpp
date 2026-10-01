#include "core_library.hpp"

#include "sha256.hpp"

#include <cstring>

#if defined(_WIN32)
#  include <windows.h>
#else
#  include <cerrno>
#  include <cstdlib>
#  include <dlfcn.h>
#  include <fcntl.h>
#  include <sys/stat.h>
#  include <unistd.h>
#endif

namespace retro::runner {

namespace {

struct CapName {
    std::uint64_t bit;
    const char* name;
};

// The same spellings the sidecar manifest uses (n64lle rcore_probe --manifest).
constexpr CapName kCaps[] = {
    {RCORE_CAP_RUN_FRAME, "run_frame"},
    {RCORE_CAP_OWNS_LOOP, "owns_loop"},
    {RCORE_CAP_SAVESTATE, "savestate"},
    {RCORE_CAP_DETERMINISTIC, "deterministic"},
    {RCORE_CAP_ROLLBACK, "rollback"},
    {RCORE_CAP_RESET, "reset"},
    {RCORE_CAP_STRICT_MODE, "strict_mode"},
    {RCORE_CAP_GAME_PACKAGE, "game_package"},
    {RCORE_CAP_ACCESSORY_HOTPLUG, "accessory_hotplug"},
    {RCORE_CAP_GL_COMPUTE, "gl_compute"},
    {RCORE_CAP_ACCESSORY_DATA, "accessory_data"},
};

} // namespace

std::string capability_names(std::uint64_t caps, std::uint64_t* unnamed) {
    std::string out;
    std::uint64_t rest = caps;
    for (const CapName& c : kCaps) {
        if (caps & c.bit) {
            if (!out.empty()) out += ',';
            out += c.name;
            rest &= ~c.bit;
        }
    }
    if (unnamed) *unnamed = rest;
    return out;
}

bool capability_name_known(const std::string& name) {
    for (const CapName& c : kCaps) {
        if (name == c.name) return true;
    }
    return false;
}

bool load_core(const fs::path& path, LoadedCore& out, std::string* error) {
    auto fail = [&](const std::string& m) {
        if (error) *error = path.string() + ": " + m;
        return false;
    };
    out.path = path;

#if defined(_WIN32)
    // Hold the file open without write sharing while hashing and loading, so
    // nothing can replace it between the two.
    // The core's own DLLs resolve beside it, and the working directory is
    // never searched.
    const fs::path abs = fs::absolute(path);
    HANDLE f = CreateFileW(abs.wstring().c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                           OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) return fail("cannot open");
    out.sha256 = file_sha256_hex(abs);
    HMODULE lib = LoadLibraryExW(abs.wstring().c_str(), nullptr,
                                 LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR |
                                     LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
    CloseHandle(f);
    if (!lib) return fail("LoadLibrary failed (error " + std::to_string(GetLastError()) + ")");
    out.handle = lib;
    void* sym = reinterpret_cast<void*>(GetProcAddress(lib, RCORE_ENTRY_SYMBOL));
#elif defined(__APPLE__)
    // No /proc/self/fd here. Copy the library out of the one open file into a
    // private 0700 directory, then hash and load that copy: nothing else can
    // write there, so the hash names the bytes that load. A signed dylib keeps
    // its signature, which lives in the file. The copy keeps the file name;
    // a core that finds siblings through @loader_path would not find them
    // (none does today).
    const int fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
    if (fd < 0) return fail(std::string("cannot open: ") + std::strerror(errno));
    const char* tmp = std::getenv("TMPDIR");
    std::string dir_template = std::string(tmp && *tmp ? tmp : "/tmp") + "/retro-core-XXXXXX";
    if (!::mkdtemp(dir_template.data())) {
        ::close(fd);
        return fail(std::string("mkdtemp: ") + std::strerror(errno));
    }
    const fs::path copy = fs::path(dir_template) / path.filename();
    const int cfd = ::open(copy.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0500);
    bool copied = cfd >= 0;
    char chunk[1 << 16];
    for (ssize_t n; copied && (n = ::read(fd, chunk, sizeof chunk)) != 0;) {
        if (n < 0) {
            if (errno == EINTR) continue;
            copied = false;
            break;
        }
        for (ssize_t off = 0; off < n;) {
            const ssize_t w = ::write(cfd, chunk + off, static_cast<size_t>(n - off));
            if (w < 0 && errno == EINTR) continue;
            if (w <= 0) { copied = false; break; }
            off += w;
        }
    }
    ::close(fd);
    if (cfd >= 0) ::close(cfd);
    void* lib = nullptr;
    if (copied) {
        out.sha256 = file_sha256_hex(copy);
        lib = ::dlopen(copy.c_str(), RTLD_NOW | RTLD_LOCAL);
    }
    const char* dl_text = lib ? nullptr : (copied ? ::dlerror() : "copying the library failed");
    const std::string dl_error = dl_text ? dl_text : "";
    ::unlink(copy.c_str()); // the loaded image stays mapped
    ::rmdir(dir_template.c_str());
    if (!lib) return fail("dlopen: " + dl_error);
    out.handle = lib;
    void* sym = ::dlsym(lib, RCORE_ENTRY_SYMBOL);
#else
    // One open file: hash it through /proc/self/fd and dlopen that same path,
    // which resolves to the inode already open rather than to whatever the
    // directory entry names by then.
    const int fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
    if (fd < 0) return fail(std::string("cannot open: ") + std::strerror(errno));
    const std::string via = "/proc/self/fd/" + std::to_string(fd);
    out.sha256 = file_sha256_hex(via);
    void* lib = ::dlopen(via.c_str(), RTLD_NOW | RTLD_LOCAL);
    ::close(fd);
    if (!lib) return fail(std::string("dlopen: ") + ::dlerror());
    out.handle = lib;
    void* sym = ::dlsym(lib, RCORE_ENTRY_SYMBOL);
#endif
    if (out.sha256.empty()) return fail("could not hash the library");
    if (!sym) return fail("exports no " RCORE_ENTRY_SYMBOL);

    auto entry = reinterpret_cast<rcore_entry_fn>(sym);
    out.api = entry(RCORE_ABI_MAJOR);
    if (!out.api) {
        return fail("rcore_entry refused ABI major " + std::to_string(RCORE_ABI_MAJOR));
    }
    if (!out.api->info) return fail("the core table carries no info");
    out.info = out.api->info;
    if (out.info->abi_major != RCORE_ABI_MAJOR) {
        return fail("the core was built against ABI major " +
                    std::to_string(out.info->abi_major) + ", this runner speaks " +
                    std::to_string(RCORE_ABI_MAJOR));
    }
    return true;
}

} // namespace retro::runner
