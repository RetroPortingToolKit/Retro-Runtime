#include "state_envelope.hpp"

#include "sha256.hpp"

#include <cstring>
#include <fstream>
#include <sstream>

// The header's lines, one `key=value` each, values escaped (\\ and \n):
//
//   abi_major=0
//   core_id=n64lle
//   core_sha256=<hex>
//   state_compat_id=<id>              absent when the core declares none
//   package_sha256=<hex>              absent without a game package
//   content_sha256=<hex>
//   accessory=<seat>,<slot>,<type id>,<content hex or empty>   one per binding
//   option=<key>=<value>              one per NETPLAY option that is set
//   option_unset=<key>                one per NETPLAY option that is not
//   state_size=<bytes>
//   state_sha256=<hex>
//   frame=<n>
//   saved_unix=<seconds>
//   thumb=<w>x<h>                     absent without a thumbnail

namespace retro::state {

namespace {

std::string escape(const std::string& s) {
    std::string o;
    for (char c : s) {
        if (c == '\\') o += "\\\\";
        else if (c == '\n') o += "\\n";
        else o += c;
    }
    return o;
}

std::string unescape(const std::string& s) {
    std::string o;
    for (std::size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '\\' && i + 1 < s.size()) {
            o += s[i + 1] == 'n' ? '\n' : s[i + 1];
            ++i;
        } else {
            o += s[i];
        }
    }
    return o;
}

void put_u32(std::string& out, std::uint32_t v) {
    for (int i = 0; i < 4; ++i) out += static_cast<char>((v >> (8 * i)) & 0xff);
}
void put_u64(std::string& out, std::uint64_t v) {
    for (int i = 0; i < 8; ++i) out += static_cast<char>((v >> (8 * i)) & 0xff);
}
bool get_u32(std::istream& in, std::uint32_t& v) {
    unsigned char b[4];
    if (!in.read(reinterpret_cast<char*>(b), 4)) return false;
    v = std::uint32_t(b[0]) | std::uint32_t(b[1]) << 8 | std::uint32_t(b[2]) << 16 |
        std::uint32_t(b[3]) << 24;
    return true;
}
bool get_u64(std::istream& in, std::uint64_t& v) {
    unsigned char b[8];
    if (!in.read(reinterpret_cast<char*>(b), 8)) return false;
    v = 0;
    for (int i = 7; i >= 0; --i) v = v << 8 | b[i];
    return true;
}

std::string header_text(const StateHeader& h) {
    const StateIdentity& id = h.identity;
    std::ostringstream o;
    o << "abi_major=" << id.abi_major << '\n';
    o << "core_id=" << escape(id.core_id) << '\n';
    o << "core_sha256=" << id.core_sha256 << '\n';
    if (id.state_compat_id) o << "state_compat_id=" << escape(*id.state_compat_id) << '\n';
    if (!id.package_sha256.empty()) o << "package_sha256=" << id.package_sha256 << '\n';
    o << "content_sha256=" << id.content_sha256 << '\n';
    for (const AccessoryIdentity& a : id.accessories) {
        o << "accessory=" << a.seat << ',' << a.slot << ',' << escape(a.type_id) << ','
          << a.content_sha256 << '\n';
    }
    for (const auto& [k, v] : id.sim_options) {
        if (v) o << "option=" << escape(k) << '=' << escape(*v) << '\n';
        else o << "option_unset=" << escape(k) << '\n';
    }
    o << "state_size=" << h.state_size << '\n';
    o << "state_sha256=" << h.state_sha256 << '\n';
    o << "frame=" << h.frame_number << '\n';
    o << "saved_unix=" << h.saved_unix << '\n';
    if (h.thumb_w && h.thumb_h) o << "thumb=" << h.thumb_w << 'x' << h.thumb_h << '\n';
    return o.str();
}

std::uint64_t to_u64(const std::string& s) { return std::strtoull(s.c_str(), nullptr, 10); }

void parse_header_text(const std::string& text, StateHeader& h) {
    StateIdentity& id = h.identity;
    std::istringstream in(text);
    std::string line;
    while (std::getline(in, line)) {
        const auto eq = line.find('=');
        if (eq == std::string::npos) continue;
        const std::string k = line.substr(0, eq);
        const std::string v = line.substr(eq + 1);
        if (k == "abi_major") id.abi_major = static_cast<std::uint32_t>(to_u64(v));
        else if (k == "core_id") id.core_id = unescape(v);
        else if (k == "core_sha256") id.core_sha256 = v;
        else if (k == "state_compat_id") id.state_compat_id = unescape(v);
        else if (k == "package_sha256") id.package_sha256 = v;
        else if (k == "content_sha256") id.content_sha256 = v;
        else if (k == "accessory") {
            // seat,slot,type,content: the type id may not contain a comma
            // (it is a dotted identifier), the content is hex.
            AccessoryIdentity a;
            std::size_t p1 = v.find(','), p2 = v.find(',', p1 + 1), p3 = v.rfind(',');
            if (p1 == std::string::npos || p2 == std::string::npos || p3 <= p2) continue;
            a.seat = static_cast<std::uint32_t>(to_u64(v.substr(0, p1)));
            a.slot = static_cast<std::uint32_t>(to_u64(v.substr(p1 + 1, p2 - p1 - 1)));
            a.type_id = unescape(v.substr(p2 + 1, p3 - p2 - 1));
            a.content_sha256 = v.substr(p3 + 1);
            id.accessories.push_back(a);
        } else if (k == "option") {
            // key=value inside the value: option keys are dotted identifiers.
            const auto eq2 = v.find('=');
            if (eq2 == std::string::npos) continue;
            id.sim_options[unescape(v.substr(0, eq2))] = unescape(v.substr(eq2 + 1));
        } else if (k == "option_unset") {
            id.sim_options[unescape(v)] = std::nullopt;
        } else if (k == "state_size") h.state_size = to_u64(v);
        else if (k == "state_sha256") h.state_sha256 = v;
        else if (k == "frame") h.frame_number = to_u64(v);
        else if (k == "saved_unix") h.saved_unix = std::strtoll(v.c_str(), nullptr, 10);
        else if (k == "thumb") {
            const auto x = v.find('x');
            if (x == std::string::npos) continue;
            h.thumb_w = static_cast<std::uint32_t>(to_u64(v.substr(0, x)));
            h.thumb_h = static_cast<std::uint32_t>(to_u64(v.substr(x + 1)));
        }
        // Anything else is a later version's field: kept out, not refused.
    }
}

// Opens the file and reads through the thumbnail. On return `in` is
// positioned at the state's length field.
bool read_through_thumb(std::ifstream& in, const fs::path& path, StateHeader& out,
                        std::vector<std::uint8_t>* thumb, std::string* error) {
    auto fail = [&](const std::string& m) {
        if (error) *error = path.string() + ": " + m;
        return false;
    };
    in.open(path, std::ios::binary);
    if (!in) return fail("cannot open");
    char magic[8];
    if (!in.read(magic, 8) || std::memcmp(magic, kEnvelopeMagic, 8) != 0) {
        return fail("not a savestate envelope");
    }
    std::uint32_t version = 0, header_len = 0;
    if (!get_u32(in, version) || !get_u32(in, header_len)) return fail("truncated");
    if (version == 0 || version > kEnvelopeVersion) {
        return fail("envelope version " + std::to_string(version) + ", this build reads up to " +
                    std::to_string(kEnvelopeVersion));
    }
    if (header_len > (1u << 20)) return fail("header too large");
    std::string text(header_len, '\0');
    if (header_len && !in.read(text.data(), header_len)) return fail("truncated header");
    out = StateHeader{};
    out.version = version;
    parse_header_text(text, out);
    std::uint64_t thumb_len = 0;
    if (!get_u64(in, thumb_len)) return fail("truncated");
    const std::uint64_t want = std::uint64_t(out.thumb_w) * out.thumb_h * 4;
    if (thumb_len != want) return fail("thumbnail size disagrees with its header");
    if (thumb) {
        thumb->resize(thumb_len);
        if (thumb_len && !in.read(reinterpret_cast<char*>(thumb->data()), std::streamsize(thumb_len))) {
            return fail("truncated thumbnail");
        }
    } else {
        in.seekg(std::streamoff(thumb_len), std::ios::cur);
    }
    return true;
}

} // namespace

