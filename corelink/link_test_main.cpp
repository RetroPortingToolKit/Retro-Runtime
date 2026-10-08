// retro-core-link-test -- drives a core through the hub's link, the way the
// hub does, and writes what headless mode writes. If the link is correct, its
// artifacts are byte-identical to `retro-core-runner` headless and to
// n64lle's rcore_probe on the same core and scenario (docs/CORE_LINK.md).
//
// Same flags as headless mode (a subset: no --replay-at; --package included),
// plus --runner <path to retro-core-runner>, and savestates the way a host's
// menu takes them (link 1.1):
//   --state-save-at K:PATH   after frame K, save an envelope to PATH
//   --state-load-at K:PATH   after frame K, load the envelope at PATH
// Each prints one `state:` line with the runner's answer. Accessory data
// (link 2.1) the way a hub sends it:
//   --vruN                        the VRU microphone on seat N (1-4)
//   --accessory-send K:SEAT:SLOT:BYTES   before grant K, send BYTES to the
//                                 accessory at (SEAT, SLOT); the core sees
//                                 them in frame K
// Every message an accessory sends back prints as
//   accessory: notify SEAT SLOT BYTES
// in arrival order, as soon as the pump that carried it returns.
// The last picture's
// first byte is printed (`picture:`): the fake core paints frame k as k & 0xff,
// so it shows where a load landed.
// Outputs in --out: core.log, events.tsv and state_hash.tsv come from the
// runner's session dir, which is --out; shot.ppm is the last picture taken
// from shared memory; summary.txt is grepped from core.log.

#include "core_link.hpp"
#include "runner_probe.hpp"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <optional>
#include <string>
#include <utility>
#include <vector>

using namespace retro::corelink;

namespace {

[[noreturn]] void die(const std::string& m) {
    std::fprintf(stderr, "retro-core-link-test: %s\n", m.c_str());
    std::exit(2);
}

std::uint32_t script_buttons(const std::string& s) {
    std::uint32_t b = 0;
    size_t start = 0;
    while (start <= s.size()) {
        const size_t plus = s.find('+', start);
        const std::string t =
            s.substr(start, plus == std::string::npos ? std::string::npos : plus - start);
        if (t.empty() || t == "-" || t == "none") {
        } else if (t == "a") b |= RCORE_PAD_SOUTH;
        else if (t == "b") b |= RCORE_PAD_WEST;
        else if (t == "z") b |= RCORE_PAD_L2;
        else if (t == "l") b |= RCORE_PAD_L1;
        else if (t == "r") b |= RCORE_PAD_R1;
        else if (t == "start") b |= RCORE_PAD_START;
        else if (t == "dup") b |= RCORE_PAD_DPAD_UP;
        else if (t == "ddown") b |= RCORE_PAD_DPAD_DOWN;
        else if (t == "dleft") b |= RCORE_PAD_DPAD_LEFT;
        else if (t == "dright") b |= RCORE_PAD_DPAD_RIGHT;
        else die("--input-script: unknown button '" + t + "'");
        if (plus == std::string::npos) break;
        start = plus + 1;
    }
    return b;
}

} // namespace

