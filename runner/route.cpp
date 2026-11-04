#include "route.hpp"
#include "sha256.hpp"
#include "transport.hpp"

#include <charconv>
#include <cstring>
#include <iomanip>
#include <sstream>
#if defined(_WIN32)
#include <windows.h>
#else
extern char** environ;
#endif

namespace retro::runner {
namespace {
constexpr std::size_t limit = 256 * 1024 * 1024;
std::string hex(const void* data, std::size_t n) {
    if (!n) return "-";
    static const char digits[] = "0123456789abcdef";
    const auto* p = static_cast<const unsigned char*>(data);
    std::string s(n * 2, '0');
    for (std::size_t i = 0; i < n; ++i) { s[2*i] = digits[p[i] >> 4]; s[2*i+1] = digits[p[i] & 15]; }
    return s;
}
std::string hex(const std::string& s) { return hex(s.data(), s.size()); }
std::string unhex(const std::string& s) {
    if (s == "-") return {};
    if (s.size() % 2 || s.size() > limit) throw std::runtime_error("invalid hex length");
    std::string out(s.size() / 2, '\0');
    auto nibble = [](char c) -> unsigned { if (c >= '0' && c <= '9') return c-'0'; if (c >= 'a' && c <= 'f') return c-'a'+10; throw std::runtime_error("invalid hex digit"); };
    for (std::size_t i = 0; i < out.size(); ++i) out[i] = char((nibble(s[2*i]) << 4) | nibble(s[2*i+1]));
    return out;
}
std::string word(std::istringstream& s) { std::string v; if (!(s >> v)) throw std::runtime_error("missing field"); return v; }
template<class T> T number(std::istringstream& s) {
    const std::string v = word(s); T n{};
    const auto r = std::from_chars(v.data(), v.data()+v.size(), n);
    if (r.ec != std::errc() || r.ptr != v.data()+v.size()) throw std::runtime_error("invalid number");
    return n;
}
void end_line(std::istringstream& s) { std::string v; if (s >> v) throw std::runtime_error("extra field"); }
std::string json(const std::string& s) {
    std::ostringstream o; o << '"';
    for (unsigned char c : s) {
        if (c == '"' || c == '\\') o << '\\' << char(c);
        else if (c < 32) o << "\\u00" << std::hex << std::setw(2) << std::setfill('0') << unsigned(c);
        else o << char(c);
    }
    return o.str() + '"';
}
std::map<std::string,std::string> engine_environment() {
    std::map<std::string,std::string> result;
    auto take=[&](const std::string& entry) {
        if(entry.rfind("N64LLE_",0)!=0)return;
        const auto eq=entry.find('=');
        if(eq!=std::string::npos)result["env:"+entry.substr(0,eq)]=entry.substr(eq+1);
    };
#if defined(_WIN32)
    wchar_t* block=GetEnvironmentStringsW();
    if(!block)throw std::runtime_error("cannot read engine environment");
    for(const wchar_t* p=block;*p;p+=wcslen(p)+1) {
        const int size=WideCharToMultiByte(CP_UTF8,0,p,-1,nullptr,0,nullptr,nullptr);
        std::string value(size,'\0');
        WideCharToMultiByte(CP_UTF8,0,p,-1,value.data(),size,nullptr,nullptr);
        value.pop_back();take(value);
    }
    FreeEnvironmentStringsW(block);
#else
    for(char** p=environ;p && *p;++p)take(*p);
#endif
    return result;
}
}

Route::Route(const LoadedCore& c, HostSession& s, const RouteArgs& a, const fs::path& out)
    : core_(c), session_(s), args_(a), out_(out) {}

bool Route::fail(const std::string& s, bool divergence) {
    if (error_.empty()) { error_ = s; diverged_ = divergence; }
    return false;
}

bool Route::open(const std::string& rom, const std::string& package_sha,
                 const std::string& title_dir, bool strict) {
    if (args_.record.empty() == args_.play.empty()) return fail("select exactly one route record/play mode");
    identity_["rom"] = file_sha256_hex(retro::corelink::utf8_path(rom));
    identity_["package"] = package_sha;
    identity_["core"] = core_.sha256;
    identity_["core_id"] = core_.info->core_id;
    identity_["core_version"] = core_.info->core_version;
    identity_["runner"] = file_sha256_hex(args_.runner);
    const fs::path game = retro::corelink::utf8_path(title_dir) / "game.toml";
    identity_["game.toml"] = fs::exists(game) ? file_sha256_hex(game) : "absent";
    identity_["strict"] = strict ? "1" : "0";
    for (const auto& [k,v] : session_.options()) identity_["option:" + k] = v ? "set:" + *v : "unset";
    const auto environment=engine_environment();
    identity_.insert(environment.begin(),environment.end());
    if (identity_["rom"].empty() || identity_["runner"].empty()) return fail("unreadable ROM or runner identity");
    if (playback()) {
        if (!read()) return false;
        auto recorded = recorded_identity_, actual = identity_;
        if (!args_.runner_reference.empty()) {
            const auto reference = file_sha256_hex(args_.runner_reference);
            if (reference.empty() || reference != recorded_identity_["runner"])
                return fail("runner reference does not match the recorded runner");
            recorded.erase("runner"); actual.erase("runner");
        }
        if (args_.allow_core_change) {
            for (const char* key : {"core", "core_version"}) { recorded.erase(key); actual.erase(key); }
            // Compare an upgrade with an expanded option surface. Existing
            // recorded options remain exact; only NEW declared defaults are
            // admitted, and the verdict names them. Removed options refuse.
            if(recorded_identity_["core"] != identity_["core"]) {
                std::uint32_t n=0;
                const auto* options=core_.api->options ? core_.api->options(&n) : nullptr;
                for(std::uint32_t i=0;options && i<n;++i) {
                    const auto& option=options[i]; const std::string key="option:"+std::string(option.key);
                    const std::string value=option.default_value ? "set:"+std::string(option.default_value) : "unset";
                    if(!recorded.count(key) && actual[key]==value) {
                        added_defaults_.push_back(key+"="+value); actual.erase(key);
                    }
                }
            }
        }
        if (recorded != actual) {
            for (const auto& [k,v] : actual) if (!recorded.count(k) || recorded[k] != v) return fail("route identity mismatch: " + k);
            return fail("route identity contains an unsupported key");
        }
    } else {
        partial_ = args_.record; partial_ += ".partial";
        if (fs::exists(args_.record) || fs::exists(partial_)) return fail("route or partial file already exists");
        if (!args_.record.parent_path().empty()) fs::create_directories(args_.record.parent_path());
        file_.open(partial_, std::ios::binary | std::ios::out);
        if (!file_) return fail("cannot create route partial file");
        file_ << "RCORE_ROUTE 1\n";
        for (const auto& [k,v] : identity_) file_ << "I " << hex(k) << ' ' << hex(v) << '\n';
    }
    return true;
}

bool Route::capture_anchor() {
    const auto& api = *core_.api;
    if (playback() || !(core_.info->capabilities & RCORE_CAP_SAVESTATE) ||
        !api.serialize_size || !api.serialize || !api.unserialize)
        return fail("this core cannot anchor a replay");
    const auto size = api.serialize_size();
    if (!size || size > limit/4) return fail("replay anchor is empty or too large");
    anchor_.resize(static_cast<std::size_t>(size));
    if (api.serialize(anchor_.data(),size) != RCORE_OK)
        return fail("the core refused to save the replay anchor");
    const auto before=state_digest();
    // Record and playback both follow the core's restore path.
    if (api.unserialize(anchor_.data(),size) != RCORE_OK)
        return fail("the core refused to restore the replay anchor");
    anchor_state_=state_digest();
    if (before!=anchor_state_) {
        expected_state_=before; actual_state_=anchor_state_;
        return fail("the core changed state while restoring the replay anchor",true);
    }
    file_ << "A " << hex(anchor_state_) << ' ' << hex(anchor_.data(),anchor_.size()) << '\n';
    return bool(file_);
}

bool Route::initial_saves() {
    const auto& regions = session_.save_regions();
    if (playback()) {
        if (regions.size() != saves_.size()) return fail("initial save region count mismatch");
        for (std::size_t i = 0; i < regions.size(); ++i) {
            const auto& r = regions[i]; const auto& s = saves_[i];
            if (r.id != s.id || r.kind != s.kind || r.seat != s.seat || r.slot != s.slot || r.erase_value != s.erase || r.size != s.bytes.size()) return fail("initial save region mismatch");
            if(r.size)std::memcpy(r.data, s.bytes.data(), r.size);
        }
        if (!calls_complete()) return false;
        if (anchored() && (!core_.api->unserialize ||
            core_.api->unserialize(anchor_.data(),anchor_.size()) != RCORE_OK))
            return fail("the core refused the replay anchor");
        if (anchored() && state_digest()!=anchor_state_) {
            expected_state_=anchor_state_; actual_state_=state_digest();
            return fail("replay anchor state differs before the first field",true);
        }
    } else {
        for (const auto& r : regions) {
            if (r.size > limit/2) return fail("initial save region too large");
            file_ << "S " << hex(r.id) << ' ' << r.kind << ' ' << r.seat << ' ' << r.slot << ' ' << r.erase_value << ' ' << hex(r.data,r.size) << '\n';
        }
        if (!write_field(boot_, true)) return false;
    }
    started_ = true;
    return true;
}

bool Route::begin(std::uint32_t w, std::uint32_t h) {
    if (!started_ || done() || !error_.empty()) return fail("route frame is not available");
    current_ = {}; current_.width = w; current_.height = h;
    if (playback()) {
        current_.width = frames_[next_].width; current_.height = frames_[next_].height;
    }
    input_cursor_ = clock_cursor_ = 0;
    pcm_.clear();
    session_.set_presentation_size(current_.width, current_.height);
    return true;
}

bool Route::input(std::uint32_t seat, rcore_pad& pad) {
    if (!playback()) return false;
    const Field& f = started_ && next_ < frames_.size() ? frames_[next_] : boot_;
    if (input_cursor_ >= f.inputs.size() || f.inputs[input_cursor_].seat != seat) {
        fail("input call order differs at field " + std::to_string(next_ + (started_ ? 1 : 0)), true);
        pad = {}; pad.struct_size = sizeof pad;
    } else pad = f.inputs[input_cursor_++].pad;
    return true;
}
void Route::delivered(std::uint32_t seat, const rcore_pad& pad) {
    if (!playback()) {
        auto& f = started_ ? current_ : boot_;
        if (f.inputs.size() >= 4096) { fail("too many input calls in one field"); return; }
        f.inputs.push_back({seat,pad});
    }
}
std::uint64_t Route::clock(std::uint64_t live) {
    if (!playback()) {
        auto& f = started_ ? current_ : boot_;
        if (f.clocks.size() >= 4096) { fail("too many clock calls in one field"); return 0; }
        f.clocks.push_back(live); return live;
    }
    const auto& f = started_ && next_ < frames_.size() ? frames_[next_] : boot_;
    if (clock_cursor_ >= f.clocks.size()) { fail("clock call count differs", true); return 0; }
    return f.clocks[clock_cursor_++];
}
void Route::video(const rcore_frame& f) {
    if (!f.pixels || f.pixel_format != RCORE_PIXEL_RGBA8 || !f.width || !f.height || std::uint64_t(f.width)*f.height > 16*1024*1024 || f.stride < std::uint64_t(f.width)*4) { fail("unsupported route picture"); return; }
    picture_width_ = f.width; picture_height_ = f.height;
    picture_.resize(std::size_t(f.width)*f.height*4);
    for (std::uint32_t y=0;y<f.height;++y) std::memcpy(picture_.data()+std::size_t(y)*f.width*4, static_cast<const std::uint8_t*>(f.pixels)+std::size_t(y)*f.stride, std::size_t(f.width)*4);
}
void Route::audio(const std::int16_t* s, std::uint32_t n) {
    if (std::uint64_t(n)*4 + pcm_.size() > 16*1024*1024) { fail("too much audio in one field"); return; }
    // Explicit little endian, independent of the host's byte order.
    for (std::uint64_t i=0;i<std::uint64_t(n)*2;++i) { pcm_.push_back(std::uint8_t(s[i])); pcm_.push_back(std::uint16_t(s[i]) >> 8); }
}
void Route::gpu_video() { gpu_ = true; picture_.clear(); }

bool Route::calls_complete() {
    if (!playback()) return true;
    const auto& f = started_ ? frames_[next_] : boot_;
    if (input_cursor_ != f.inputs.size() || clock_cursor_ != f.clocks.size()) return fail("input/clock call count differs at field " + std::to_string(next_ + (started_ ? 1 : 0)), true);
    return error_.empty();
}

std::string Route::state_digest() const {
    std::string digest="-";
    if (RCORE_HAS(core_.api,rcore_core_api,state_hash) && core_.api->state_hash) {
        digest = std::to_string(core_.api->state_hash());
        if (RCORE_HAS(core_.api,rcore_core_api,state_hash_parts) && core_.api->state_hash_parts) {
            std::uint64_t parts[3]{}; const char* names[3]{};
            core_.api->state_hash_parts(parts,names);
            for (unsigned i=0;i<3;++i) digest += ":" + std::to_string(parts[i]);
        }
    }
    return digest;
}
bool Route::end() {
    if (!calls_complete()) return false;
    current_.state=state_digest();
    current_.image = picture_.empty() ? "-" : std::to_string(picture_width_) + ":" + std::to_string(picture_height_) + ":" + sha256_hex(picture_.data(),picture_.size());
    current_.audio = sha256_hex(pcm_.data(),pcm_.size());
    const auto mark = args_.marks.find(next_+1);
    current_.mark = playback() ? frames_[next_].mark : mark == args_.marks.end() ? "" : mark->second;
    if (playback()) {
        const auto& f = frames_[next_];
        if (f.state != current_.state) {
            expected_state_=f.state; actual_state_=current_.state;
            fail("state checkpoint differs at field " + std::to_string(next_+1),true);
        }
        else if (f.image != current_.image) fail("picture checkpoint differs at field " + std::to_string(next_+1),true);
        else if (f.audio != current_.audio) fail("audio checkpoint differs at field " + std::to_string(next_+1),true);
    }
    if ((!current_.mark.empty() || !error_.empty()) && !picture_.empty()) {
        std::ofstream shot(out_ / ("route-"+std::to_string(next_+1)+".ppm"),std::ios::binary);
        shot << "P6\n" << picture_width_ << ' ' << picture_height_ << "\n255\n";
        for (std::size_t i=0;i<picture_.size();i+=4) shot.write(reinterpret_cast<const char*>(picture_.data()+i),3);
        if (!shot) fail("cannot write checkpoint picture");
    }
    if (!playback() && error_.empty() && !write_field(current_,false)) return false;
    if (error_.empty()) ++next_;
    return error_.empty();
}

bool Route::write_field(const Field& f, bool boot) {
    file_ << (boot ? "B 0" : "F " + std::to_string(next_+1)) << ' ' << f.width << ' ' << f.height << ' ' << f.inputs.size() << ' ' << f.clocks.size() << ' ' << hex(f.state) << ' ' << hex(f.image) << ' ' << hex(f.audio) << ' ' << hex(f.mark) << '\n';
    for (const auto& i : f.inputs) {
        file_ << "P " << i.seat << ' ' << i.pad.connected << ' ' << i.pad.buttons;
        for (auto a : i.pad.axes) file_ << ' ' << a;
        file_ << '\n';
    }
    for (auto t : f.clocks) file_ << "T " << t << '\n';
    file_.flush();
    if(file_.tellp()>std::streamoff(limit-1024))return fail("route exceeds 256 MiB limit");
    return file_ ? true : fail("cannot write route");
}

bool Route::read() {
    try {
        std::error_code ec; const auto size = fs::file_size(args_.play,ec);
        if (ec || size > limit || size < 16) throw std::runtime_error("missing or oversized route");
        std::ifstream in(args_.play,std::ios::binary);
        std::string text(std::istreambuf_iterator<char>(in),{});
        const auto footer = text.rfind("\nEND ");
        if (footer == std::string::npos || text.back() != '\n') throw std::runtime_error("incomplete route (missing END)");
        const std::string prefix = text.substr(0,footer+1);
        std::istringstream tail(text.substr(footer+1));
        if (word(tail) != "END") throw std::runtime_error("missing END");
        const auto count = number<std::uint64_t>(tail);
        if (word(tail) != sha256_hex(prefix.data(),prefix.size())) throw std::runtime_error("route checksum mismatch");
        end_line(tail);
        std::istringstream lines(prefix); std::string line;
        if (!std::getline(lines,line) || line != "RCORE_ROUTE 1") throw std::runtime_error("unsupported route format");
        bool boot_seen = false;
        while (std::getline(lines,line)) {
            std::istringstream row(line); const auto type = word(row);
            if (type == "I" && !boot_seen && saves_.empty()) {
                const auto key=unhex(word(row)), value=unhex(word(row));
                if (!recorded_identity_.emplace(key,value).second) throw std::runtime_error("duplicate identity");
            } else if (type == "A" && !boot_seen && saves_.empty() && anchor_.empty()) {
                anchor_state_=unhex(word(row));
                const auto bytes=unhex(word(row));
                if (bytes.empty() || bytes.size()>limit/4) throw std::runtime_error("invalid anchor size");
                anchor_.assign(bytes.begin(),bytes.end());
            } else if (type == "S" && !boot_seen) {
                InitialSave s; s.id=unhex(word(row)); s.kind=number<std::uint32_t>(row); s.seat=number<std::uint32_t>(row); s.slot=number<std::uint32_t>(row); s.erase=number<std::uint32_t>(row);
                const auto bytes=unhex(word(row)); s.bytes.assign(bytes.begin(),bytes.end()); saves_.push_back(std::move(s));
                if (saves_.size()>64) throw std::runtime_error("too many save regions");
            } else if (type == "B" || type == "F") {
                const auto n=number<std::uint64_t>(row);
                if ((type == "B" && (boot_seen || n)) || (type == "F" && (!boot_seen || n != frames_.size()+1))) throw std::runtime_error("field order mismatch");
                Field f; f.width=number<std::uint32_t>(row); f.height=number<std::uint32_t>(row);
                const auto pads=number<unsigned>(row), clocks=number<unsigned>(row);
                if (pads>4096 || clocks>4096) throw std::runtime_error("too many callbacks");
                f.state=unhex(word(row)); f.image=unhex(word(row)); f.audio=unhex(word(row)); f.mark=unhex(word(row)); end_line(row);
                for(unsigned i=0;i<pads;++i) {
                    if (!std::getline(lines,line)) throw std::runtime_error("missing pad");
                    std::istringstream p(line); if(word(p)!="P") throw std::runtime_error("missing pad");
                    Input v{}; v.seat=number<std::uint32_t>(p); v.pad.struct_size=sizeof v.pad; v.pad.connected=number<std::uint32_t>(p); v.pad.buttons=number<std::uint32_t>(p);
                    if(v.seat>=RCORE_MAX_SEATS || v.pad.connected>1) throw std::runtime_error("invalid seat/connection");
                    for(auto& a:v.pad.axes) a=number<std::int16_t>(p); end_line(p); f.inputs.push_back(v);
                }
                for(unsigned i=0;i<clocks;++i) {
                    if(!std::getline(lines,line)) throw std::runtime_error("missing clock");
                    std::istringstream t(line); if(word(t)!="T") throw std::runtime_error("missing clock"); f.clocks.push_back(number<std::uint64_t>(t)); end_line(t);
                }
                if(type=="B") { boot_=std::move(f); boot_seen=true; } else frames_.push_back(std::move(f));
                continue;
            } else throw std::runtime_error("unknown or misplaced record");
            end_line(row);
        }
        if(!boot_seen || frames_.empty() || count!=frames_.size()) throw std::runtime_error("field count mismatch");
        return true;
    } catch(const std::exception& e) { return fail(std::string("route refused: ")+e.what()); }
}

void Route::write_verdict(const char* result) {
    const fs::path path=args_.verdict.empty() ? out_/"route-verdict.json" : args_.verdict;
    const auto target=fs::absolute(path).lexically_normal();
    for(const auto& source:{args_.record,args_.play,partial_})
        if(!source.empty() && target==fs::absolute(source).lexically_normal()) { fail("verdict path would overwrite a route"); return; }
    std::ofstream v(path,std::ios::binary);
    v << "{\n  \"schema\": 1,\n  \"result\": " << json(result) << ",\n  \"frames\": " << next_
      << ",\n  \"first_divergence\": " << (diverged_ ? std::to_string(next_+1) : "null")
      << ",\n  \"takeover_frame\": " << (taken_over_ ? std::to_string(next_+1) : "null")
      << ",\n  \"detail\": " << json(error_) << ",\n  \"recorded_core\": " << json(playback() ? recorded_identity_["core"] : identity_["core"])
      << ",\n  \"expected_state\": " << json(expected_state_)
      << ",\n  \"actual_state\": " << json(actual_state_)
      << ",\n  \"player_core\": " << json(identity_["core"])
      << ",\n  \"recorded_runner\": " << json(playback() ? recorded_identity_["runner"] : identity_["runner"])
      << ",\n  \"player_runner\": " << json(identity_["runner"])
      << ",\n  \"runner_reference_supplied\": " << (!args_.runner_reference.empty() ? "true" : "false")
      << ",\n  \"core_change_allowed\": " << (args_.allow_core_change ? "true" : "false")
      << ",\n  \"added_default_options\": [";
    for(std::size_t i=0;i<added_defaults_.size();++i) { if(i)v << ',';v << json(added_defaults_[i]); }
    v << "]\n}\n";
    if(!v) fail("cannot write route verdict");
}

bool Route::live_input(const rcore_pad pads[RCORE_MAX_SEATS]) {
    // The frontend already maps controls and applies its configured deadzone.
    // Its raw-axis fallback of 8000 also rejects residual noise here; this
    // affects takeover detection only, never the input delivered after it.
    for (unsigned seat=0;seat<RCORE_MAX_SEATS;++seat) {
        if (!pads[seat].connected) continue;
        if (pads[seat].buttons) return true;
        for (auto axis:pads[seat].axes) if (axis>8000 || axis<-8000) return true;
    }
    return false;
}

bool Route::finish(bool success, bool stopped, bool taken_over) {
    if(finished_) return error_.empty(); finished_=true;
    taken_over_ = taken_over;
    const bool user_stop = (stopped || taken_over) && playback() && !done();
    if(!success && error_.empty()) fail("core or session failed");
    if(!next_ && !user_stop && error_.empty()) fail("no complete field was recorded or played");
    if(playback() && !done() && !user_stop && error_.empty()) fail("playback stopped before the route ended");
    if(!playback() && error_.empty()) {
        file_.close();
        const auto digest=file_sha256_hex(partial_);
        std::ofstream tail(partial_,std::ios::binary|std::ios::app);
        tail << "END " << next_ << ' ' << digest << '\n'; tail.close();
        std::error_code ec;
        if(digest.empty() || !tail) fail("cannot seal route");
        else { fs::rename(partial_,args_.record,ec); if(ec) fail("cannot publish completed route: "+ec.message()); }
    }
    write_verdict(error_.empty() ? (taken_over ? "taken_over" : user_stop ? "stopped" : playback() ? "in_sync" : "recorded") : diverged_ ? "diverged" : "failed");
    return error_.empty();
}

std::uint32_t Route::width() const { return playback() && next_<frames_.size() ? frames_[next_].width : 0; }
std::uint32_t Route::height() const { return playback() && next_<frames_.size() ? frames_[next_].height : 0; }
} // namespace retro::runner