std::vector<std::uint8_t> make_thumbnail(const std::uint8_t* rgba, std::uint32_t width,
                                         std::uint32_t height, std::uint32_t stride) {
    std::vector<std::uint8_t> out;
    if (!rgba || !width || !height) return out;
    out.resize(std::size_t(kThumbWidth) * kThumbHeight * 4);
    for (std::uint32_t y = 0; y < kThumbHeight; ++y) {
        const std::uint8_t* row = rgba + std::size_t(y * height / kThumbHeight) * stride;
        std::uint8_t* dst = out.data() + std::size_t(y) * kThumbWidth * 4;
        for (std::uint32_t x = 0; x < kThumbWidth; ++x) {
            const std::uint8_t* px = row + std::size_t(x * width / kThumbWidth) * 4;
            dst[x * 4 + 0] = px[0];
            dst[x * 4 + 1] = px[1];
            dst[x * 4 + 2] = px[2];
            dst[x * 4 + 3] = 0xff;
        }
    }
    return out;
}

bool write_state(const fs::path& path, StateHeader header, const std::vector<std::uint8_t>& thumb,
                 const void* state, std::uint64_t size, std::string* error) {
    auto fail = [&](const std::string& m) {
        if (error) *error = path.string() + ": " + m;
        return false;
    };
    const bool has_thumb = thumb.size() == std::size_t(kThumbWidth) * kThumbHeight * 4;
    header.version = kEnvelopeVersion;
    header.state_size = size;
    header.state_sha256 = runner::sha256_hex(state, static_cast<std::size_t>(size));
    header.thumb_w = has_thumb ? kThumbWidth : 0;
    header.thumb_h = has_thumb ? kThumbHeight : 0;

    std::string head(kEnvelopeMagic, 8);
    put_u32(head, kEnvelopeVersion);
    const std::string text = header_text(header);
    put_u32(head, static_cast<std::uint32_t>(text.size()));
    head += text;
    put_u64(head, has_thumb ? thumb.size() : 0);

    std::error_code ec;
    if (path.has_parent_path()) fs::create_directories(path.parent_path(), ec);
    const fs::path tmp = path.string() + ".tmp";
    {
        std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
        if (!out) return fail("cannot write");
        out.write(head.data(), std::streamsize(head.size()));
        if (has_thumb) out.write(reinterpret_cast<const char*>(thumb.data()), std::streamsize(thumb.size()));
        std::string len;
        put_u64(len, size);
        out.write(len.data(), 8);
        out.write(static_cast<const char*>(state), std::streamsize(size));
        if (!out) {
            out.close();
            fs::remove(tmp, ec);
            return fail("write failed");
        }
    }
    fs::rename(tmp, path, ec);
    if (ec) {
        fs::remove(tmp, ec);
        return fail("cannot replace: " + ec.message());
    }
    return true;
}