int main(int argc, char** argv) {
    // UTF-8 on every OS: Windows' argv is the ANSI code page (transport.hpp).
    const std::vector<std::string> args = retro::corelink::utf8_args(argc, argv);
    argc = static_cast<int>(args.size());
    LaunchSpec spec;
    spec.gl = false;
    std::uint64_t frames = 60;
    bool seat0 = true;
    std::vector<std::pair<std::uint64_t, std::uint32_t>> script;
    struct StateOp {
        std::uint64_t at;
        bool save;
        std::string path;
    };
    std::vector<StateOp> state_ops;
    struct AccessorySend {
        std::uint64_t before_frame;
        std::uint32_t seat, slot;
        std::string bytes;
    };
    std::vector<AccessorySend> sends;
    auto state_op = [&](const std::string& v, bool save) {
        const auto colon = v.find(':');
        if (colon == std::string::npos) die("--state-save-at / --state-load-at K:PATH");
        state_ops.push_back({std::strtoull(v.substr(0, colon).c_str(), nullptr, 10), save,
                             v.substr(colon + 1)});
    };
    for (int i = 1; i < argc; ++i) {
        const std::string a = args[i];
        auto val = [&]() -> std::string {
            if (i + 1 >= argc) die(a + " needs a value");
            return args[++i];
        };
        if (a == "--probe") { // what probe_runner reads from a runner, then exit
            RunnerVersion v;
            std::string err;
            if (!probe_runner(utf8_path(val()), v, &err)) die(err);
            std::printf("probe: version %s, link %u.%u, rcore ABI %u, %s, game_package %u, "
                        "describe %u, accessory_data %u\n",
                        v.version.c_str(), v.link_major, v.link_minor, v.abi_major,
                        v.compatible() ? "compatible" : "NOT compatible with this host",
                        v.game_package, v.describe, v.accessory_data);
            return 0;
        } else if (a == "--runner") spec.runner = utf8_path(val());
        else if (a == "--core") spec.core = utf8_path(val());
        else if (a == "--rom") spec.rom = val();
        else if (a == "--package") spec.package = val();
        else if (a == "--title-dir") spec.title_dir = utf8_path(val());
        else if (a == "--out") spec.session_dir = utf8_path(val());
        else if (a == "--frames") frames = std::strtoull(val().c_str(), nullptr, 10);
        else if (a == "--load-state") spec.load_state = utf8_path(val());
        else if (a.size() >= 11 && a.compare(0, 6, "--tpak") == 0 && a[6] >= '1' && a[6] <= '4') {
            const std::size_t seat = static_cast<std::size_t>(a[6] - '1');
            const std::string id = "tpak" + std::string(1, a[6]);
            const std::string what = a.substr(7);
            if (what == "-rom") spec.tpak_roms[seat] = val();
            else if (what == "-save") spec.save_files[id] = utf8_path(val());
            else if (what == "-rtc") spec.save_files[id + ".rtc"] = utf8_path(val());
            else die("unknown argument " + a);
        }
        else if (a.size() == 6 && a.compare(0, 5, "--vru") == 0 && a[5] >= '1' && a[5] <= '4') {
            spec.vru_seats[static_cast<std::size_t>(a[5] - '1')] = true;
        } else if (a == "--accessory-send") { // K:SEAT:SLOT:BYTES, BYTES may hold colons
            const std::string v = val();
            const auto c1 = v.find(':');
            const auto c2 = c1 == std::string::npos ? c1 : v.find(':', c1 + 1);
            const auto c3 = c2 == std::string::npos ? c2 : v.find(':', c2 + 1);
            if (c3 == std::string::npos) die("--accessory-send K:SEAT:SLOT:BYTES");
            sends.push_back({std::strtoull(v.substr(0, c1).c_str(), nullptr, 10),
                             static_cast<std::uint32_t>(std::strtoul(v.substr(c1 + 1, c2 - c1 - 1).c_str(), nullptr, 10)),
                             static_cast<std::uint32_t>(std::strtoul(v.substr(c2 + 1, c3 - c2 - 1).c_str(), nullptr, 10)),
                             v.substr(c3 + 1)});
        }
        else if (a == "--save") { // <region id>=<file>
            const std::string kv = val();
            const auto eq = kv.find('=');
            if (eq == std::string::npos) die("--save region=file");
            spec.save_files[kv.substr(0, eq)] = utf8_path(kv.substr(eq + 1));
        } else if (a == "--env") spec.env.push_back(val()); // NAME=value, for the runner
        else if (a == "--gl") spec.gl = true;
        else if (a == "--strict") spec.strict = true;
        else if (a == "--no-seats") seat0 = false;
        else if (a == "--opt") {
            const std::string kv = val();
            const auto eq = kv.find('=');
            if (eq == std::string::npos) die("--opt key=value");
            spec.options[kv.substr(0, eq)] = kv.substr(eq + 1);
        } else if (a == "--input-script") {
            const std::string s = val();
            size_t start = 0;
            while (start <= s.size()) {
                const size_t comma = s.find(',', start);
                const std::string ev = s.substr(
                    start, comma == std::string::npos ? std::string::npos : comma - start);
                const auto colon = ev.find(':');
                if (colon == std::string::npos) die("--input-script F:buttons,...");
                script.emplace_back(std::strtoull(ev.substr(0, colon).c_str(), nullptr, 10),
                                    script_buttons(ev.substr(colon + 1)));
                if (comma == std::string::npos) break;
                start = comma + 1;
            }
        } else if (a == "--state-save-at") {
            state_op(val(), true);
        } else if (a == "--state-load-at") {
            state_op(val(), false);
        } else if (a == "--replay-at") {
            die("--replay-at: not over the link yet (needs the savestate envelope)");
        } else {
            die("unknown argument " + a);
        }
    }
    if (spec.runner.empty() || spec.core.empty() || spec.rom.empty()) {
        die("--runner, --core and --rom are required");
    }
    if (spec.title_dir.empty()) spec.title_dir = ".";
    if (spec.session_dir.empty()) spec.session_dir = ".";

    // Before frame 1, the seats stand as frame 1 will have them.
    if (seat0) {
        spec.initial_pads[0].connected = 1;
        for (const auto& [at, buttons] : script) {
            if (at <= 1) spec.initial_pads[0].buttons = buttons;
        }
    }

    CoreLink link;
    std::string err;
    if (!link.start(spec, &err)) die(err);
    while (link.state() == LinkState::Starting) link.pump(100);
    if (link.state() != LinkState::Ready) {
        die("the runner ended before it was ready (exit " + std::to_string(link.exit_code()) +
            (link.exit_reason().empty() ? "" : ": " + link.exit_reason()) + "); see " +
            link.runner_log().string());
    }
    const CoreIdentity& id = link.identity();
    std::printf("link: %s %s sha256 %s draft %u%s\n", id.core_id.c_str(), id.core_version.c_str(),
                id.sha256.c_str(), id.draft_revision, id.engine_dirty ? " (engine dirty)" : "");

    // What the accessories said, printed as soon as the pump that carried it
    // returns, so the order against `state:` lines is the order on the wire.
    auto print_notifies = [&] {
        while (const auto n = link.poll_accessory_notify()) {
            std::printf("accessory: notify %u %u %.*s\n", n->seat, n->slot,
                        static_cast<int>(n->bytes.size()),
                        reinterpret_cast<const char*>(n->bytes.data()));
        }
    };

    // ---- the same frames headless mode runs, one grant each -----------------
    bool ok = true;
    for (std::uint64_t k = 1; k <= frames && ok; ++k) {
        for (const AccessorySend& snd : sends) {
            if (snd.before_frame != k) continue;
            if (!link.send_accessory(snd.seat, snd.slot, snd.bytes.data(), snd.bytes.size())) {
                std::printf("accessory: send before frame %llu not sent (link %u.%u, core %s "
                            "accessory_data)\n",
                            static_cast<unsigned long long>(k), kProtocolMajor,
                            link.identity().protocol_minor,
                            (link.identity().capabilities & RCORE_CAP_ACCESSORY_DATA) ? "declares"
                                                                                      : "lacks");
            }
        }
        rcore_pad pads[RCORE_MAX_SEATS]{};
        for (auto& p : pads) p.struct_size = sizeof(rcore_pad);
        if (seat0) {
            pads[0].connected = 1;
            for (const auto& [at, buttons] : script) {
                if (at <= k) pads[0].buttons = buttons;
            }
        }
        if (!link.grant(pads)) die("grant " + std::to_string(k) + " refused");
        while (link.state() == LinkState::Ready && link.frames_done() < k) link.pump(100);
        print_notifies();
        if (link.state() != LinkState::Ready) ok = false;
        link.take_frame();
        std::int16_t sink[4096];
        while (link.drain_audio(sink, 2048)) {
        }
        for (const StateOp& op : state_ops) {
            if (op.at != k || !ok) continue;
            const bool sent = op.save ? link.request_save_state(utf8_path(op.path))
                                      : link.request_load_state(utf8_path(op.path));
            if (!sent) {
                std::printf("state: %s at frame %llu not sent (link %u.%u, core %s savestate)\n",
                            op.save ? "save" : "load", static_cast<unsigned long long>(k),
                            kProtocolMajor, link.identity().protocol_minor,
                            (link.identity().capabilities & RCORE_CAP_SAVESTATE) ? "declares"
                                                                                 : "lacks");
                continue;
            }
            std::optional<StateResult> r;
            while (!(r = link.take_state_result())) link.pump(100);
            std::printf("state: %s at frame %llu %s%s%s\n", op.save ? "save" : "load",
                        static_cast<unsigned long long>(k), r->ok ? "ok" : "failed",
                        r->ok ? (" (" + std::to_string(r->bytes) + " bytes)").c_str() : ": ",
                        r->ok ? "" : r->detail.c_str());
            if (link.state() != LinkState::Ready) ok = false;
        }
    }
    for (const LinkLog& l : link.logs) {
        if (l.level <= RCORE_LOG_WARN) std::fprintf(stderr, "core[%u]: %s\n", l.level, l.text.c_str());
    }
    std::uint32_t faults = 0;
    for (const LinkEvent& e : link.events) {
        if (e.kind == RCORE_EVENT_FAULT) {
            ++faults;
            std::fprintf(stderr, "core FAULT: %s\n", e.detail.c_str());
        }
    }
    link.stop();
    if (link.exit_code() != 0) {
        ok = false;
        std::fprintf(stderr, "retro-core-link-test: runner exit %d%s%s\n", link.exit_code(),
                     link.exit_reason().empty() ? "" : ": ", link.exit_reason().c_str());
    }

    // ---- the artifacts headless mode writes ----------------------------------
    if (const FrameInfo* f = link.frame_info()) {
        std::ofstream shot(spec.session_dir / "shot.ppm", std::ios::binary);
        shot << "P6\n" << f->width << ' ' << f->height << "\n255\n";
        const std::uint8_t* px = link.frame_pixels();
        for (std::size_t i = 0; i < std::size_t(f->width) * f->height; ++i) {
            shot.write(reinterpret_cast<const char*>(px + i * 4), 3);
        }
    }
    {
        std::ifstream log(spec.session_dir / "core.log");
        std::ofstream sm(spec.session_dir / "summary.txt");
        std::string line;
        while (std::getline(log, line)) {
            const auto tab = line.find('\t');
            const std::string msg = tab == std::string::npos ? line : line.substr(tab + 1);
            if (!msg.compare(0, 9, "RUN_DONE ") || !msg.compare(0, 11, "RDRAM_HASH ") ||
                !msg.compare(0, 15, "DISPATCH_STATS ")) {
                sm << msg << '\n';
            }
        }
    }
    if (const FrameInfo* f = link.frame_info(); f && f->width && link.frame_pixels()) {
        std::printf("picture: first byte %u\n", unsigned(link.frame_pixels()[0]));
    }
    // 2.2: what the overlay's readout is made of.
    std::printf("frame stats: game %llu work %s\n",
                static_cast<unsigned long long>(link.frame_stats().game_frame),
                link.frame_stats().work_ns ? "measured" : "none");
    std::printf("link-test: %llu frame(s) granted and done, runner exit %d, %u fault(s)\n",
                static_cast<unsigned long long>(link.frames_done()), link.exit_code(), faults);
    return (ok && !faults) ? 0 : 1;
}