bool is_envelope(const fs::path& path) {
    std::ifstream in(path, std::ios::binary);
    char magic[8];
    return in.read(magic, 8) && std::memcmp(magic, kEnvelopeMagic, 8) == 0;
}

bool read_state_header(const fs::path& path, StateHeader& out, std::vector<std::uint8_t>* thumb,
                       std::string* error) {
    std::ifstream in;
    return read_through_thumb(in, path, out, thumb, error);
}

bool read_state(const fs::path& path, StateHeader& out, std::vector<std::uint8_t>& state,
                std::string* error) {
    std::ifstream in;
    if (!read_through_thumb(in, path, out, nullptr, error)) return false;
    std::uint64_t len = 0;
    if (!get_u64(in, len)) {
        if (error) *error = path.string() + ": truncated";
        return false;
    }
    if (len != out.state_size) {
        if (error) {
            *error = path.string() + ": the state is " + std::to_string(len) +
                     " bytes, its header says " + std::to_string(out.state_size);
        }
        return false;
    }
    state.resize(static_cast<std::size_t>(len));
    if (len && !in.read(reinterpret_cast<char*>(state.data()), std::streamsize(len))) {
        if (error) *error = path.string() + ": truncated state";
        return false;
    }
    return true;
}

std::string check_state(const StateHeader& saved, const StateIdentity& running,
                        const std::vector<std::uint8_t>& state) {
    const StateIdentity& s = saved.identity;
    auto differs = [](const std::string& what, const std::string& in_state,
                      const std::string& here, const char* verb = "differs") {
        return what + " " + verb + ": the state has " + (in_state.empty() ? "(none)" : in_state) +
               ", this session has " + (here.empty() ? "(none)" : here);
    };
    // 1. ABI major, core id.
    if (s.abi_major != running.abi_major) {
        return differs("rcore ABI major", std::to_string(s.abi_major),
                       std::to_string(running.abi_major));
    }
    if (s.core_id != running.core_id) return differs("core id", s.core_id, running.core_id);
    // 2. Build identity: the core's promise when it made one, else its file.
    if (running.state_compat_id || s.state_compat_id) {
        if (s.state_compat_id != running.state_compat_id) {
            return differs("state_compat_id", s.state_compat_id.value_or(""),
                           running.state_compat_id.value_or(""));
        }
    } else if (s.core_sha256 != running.core_sha256) {
        return differs("core build (SHA-256; this core binds states to its exact file)",
                       s.core_sha256, running.core_sha256);
    }
    // 3. Game package, content. A core that declares a state_compat_id has
    // promised that its own load check decides whether a state survives a
    // rebuild, and a game package is part of that build: comparing its hash
    // here refused every slot after every package regeneration while the
    // engine would have loaded them (owner ruling 2026-09-29, CORE_ABI.md
    // "Savestates"). A core that made no promise keeps the comparison; nothing
    // else guards it.
    if (!running.state_compat_id && s.package_sha256 != running.package_sha256) {
        return differs("game package SHA-256", s.package_sha256, running.package_sha256);
    }
    if (s.content_sha256 != running.content_sha256) {
        return differs("content SHA-256", s.content_sha256, running.content_sha256);
    }
    // 4. Accessories, then simulation options.
    auto describe = [](const std::vector<AccessoryIdentity>& v) {
        std::string o;
        for (const AccessoryIdentity& a : v) {
            if (!o.empty()) o += "; ";
            o += "seat " + std::to_string(a.seat) + " slot " + std::to_string(a.slot) + " " +
                 a.type_id + (a.content_sha256.empty() ? "" : " " + a.content_sha256);
        }
        return o;
    };
    if (!(s.accessories == running.accessories)) {
        return differs("accessories", describe(s.accessories), describe(running.accessories),
                       "differ");
    }
    for (const auto& [k, v] : running.sim_options) {
        const auto it = s.sim_options.find(k);
        const std::string here = v ? *v : "(unset)";
        const std::string there =
            it == s.sim_options.end() ? "(not recorded)" : it->second ? *it->second : "(unset)";
        if (it == s.sim_options.end() || it->second != v) return differs("option " + k, there, here);
    }
    for (const auto& [k, v] : s.sim_options) {
        if (!running.sim_options.count(k)) {
            return differs("option " + k, v ? *v : "(unset)", "(not declared)");
        }
    }
    // 5. The core's bytes, against the hash taken when they were written.
    if (state.size() != saved.state_size ||
        runner::sha256_hex(state.data(), state.size()) != saved.state_sha256) {
        return "the state's bytes do not match the SHA-256 recorded when it was saved "
               "(the file is corrupt)";
    }
    return {};
}

} // namespace retro::state
